// kl_shc — Metal shader-compile census and warm-up (round F3, hla).
// Measurements and the decision this rests on: hla/FOVEATION-F3.md.
//
// MoltenVK turns SPIR-V into MSL inside vkCreateGraphicsPipelines and then asks
// Metal for three things, each a compile that can block the calling thread:
//   newLibraryWithSource:options:completionHandler:     MSL -> AIR (front end)
//   newFunctionWithName:constantValues:completionHandler: specialisation
//   newRenderPipelineStateWithDescriptor:completionHandler: AIR -> GPU (back end)
// (the async variants, each followed by a wait; MoltenVK uses them so it can
// time out). Metal keeps an on-disk cache of both the libraries and the GPU
// binaries, so the second launch that needs a shader pays almost nothing; the
// first one pays everything, synchronously, at the guest's first draw.
//
// Part 1 — CENSUS (always on when installed): every one of those entry points
// (plus compute pipelines and depth-stencil states) is timed per category, per
// calling thread (async ones from the call to the completion handler, named
// after the thread that started them but charged to the all-thread totals only:
// the handler may run after that thread is gone, so it never touches its TLS),
// summed per [perf] second, and compiles slower than KL_SHC_SLOW_MS (default
// 50) are named with their thread. kl_vulkan reads the all-thread totals around
// each vkCreateGraphicsPipelines to split a slow pipeline into Metal's share
// and MoltenVK's own (SPIRV-Cross).
//
// The swizzles sit OUTSIDE kl_fove's (installed after it): a library source
// seen here is MoltenVK's own, unpatched, and a compile issued from here goes
// through kl_fove's patch exactly like the guest's.
// (Metal classes are looked up by name: the test binaries link the runtime
// without Metal.framework.)
#include "kl_shc.h"
#include "kl_env.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>
#include <dispatch/dispatch.h>
#include <pthread/qos.h>

#if __has_feature(objc_arc)
#define KS_RELEASE(x) ((void)0)
#else
#define KS_RELEASE(x) [(x) release]
#endif

#define KSI(...) fprintf(stderr, "  [shc] " __VA_ARGS__)

static const char *const k_cat[KL_SHC_N] = { "lib", "fn", "rps", "cps", "dss" };

static int g_on;
static id<MTLDevice> g_dev;
static int g_slow_ms = 50, g_trace = 1;

// ---------------------------------------------------------------- clocks
static uint64_t ks_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct ks_nm { char s[32]; };
// Part 2's state the census line reads (defined below).
static const char *g_record;
static volatile int g_warm_on, g_workers_live;
static void ks_warm_report(const char *why);
static __thread uint64_t t_ns[KL_SHC_N];
static __thread int t_depth;           // inside one of our timed calls on this thread
static __thread int t_warm;            // this thread is a warm-up worker

static struct {
    uint64_t ns[KL_SHC_N], n[KL_SHC_N], worst[KL_SHC_N];
} g_tot, g_win;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_win_top[32];             // the thread that compiled longest this window
static uint64_t g_win_top_ns;
static uint32_t g_slow_said;

void kl_shc_thread_ns(uint64_t out[KL_SHC_N]) {
    for (int i = 0; i < KL_SHC_N; i++) out[i] = t_ns[i];
}

void kl_shc_total_ns(uint64_t out[KL_SHC_N]) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < KL_SHC_N; i++) out[i] = g_tot.ns[i];
    pthread_mutex_unlock(&g_mu);
}

// Seconds since the census was installed: the slow lines carry it so a hitch in
// the [perf] stream can be matched to the compiles inside it.
static uint64_t g_t0;
double kl_shc_clock(void) { return g_t0 ? (double)(ks_now() - g_t0) / 1e9 : 0.0; }

static void ks_charge(int cat, uint64_t ns, uint64_t *acc, const char *thread, const char *what) {
    if (acc) __atomic_add_fetch(&acc[cat], ns, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_mu);
    g_tot.ns[cat] += ns; g_tot.n[cat]++; if (ns > g_tot.worst[cat]) g_tot.worst[cat] = ns;
    g_win.ns[cat] += ns; g_win.n[cat]++; if (ns > g_win.worst[cat]) g_win.worst[cat] = ns;
    if (thread && ns > g_win_top_ns) { g_win_top_ns = ns; snprintf(g_win_top, sizeof g_win_top, "%s", thread); }
    int say = ns >= (uint64_t)g_slow_ms * 1000000ull && g_slow_said < 400;
    if (say) g_slow_said++;
    pthread_mutex_unlock(&g_mu);
    if (say) KSI("t=%.2f slow %s %.1f ms on %s%s%s\n", kl_shc_clock(), k_cat[cat], (double)ns / 1e6,
                 thread && *thread ? thread : "?", what ? " " : "", what ? what : "");
}

// "<name>#<tid % 100000>", the [perf] line's spelling.
void kl_shc_thread_label(char out[32]) {
    char nm[24] = "";
    uint64_t tid = 0;
    pthread_getname_np(pthread_self(), nm, sizeof nm);
    pthread_threadid_np(NULL, &tid);
    snprintf(out, 32, "%s#%llu", nm, (unsigned long long)(tid % 100000));
}
static void ks_thread_name(char out[32]) { kl_shc_thread_label(out); }

void kl_shc_perf_field(char *out, unsigned cap) {
    if (!cap) return;
    out[0] = 0;
    if (!g_on) return;
    pthread_mutex_lock(&g_mu);
    uint64_t any = 0;
    for (int i = 0; i < KL_SHC_N; i++) any += g_win.n[i];
    int o = snprintf(out, cap, " | t=%.0f", kl_shc_clock());
    if (any) {
        o += snprintf(out + o, cap - (unsigned)o, " shc");
        for (int i = 0; i < KL_SHC_N && o < (int)cap - 32; i++)
            if (g_win.n[i])
                o += snprintf(out + o, cap - (unsigned)o, " %s %llu/%.0fms(max %.0f)", k_cat[i],
                              (unsigned long long)g_win.n[i], (double)g_win.ns[i] / 1e6,
                              (double)g_win.worst[i] / 1e6);
        if (o < (int)cap - 40) snprintf(out + o, cap - (unsigned)o, " top %s", g_win_top);
    }
    memset(&g_win, 0, sizeof g_win);
    g_win_top[0] = 0; g_win_top_ns = 0;
    pthread_mutex_unlock(&g_mu);
    static int every;
    if ((g_warm_on || g_record) && ++every % 10 == 0) ks_warm_report(g_workers_live ? "running" : "state");
}

// ---------------------------------------------------------------- swizzles
#define KS_SW_N 32
static struct { Class cls; SEL sel; IMP orig; } g_sw[KS_SW_N];
static int g_nsw;

static IMP ks_orig(id self, SEL sel) {
    int n = __atomic_load_n(&g_nsw, __ATOMIC_ACQUIRE);
    for (Class c = object_getClass(self); c; c = class_getSuperclass(c))
        for (int i = 0; i < n; i++)
            if (g_sw[i].cls == c && g_sw[i].sel == sel) return g_sw[i].orig;
    return NULL;
}

static int ks_swap(Class c, const char *name, IMP mine) {
    SEL s = sel_registerName(name);
    Method m = class_getInstanceMethod(c, s);
    if (!m) return 0;
    for (int i = 0; i < g_nsw; i++) if (g_sw[i].cls == c && g_sw[i].sel == s) return 1;
    if (g_nsw >= KS_SW_N) return 0;
    g_sw[g_nsw].cls = c; g_sw[g_nsw].sel = s; g_sw[g_nsw].orig = method_getImplementation(m);
    __atomic_store_n(&g_nsw, g_nsw + 1, __ATOMIC_RELEASE);
    class_replaceMethod(c, s, mine, method_getTypeEncoding(m));
    return 1;
}

static uint64_t ks_fnv(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 1099511628211ull; }
    return h;
}

// A library compile seen (not a warm-up's): hash + size + time, one line each
// when KL_SHC_TRACE (default 1) — the set of hashes is what says whether the
// host's MSL is the headset's.
static void ks_lib_seen(NSString *src, uint64_t ns, const char *thread, int async) {
    if (g_trace < 1 || !src) return;
    const char *s = src.UTF8String;
    if (!s) return;
    size_t n = strlen(s);
    static uint32_t seq;
    uint32_t k = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
    KSI("t=%.2f lib #%u %016llx %zu B %.1f ms %s%s\n", kl_shc_clock(), k, (unsigned long long)ks_fnv(s, n), n,
        (double)ns / 1e6, async ? "async " : "", thread);
}


// ================================================================ Part 2 — RECORD + WARM
//
// RECORD (KL_SHC_RECORD=<file>; the app points it into its container): every
// library source a real compile asks for that the file does not hold yet is
// appended — MoltenVK's MSL (unpatched; zlib), the compile options as text, and
// the map being spawned (from Valve's "SV:  Spawn Server:" line). One file per
// device; it never leaves the device.
//
// WARM (KL_SHC_WARM=1, the default when a list exists): at install the record
// and the SHIPPED list (KL_SHC_SHIPPED=<file>, generated on the Mac by touring
// the maps: hla/recon/native-F3/tour.sh + mkwarm.py) are loaded, and once the
// guest's first real compile shows the options this device uses, background
// threads (QoS utility) compile every entry with those options through the
// swizzled entry point — i.e. through kl_fove's patch when foveation is on —
// so Metal's on-disk library cache already holds them when the guest asks.
// Entries tagged with the map that is loading go first. A warm thread holds off
// while a real compile is in flight, so it never queues the guest behind it.
// Safety: a warm compile's result is released unused; a failed one is counted;
// if 40 real compiles found NOTHING from the shipped list, the shipped list is
// taken not to match this device's MSL and its warming stops.

#define KS_MAGIC "KLSHW001"
typedef struct {
    uint64_t key;
    const uint8_t *z;              // zlib'd MSL (in a loaded file buffer)
    uint32_t zlen, rawlen;
    const char *opt, *maps;        // NUL-terminated (in the buffer)
    uint8_t origin;                // 1 local record, 2 shipped
    uint8_t kind;                  // 1 library (z = zlib'd MSL), 2 pipeline state (z = raw blob)
    volatile uint8_t done;         // warmed, being warmed, or the guest asked first
} ks_ent;

static ks_ent *g_ent;
static uint32_t g_nent, g_cap_ent;
// key -> entry index + 1 (0 empty), or KS_SEEN for a key only seen live.
#define KS_TAB 32768u
#define KS_SEEN 0xffffffffu
static struct { uint64_t key; uint32_t v; } g_tab[KS_TAB];
static pthread_mutex_t g_wmu = PTHREAD_MUTEX_INITIALIZER;

static dispatch_queue_t g_recq;
static char g_map[64];             // the map Valve is loading/playing
static volatile uint32_t g_map_gen;
static char g_live_opt[512];
// A copy of the guest's own MTLCompileOptions (taken with g_live_opt): what the
// warm compiles with, so anything the text cannot carry (preprocessor macros,
// options this list does not know) is still the guest's.
static id g_live_obj;
static volatile int g_live_known, g_real_inflight, g_stop_shipped;
static struct {
    uint64_t warmed, warm_ns, failed, skipped_opt, skipped_shipped, opt_bad;
    uint64_t real, hit_warm, hit_early, miss, hit_shipped, recorded;
    uint64_t pwarmed, pwarm_ns, pfailed, pskipped;              // pipeline states
    uint64_t preal, phit_warm, phit_early, pmiss, punrec, precorded;
} g_w;

static const char *const k_optkeys[] = {
    "languageVersion", "fastMathEnabled", "mathMode", "mathFloatingPointFunctions",
    "preserveInvariance", "optimizationLevel", "libraryType", "enableLogging",
    "allowReferencingUndefinedSymbols", "compileSymbolVisibility", "maxTotalThreadsPerThreadgroup",
};

static void ks_opts_text(id o, char *out, size_t cap) {
    if (!o) { snprintf(out, cap, "nil"); return; }
    size_t w = 0;
    out[0] = 0;
    for (unsigned i = 0; i < sizeof k_optkeys / sizeof *k_optkeys && w < cap - 64; i++) {
        if (![o respondsToSelector:sel_registerName(k_optkeys[i])]) continue;
        id v = nil;
        @try { v = [o valueForKey:[NSString stringWithUTF8String:k_optkeys[i]]]; } @catch (id ex) { v = nil; }
        if ([v isKindOfClass:[NSNumber class]])
            w += (size_t)snprintf(out + w, cap - w, "%s=%lld;", k_optkeys[i], [(NSNumber *)v longLongValue]);
    }
    NSDictionary *m = [o respondsToSelector:@selector(preprocessorMacros)] ? [o preprocessorMacros] : nil;
    if (m.count && w < cap - 32) w += (size_t)snprintf(out + w, cap - w, "macros=%lu;", (unsigned long)m.count);
}

// nil + 1 when the text cannot be rebuilt exactly (macros); nil + 0 for "nil".
static id ks_opts_make(const char *t, int *bad) {
    *bad = 0;
    if (!strcmp(t, "nil")) return nil;
    if (strstr(t, "macros=")) { *bad = 1; return nil; }
    MTLCompileOptions *o = [(Class)objc_getClass("MTLCompileOptions") new];
    const char *p = t;
    while (*p) {
        const char *eq = strchr(p, '='), *sc = eq ? strchr(eq, ';') : NULL;
        if (!eq || !sc) break;
        char k[64];
        size_t kl = (size_t)(eq - p);
        if (kl < sizeof k) {
            memcpy(k, p, kl); k[kl] = 0;
            long long v = strtoll(eq + 1, NULL, 10);
            for (unsigned i = 0; i < sizeof k_optkeys / sizeof *k_optkeys; i++)
                if (!strcmp(k, k_optkeys[i]) && [o respondsToSelector:sel_registerName(k_optkeys[i])]) {
                    @try { [o setValue:@(v) forKey:[NSString stringWithUTF8String:k]]; } @catch (id ex) { *bad = 1; }
                }
        }
        p = sc + 1;
    }
    return o;
}

// The options a warm compile uses: the guest's own (copied) once known, else
// rebuilt from the entry's text (KL_SHC_WARM_SYNC). *own = caller releases.
// NULL + *bad when they cannot be rebuilt (macros) — counted, said once.
static id ks_warm_opts(const char *entry_opt, int *own, int *bad) {
    *own = 0; *bad = 0;
    if (__atomic_load_n(&g_live_known, __ATOMIC_ACQUIRE) && (g_live_obj || !strcmp(g_live_opt, "nil"))) return g_live_obj;
    id o = ks_opts_make(__atomic_load_n(&g_live_known, __ATOMIC_ACQUIRE) ? g_live_opt : entry_opt, bad);
    *own = 1;
    if (*bad) {
        static int said;
        __atomic_add_fetch(&g_w.opt_bad, 1, __ATOMIC_RELAXED);
        if (!said++) KSI("WARNING: compile options \"%s\" cannot be rebuilt (preprocessor macros) — "
                         "these warm compiles are SKIPPED (counted as 'options unbuildable')\n",
                         g_live_known ? g_live_opt : entry_opt);
    }
    return o;
}

// A library's key is its source alone (FNV-1a 64): one device compiles every
// library with one option set, and a list recorded elsewhere is warmed with
// THIS device's options (the guest's own requests are what a warm must hit).
static uint64_t ks_key(const char *src, size_t n, const char *opt) {
    (void)opt;
    return ks_fnv(src, n);
}

// g_wmu held.
static uint32_t *ks_slot(uint64_t key) {
    uint32_t h = (uint32_t)(key ^ (key >> 29)) & (KS_TAB - 1);
    for (uint32_t i = 0; i < KS_TAB; i++, h = (h + 1) & (KS_TAB - 1)) {
        if (!g_tab[h].v) { g_tab[h].key = key; return &g_tab[h].v; }
        if (g_tab[h].key == key) return &g_tab[h].v;
    }
    return NULL;
}

// The record file is appended by a process that is never shut down — on the
// headset it is always killed, possibly mid-write — so a torn tail is normal,
// not corruption. Loading it as this device's record REPAIRS it in place: the
// file is truncated to its last whole entry (a short or bad header starts it
// over), so the next append lands where the loader will find it. An entry
// whose header fails a check (sizes, NULs, a pipeline state whose rawlen is
// not its zlen, lengths past KS_RAW_CAP) is the torn point. The shipped list
// is read-only: there a bad entry just ends the list.
#define KS_REC_CAP (256u << 20)    // the record stops growing here (logged once)
#define KS_RAW_CAP (64u << 20)     // no MSL / blob is anywhere near this
#define KS_LIST_MAX (512l << 20)
static int g_rec_fd = -1;
static uint64_t g_rec_size;        // the record's length (the writer's view)
static uint8_t *g_buf[2];          // the loaded files (entries point into them)

// The size of the entry at b + o, or 0 when it is torn or fails a check.
static size_t ks_entry_size(const uint8_t *b, size_t sz, size_t o, uint8_t *kind_out) {
    if (o + 24 > sz) return 0;
    uint32_t magic, rawlen, zlen; uint16_t optlen, mapslen;
    memcpy(&magic, b + o, 4);
    uint8_t kind = magic == 0x31544e45u ? 1 : magic == 0x31504e45u ? 2 : 0;   // "ENT1" / "ENP1"
    if (!kind) return 0;
    memcpy(&rawlen, b + o + 12, 4); memcpy(&zlen, b + o + 16, 4);
    memcpy(&optlen, b + o + 20, 2); memcpy(&mapslen, b + o + 22, 2);
    if (!rawlen || !zlen || rawlen > KS_RAW_CAP || zlen > KS_RAW_CAP || (kind == 2 && rawlen != zlen)) return 0;
    size_t need = 24 + (size_t)optlen + mapslen + zlen;
    if (o + need > sz || !optlen || !mapslen || b[o + 24 + optlen - 1] || b[o + 24 + optlen + mapslen - 1]) return 0;
    *kind_out = kind;
    return need;
}

// Load one list file into the entry table (origin 1 = this device's record,
// repaired in place; 2 = the shipped list). Returns entries added.
static uint32_t ks_load(const char *path, uint8_t origin) {
    if (!path || !*path) return 0;
    int rec = origin == 1;
    int fd = open(path, (rec ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    long sz = fstat(fd, &st) ? -1 : (long)st.st_size;
    uint8_t *b = (sz >= 8 && sz < KS_LIST_MAX) ? malloc((size_t)sz) : NULL;
    size_t got = 0;
    while (b && got < (size_t)sz) {
        ssize_t r = pread(fd, b + got, (size_t)sz - got, (off_t)got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (!b || got != (size_t)sz || memcmp(b, KS_MAGIC, 8)) {
        int read_err = b && got != (size_t)sz;
        free(b);
        if (rec && sz > 0 && !read_err && !ftruncate(fd, 0)) {
            KSI("warm list %s (this device's record): short or bad header (%ld B) — started over\n", path, sz);
            g_rec_size = 0;
        } else if (sz != 0) {
            KSI("warm list %s: not a list (bad magic or size) — ignored\n", path);
        }
        close(fd);
        return 0;
    }
    uint32_t added = 0, dup = 0;
    size_t o = 8, need;
    uint8_t kind = 0;
    pthread_mutex_lock(&g_wmu);
    while ((need = ks_entry_size(b, (size_t)sz, o, &kind))) {
        uint64_t key; uint32_t rawlen, zlen; uint16_t optlen, mapslen;
        memcpy(&key, b + o + 4, 8); memcpy(&rawlen, b + o + 12, 4); memcpy(&zlen, b + o + 16, 4);
        memcpy(&optlen, b + o + 20, 2); memcpy(&mapslen, b + o + 22, 2);
        uint32_t *sl = ks_slot(key);
        if (sl && *sl && *sl != KS_SEEN) {
            g_ent[*sl - 1].origin |= origin;
            dup++;
        } else if (sl) {
            if (g_nent == g_cap_ent) {
                g_cap_ent = g_cap_ent ? g_cap_ent * 2 : 1024;
                g_ent = realloc(g_ent, g_cap_ent * sizeof *g_ent);
            }
            ks_ent *e = &g_ent[g_nent++];
            memset(e, 0, sizeof *e);
            e->key = key; e->rawlen = rawlen; e->zlen = zlen; e->origin = origin; e->kind = kind;
            e->opt = (const char *)b + o + 24;
            e->maps = (const char *)b + o + 24 + optlen;
            e->z = b + o + 24 + optlen + mapslen;
            *sl = g_nent;
            added++;
        }
        o += need;
    }
    pthread_mutex_unlock(&g_wmu);
    int torn = o < (size_t)sz, cut = 0;
    if (torn && rec) cut = !ftruncate(fd, (off_t)o);
    if (rec) g_rec_size = cut || !torn ? o : (uint64_t)sz;
    close(fd);
    free(g_buf[origin - 1]);
    g_buf[origin - 1] = b;
    uint32_t np = 0;
    for (uint32_t i = 0; i < g_nent; i++) np += g_ent[i].kind == 2 && (g_ent[i].origin & origin);
    KSI("warm list %s (%s): %u entries (%u pipeline states), %u already listed, %ld KiB%s", path,
        origin == 2 ? "shipped" : "this device's record", added, np, dup, sz >> 10,
        !torn ? "\n" : !rec ? " — corrupt tail ignored\n" : "");
    if (torn && rec)
        fprintf(stderr, " — torn tail (killed mid-write) at %zu: %s, %ld B dropped\n", o,
                cut ? "truncated there" : "TRUNCATE FAILED", sz - (long)o);
    return added;
}

// Free the loaded files once nothing will read an entry's payload again (the
// warm threads are done, or never ran): keys, kinds and done-flags stay — the
// classification of guest compiles needs only those.
static void ks_release_payloads(void) {
    pthread_mutex_lock(&g_wmu);
    for (uint32_t i = 0; i < g_nent; i++) { g_ent[i].z = NULL; g_ent[i].opt = g_ent[i].maps = NULL; }
    for (int i = 0; i < 2; i++) { free(g_buf[i]); g_buf[i] = NULL; }
    pthread_mutex_unlock(&g_wmu);
}

// Open the record for appending (O_APPEND; created with its magic when empty).
static int ks_rec_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return -1; }
    g_rec_size = (uint64_t)st.st_size;
    if (!st.st_size) {
        if (write(fd, KS_MAGIC, 8) != 8) { (void)ftruncate(fd, 0); close(fd); return -1; }
        g_rec_size = 8;
    }
    return fd;
}

// One entry, header + payload, in ONE write(): a kill leaves at most one torn
// entry at the tail, which the next load cuts off. 1 written, 0 not (cap / error).
static int ks_rec_write(int fd, uint32_t magic, uint64_t key, uint32_t rawlen, const void *z, uint32_t zlen,
                        const char *op, const char *mp) {
    static int said_cap, said_err;
    uint16_t optlen = (uint16_t)(strlen(op) + 1), mapslen = (uint16_t)(strlen(mp) + 1);
    size_t n = 24 + (size_t)optlen + mapslen + zlen;
    if (g_rec_size + n > KS_REC_CAP) {
        if (!said_cap++) KSI("record reached %u MiB: no longer recording (warming continues)\n", KS_REC_CAP >> 20);
        return 0;
    }
    uint8_t *b = malloc(n);
    if (!b) return 0;
    memcpy(b, &magic, 4); memcpy(b + 4, &key, 8); memcpy(b + 12, &rawlen, 4); memcpy(b + 16, &zlen, 4);
    memcpy(b + 20, &optlen, 2); memcpy(b + 22, &mapslen, 2);
    memcpy(b + 24, op, optlen); memcpy(b + 24 + optlen, mp, mapslen); memcpy(b + 24 + optlen + mapslen, z, zlen);
    ssize_t w = write(fd, b, n);
    free(b);
    if (w != (ssize_t)n) {
        if (w > 0) (void)ftruncate(fd, (off_t)g_rec_size);      // take a short write back off
        if (!said_err++) KSI("record write failed (%zd of %zu B) — entry dropped\n", w, n);
        return 0;
    }
    g_rec_size += n;
    return 1;
}

static void ks_record_entry(uint32_t magic, uint64_t key, const void *data, size_t n, const char *opt, int zip) {
    if (!g_record || !g_recq || g_rec_fd < 0) return;
    char *s = malloc(n), *op = strdup(opt);
    char maps[80];
    pthread_mutex_lock(&g_wmu);
    snprintf(maps, sizeof maps, ",%s,", g_map[0] ? g_map : "-");
    pthread_mutex_unlock(&g_wmu);
    char *mp = strdup(maps);
    if (!s || !op || !mp) { free(s); free(op); free(mp); return; }
    memcpy(s, data, n);
    dispatch_async(g_recq, ^{
        uLongf zl = zip ? compressBound((uLong)n) : (uLongf)n;
        uint8_t *z = malloc(zl);
        int ok = z && (zip ? compress2(z, &zl, (const Bytef *)s, (uLong)n, 6) == Z_OK : (memcpy(z, s, n), 1));
        if (ok && n <= KS_RAW_CAP && zl <= KS_RAW_CAP &&
            ks_rec_write(g_rec_fd, magic, key, (uint32_t)n, z, (uint32_t)zl, op, mp))
            __atomic_add_fetch(magic == 0x31544e45u ? &g_w.recorded : &g_w.precorded, 1, __ATOMIC_RELAXED);
        free(z); free(s); free(op); free(mp);
    });
}

static void ks_record(const char *src, size_t n, const char *opt) {
    ks_record_entry(0x31544e45u, ks_key(src, n, opt), src, n, opt, 1);
}

// A real (guest) library compile is about to run: classify it against the
// lists, record it if new, and hold the warm threads off while it runs.
static uint64_t ks_real_begin(NSString *src, id opts) {
    __atomic_add_fetch(&g_real_inflight, 1, __ATOMIC_ACQ_REL);
    if (!g_warm_on && !g_record) return 0;
    const char *s = src.UTF8String;
    if (!s) return 0;
    size_t n = strlen(s);
    char opt[512];
    ks_opts_text(opts, opt, sizeof opt);
    if (!g_live_known) {
        pthread_mutex_lock(&g_wmu);
        if (!g_live_known) {
            snprintf(g_live_opt, sizeof g_live_opt, "%s", opt);
            id cp = nil;
            @try { cp = [opts copy]; } @catch (id ex) { cp = nil; }
            g_live_obj = cp;
            __atomic_store_n(&g_live_known, 1, __ATOMIC_RELEASE);
            KSI("this device compiles with options: %s%s\n", opt,
                !opts ? "" : cp ? " (warm compiles with a copy of them)"
                               : " — COPY FAILED: the warm rebuilds them from this text");
        }
        pthread_mutex_unlock(&g_wmu);
    }
    uint64_t key = ks_key(s, n, opt);
    int fresh = 0;
    pthread_mutex_lock(&g_wmu);
    g_w.real++;
    uint32_t *sl = ks_slot(key);
    if (sl && *sl && *sl != KS_SEEN) {
        ks_ent *e = &g_ent[*sl - 1];
        if (e->done == 2) g_w.hit_warm++;
        else { g_w.hit_early++; e->done = 3; }
        if (e->origin & 2) g_w.hit_shipped++;
    } else {
        g_w.miss++;
        if (sl && !*sl) { *sl = KS_SEEN; fresh = 1; }
    }
    if (!g_stop_shipped && g_w.real >= 40 && !g_w.hit_shipped) {
        int any_shipped = 0;
        for (uint32_t i = 0; i < g_nent && !any_shipped; i++) any_shipped = g_ent[i].origin & 2;
        if (any_shipped) {
            g_stop_shipped = 1;
            KSI("40 real compiles and NONE was in the shipped list: its MSL does not match this "
                "device's — shipped warming stopped (this device's own record still warms)\n");
        }
    }
    pthread_mutex_unlock(&g_wmu);
    if (fresh) ks_record(s, n, opt);
    return key;
}
static void ks_real_end(void) { __atomic_sub_fetch(&g_real_inflight, 1, __ATOMIC_ACQ_REL); }

// ---------------------------------------------------------------- pipeline states
//
// The library warm leaves the back-end compile (newRenderPipelineState: AIR ->
// GPU code for this descriptor's formats, blending and vertex fetch) to the
// guest's first draw. To warm that too, a real pipeline state is recorded as
// what rebuilds it: each stage's library (by list key), function name and
// specialisation constants (captured from MTLFunctionConstantValues' setters),
// plus the descriptor's state as text. Warming one = library (a cache hit by
// then), function, descriptor, newRenderPipelineStateWithDescriptor:, released.
static char kAssocLib, kAssocFn, kAssocCv;

static uint32_t ks_type_size(NSUInteger t) {
    switch (t) {
    case 3: case 29: case 33: return 4;          // float, int, uint
    case 16: case 37: case 41: return 2;         // half, short, ushort
    case 45: case 49: case 53: return 1;         // char, uchar, bool
    case 81: case 85: return 8;                  // long, ulong
    default: return 0;
    }
}

// The constant log on a MTLFunctionConstantValues: mode u8 (0 index, 1 name,
// 2 range), type u32, index/start u32, count u32, name len u16, name, data.
// A type we cannot size poisons it (length 0xffffffff marker).
static NSMutableData *ks_cv_log(id cv, int make) {
    NSMutableData *d = objc_getAssociatedObject(cv, &kAssocCv);
    if (!d && make) {
        d = [NSMutableData data];
        objc_setAssociatedObject(cv, &kAssocCv, d, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return d;
}
static void ks_cv_add(id cv, uint8_t mode, NSUInteger type, uint32_t idx, uint32_t count, const char *name, const void *v) {
    NSMutableData *d = ks_cv_log(cv, 1);
    uint32_t sz = ks_type_size(type);
    if (!sz || !v) { uint32_t bad = 0xffffffffu; [d appendBytes:&bad length:4]; return; }
    uint32_t t = (uint32_t)type;
    uint16_t nl = name ? (uint16_t)strlen(name) : 0;
    [d appendBytes:&mode length:1]; [d appendBytes:&t length:4]; [d appendBytes:&idx length:4];
    [d appendBytes:&count length:4]; [d appendBytes:&nl length:2];
    if (nl) [d appendBytes:name length:nl];
    [d appendBytes:v length:(NSUInteger)sz * count];
}
static void ks_cv_idx(id self, SEL cmd, const void *v, NSUInteger type, NSUInteger idx) {
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))ks_orig(self, cmd))(self, cmd, v, type, idx);
    ks_cv_add(self, 0, type, (uint32_t)idx, 1, NULL, v);
}
static void ks_cv_range(id self, SEL cmd, const void *v, NSUInteger type, NSRange r) {
    ((void (*)(id, SEL, const void *, NSUInteger, NSRange))ks_orig(self, cmd))(self, cmd, v, type, r);
    ks_cv_add(self, 2, type, (uint32_t)r.location, (uint32_t)r.length, NULL, v);
}
static void ks_cv_name(id self, SEL cmd, const void *v, NSUInteger type, NSString *name) {
    ((void (*)(id, SEL, const void *, NSUInteger, id))ks_orig(self, cmd))(self, cmd, v, type, name);
    ks_cv_add(self, 1, type, 0, 1, name.UTF8String, v);
}
static void ks_cv_reset(id self, SEL cmd) {
    ((void (*)(id, SEL))ks_orig(self, cmd))(self, cmd);
    objc_setAssociatedObject(self, &kAssocCv, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static int ks_cv_replay(id cv, const uint8_t *p, size_t n) {
    size_t o = 0;
    while (o < n) {
        if (o + 4 <= n && !memcmp(p + o, "\xff\xff\xff\xff", 4)) return 0;
        if (o + 15 > n) return 0;
        uint8_t mode = p[o]; uint32_t t, idx, count; uint16_t nl;
        memcpy(&t, p + o + 1, 4); memcpy(&idx, p + o + 5, 4); memcpy(&count, p + o + 9, 4); memcpy(&nl, p + o + 13, 2);
        o += 15;
        uint32_t sz = ks_type_size(t);
        if (!sz || o + nl + (size_t)sz * count > n) return 0;
        const char *nm = (const char *)p + o; o += nl;
        const void *v = p + o; o += (size_t)sz * count;
        if (mode == 0) [cv setConstantValue:v type:(MTLDataType)t atIndex:idx];
        else if (mode == 2) [cv setConstantValues:v type:(MTLDataType)t withRange:NSMakeRange(idx, count)];
        else {
            NSString *s = [[NSString alloc] initWithBytes:nm length:nl encoding:NSUTF8StringEncoding];
            [cv setConstantValue:v type:(MTLDataType)t withName:s];
            KS_RELEASE(s);
        }
    }
    return 1;
}

// A function's rebuild record: lib key u64, name len u16, name, const len u32, consts.
static void ks_fn_note(id lib, id fn, NSString *name, id cv) {
    if (!fn || !lib || (!g_warm_on && !g_record)) return;
    NSNumber *lk = objc_getAssociatedObject(lib, &kAssocLib);
    if (!lk) return;
    NSData *c = cv ? ks_cv_log(cv, 0) : nil;
    const char *nm = name.UTF8String;
    if (!nm) return;
    uint64_t k = lk.unsignedLongLongValue;
    uint16_t nl = (uint16_t)strlen(nm);
    uint32_t cl = c ? (uint32_t)c.length : 0;
    NSMutableData *r = [NSMutableData dataWithCapacity:16 + nl + cl];
    [r appendBytes:&k length:8]; [r appendBytes:&nl length:2]; [r appendBytes:nm length:nl];
    [r appendBytes:&cl length:4];
    if (cl) [r appendData:c];
    objc_setAssociatedObject(fn, &kAssocFn, r, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static const char *const k_dsc[] = { "rasterSampleCount", "alphaToCoverageEnabled", "alphaToOneEnabled",
    "rasterizationEnabled", "inputPrimitiveTopology", "depthAttachmentPixelFormat",
    "stencilAttachmentPixelFormat", "maxVertexAmplificationCount", "supportIndirectCommandBuffers" };
static const char *const k_dca[] = { "pixelFormat", "writeMask", "blendingEnabled", "sourceRGBBlendFactor",
    "destinationRGBBlendFactor", "rgbBlendOperation", "sourceAlphaBlendFactor",
    "destinationAlphaBlendFactor", "alphaBlendOperation" };
static const char *const k_dla[] = { "stride", "stepFunction", "stepRate" };
static const char *const k_dat[] = { "format", "offset", "bufferIndex" };
#define KS_N(a) (sizeof a / sizeof *a)

static long long ks_kv(id o, const char *k, int *ok) {
    if (!o || ![o respondsToSelector:sel_registerName(k)]) { *ok = 0; return 0; }
    id v = nil;
    @try { v = [o valueForKey:[NSString stringWithUTF8String:k]]; } @catch (id ex) { v = nil; }
    if (![v isKindOfClass:[NSNumber class]]) { *ok = 0; return 0; }
    *ok = 1;
    return [(NSNumber *)v longLongValue];
}

// The descriptor's compile-relevant state as "k=v;" text; 0 when it does not fit.
static int ks_desc_text(MTLRenderPipelineDescriptor *d, char *out, size_t cap) {
    size_t w = 0;
    int ok;
#define KS_PUT(...) do { int n_ = snprintf(out + w, cap - w, __VA_ARGS__); if (n_ < 0 || (size_t)n_ >= cap - w) return 0; w += (size_t)n_; } while (0)
    for (unsigned i = 0; i < KS_N(k_dsc); i++) { long long v = ks_kv(d, k_dsc[i], &ok); if (ok) KS_PUT("s.%s=%lld;", k_dsc[i], v); }
    for (unsigned c = 0; c < 8; c++) {
        id ca = d.colorAttachments[c];
        if (!ks_kv(ca, "pixelFormat", &ok)) continue;
        for (unsigned i = 0; i < KS_N(k_dca); i++) { long long v = ks_kv(ca, k_dca[i], &ok); if (ok) KS_PUT("c%u.%s=%lld;", c, k_dca[i], v); }
    }
    MTLVertexDescriptor *vd = d.vertexDescriptor;
    for (unsigned b = 0; vd && b < 31; b++) {
        id l = vd.layouts[b];
        if (!ks_kv(l, "stride", &ok)) continue;
        for (unsigned i = 0; i < KS_N(k_dla); i++) { long long v = ks_kv(l, k_dla[i], &ok); if (ok) KS_PUT("l%u.%s=%lld;", b, k_dla[i], v); }
    }
    for (unsigned a = 0; vd && a < 31; a++) {
        id at = vd.attributes[a];
        if (!ks_kv(at, "format", &ok)) continue;
        for (unsigned i = 0; i < KS_N(k_dat); i++) { long long v = ks_kv(at, k_dat[i], &ok); if (ok) KS_PUT("a%u.%s=%lld;", a, k_dat[i], v); }
    }
    for (unsigned b = 0; b < 31; b++) {
        long long v = (long long)d.vertexBuffers[b].mutability;
        if (v) KS_PUT("vb%u=%lld;", b, v);
        v = (long long)d.fragmentBuffers[b].mutability;
        if (v) KS_PUT("fb%u=%lld;", b, v);
    }
#undef KS_PUT
    return 1;
}

static void ks_desc_apply(MTLRenderPipelineDescriptor *d, const char *t) {
    for (const char *p = t; *p;) {
        const char *eq = strchr(p, '='), *sc = eq ? strchr(eq, ';') : NULL;
        if (!eq || !sc) break;
        char k[64];
        size_t kl = (size_t)(eq - p);
        long long v = strtoll(eq + 1, NULL, 10);
        if (kl < sizeof k) {
            memcpy(k, p, kl); k[kl] = 0;
            id obj = nil;
            const char *key = NULL;
            unsigned ix = 0;
            if (!strncmp(k, "s.", 2)) { obj = d; key = k + 2; }
            else if (k[0] == 'c' && sscanf(k, "c%u.", &ix) == 1 && ix < 8) { obj = d.colorAttachments[ix]; key = strchr(k, '.') + 1; }
            else if (k[0] == 'l' && sscanf(k, "l%u.", &ix) == 1 && ix < 31) { obj = d.vertexDescriptor.layouts[ix]; key = strchr(k, '.') + 1; }
            else if (k[0] == 'a' && sscanf(k, "a%u.", &ix) == 1 && ix < 31) { obj = d.vertexDescriptor.attributes[ix]; key = strchr(k, '.') + 1; }
            else if (!strncmp(k, "vb", 2) && sscanf(k, "vb%u", &ix) == 1 && ix < 31) d.vertexBuffers[ix].mutability = (MTLMutability)v;
            else if (!strncmp(k, "fb", 2) && sscanf(k, "fb%u", &ix) == 1 && ix < 31) d.fragmentBuffers[ix].mutability = (MTLMutability)v;
            if (obj && key && [obj respondsToSelector:sel_registerName(key)])
                @try { [obj setValue:@(v) forKey:[NSString stringWithUTF8String:key]]; } @catch (id ex) { }
        }
        p = sc + 1;
    }
}

// A real pipeline state is about to be built: classify + record (when its
// functions carry rebuild records). Returns 1 when it counted as in flight.
static void ks_pso_real(MTLRenderPipelineDescriptor *d) {
    __atomic_add_fetch(&g_real_inflight, 1, __ATOMIC_ACQ_REL);
    if ((!g_warm_on && !g_record) || ![d isKindOfClass:(Class)objc_getClass("MTLRenderPipelineDescriptor")]) return;
    NSData *vr = d.vertexFunction ? objc_getAssociatedObject(d.vertexFunction, &kAssocFn) : nil;
    NSData *fr = d.fragmentFunction ? objc_getAssociatedObject(d.fragmentFunction, &kAssocFn) : nil;
    char desc[6144];
    if (!vr || (d.fragmentFunction && !fr) || !ks_desc_text(d, desc, sizeof desc)) {
        pthread_mutex_lock(&g_wmu); g_w.punrec++; pthread_mutex_unlock(&g_wmu);
        return;
    }
    uint16_t vl = (uint16_t)vr.length, fl = (uint16_t)fr.length;
    size_t dl = strlen(desc) + 1;
    size_t n = 2 + vl + 2 + fl + dl;
    uint8_t *b = malloc(n);
    if (!b) return;
    memcpy(b, &vl, 2); memcpy(b + 2, vr.bytes, vl);
    memcpy(b + 2 + vl, &fl, 2); if (fl) memcpy(b + 4 + vl, fr.bytes, fl);
    memcpy(b + 4 + vl + fl, desc, dl);
    uint64_t key = ks_fnv((const char *)b, n) * 0xff51afd7ed558ccdull + 1;
    int fresh = 0;
    pthread_mutex_lock(&g_wmu);
    g_w.preal++;
    uint32_t *sl = ks_slot(key);
    if (sl && *sl && *sl != KS_SEEN) {
        ks_ent *e = &g_ent[*sl - 1];
        if (e->done == 2) g_w.phit_warm++;
        else { g_w.phit_early++; e->done = 3; }
    } else {
        g_w.pmiss++;
        if (sl && !*sl) { *sl = KS_SEEN; fresh = 1; }
    }
    pthread_mutex_unlock(&g_wmu);
    if (fresh) ks_record_entry(0x31504e45u, key, b, n, "pso", 0);
    free(b);
}

// Warm one pipeline-state entry. 1 done, 0 skipped (a library it needs is not
// listed or has other options), -1 failed.
static id ks_warm_lib(uint64_t key) {
    pthread_mutex_lock(&g_wmu);
    uint32_t *sl = ks_slot(key);
    ks_ent *e = sl && *sl && *sl != KS_SEEN ? &g_ent[*sl - 1] : NULL;
    pthread_mutex_unlock(&g_wmu);
    if (!e || e->kind != 1 || !e->z) return nil;
    uLongf rl = e->rawlen;
    char *raw = malloc((size_t)e->rawlen + 1);
    if (!raw || uncompress((Bytef *)raw, &rl, e->z, e->zlen) != Z_OK || rl != e->rawlen) { free(raw); return nil; }
    raw[rl] = 0;
    NSString *src = [[NSString alloc] initWithBytesNoCopy:raw length:rl encoding:NSUTF8StringEncoding freeWhenDone:YES];
    int bad, own;
    id opts = ks_warm_opts(e->opt, &own, &bad);
    id lib = (src && !bad) ? [g_dev newLibraryWithSource:src options:opts error:NULL] : nil;
    if (own) KS_RELEASE(opts);
    KS_RELEASE(src);
    return lib;                    // +1
}

static id ks_warm_fn(const uint8_t *r, size_t n) {
    if (n < 14) return nil;
    uint64_t k; uint16_t nl; uint32_t cl;
    memcpy(&k, r, 8); memcpy(&nl, r + 8, 2);
    if (10 + (size_t)nl + 4 > n) return nil;
    memcpy(&cl, r + 10 + nl, 4);
    if (14 + (size_t)nl + cl > n) return nil;
    id lib = ks_warm_lib(k);
    if (!lib) return nil;
    NSString *name = [[NSString alloc] initWithBytes:r + 10 length:nl encoding:NSUTF8StringEncoding];
    id fn = nil;
    if (cl) {
        MTLFunctionConstantValues *cv = [(Class)objc_getClass("MTLFunctionConstantValues") new];
        if (ks_cv_replay(cv, r + 14 + nl, cl)) fn = [lib newFunctionWithName:name constantValues:cv error:NULL];
        KS_RELEASE(cv);
    } else {
        fn = [lib newFunctionWithName:name];
    }
    KS_RELEASE(name);
    KS_RELEASE(lib);
    return fn;                     // +1
}

static int ks_warm_pso(const ks_ent *e) {
    const uint8_t *b = e->z;
    size_t n = e->rawlen;
    if (!b || n < 5 || n != e->zlen) return -1;
    uint16_t vl, fl;
    memcpy(&vl, b, 2);
    if (2 + (size_t)vl + 2 > n) return -1;
    memcpy(&fl, b + 2 + vl, 2);
    if (4 + (size_t)vl + fl + 1 > n || b[n - 1]) return -1;
    id vf = ks_warm_fn(b + 2, vl);
    if (!vf) return 0;
    id ff = fl ? ks_warm_fn(b + 4 + vl, fl) : nil;
    if (fl && !ff) { KS_RELEASE(vf); return 0; }
    MTLRenderPipelineDescriptor *d = [(Class)objc_getClass("MTLRenderPipelineDescriptor") new];
    d.vertexFunction = vf;
    d.fragmentFunction = ff;
    // A fresh descriptor's vertexDescriptor is non-nil on macOS 27 (checked);
    // should one ever start nil, the layouts/attributes below would be dropped
    // silently and the warmed state would not be the guest's.
    if (!d.vertexDescriptor) d.vertexDescriptor = [(Class)objc_getClass("MTLVertexDescriptor") vertexDescriptor];
    ks_desc_apply(d, (const char *)b + 4 + vl + fl);
    id pso = nil;
    @try { pso = [g_dev newRenderPipelineStateWithDescriptor:d error:NULL]; } @catch (id ex) { pso = nil; }
    int r = pso ? 1 : -1;
    KS_RELEASE(pso);
    KS_RELEASE(d);
    KS_RELEASE(vf);
    KS_RELEASE(ff);
    return r;
}


void kl_shc_note_map(const char *name) {
    if (!name || !*name) return;
    pthread_mutex_lock(&g_wmu);
    snprintf(g_map, sizeof g_map, "%.*s", (int)strcspn(name, "\" \t\r\n"), name);
    pthread_mutex_unlock(&g_wmu);
    __atomic_add_fetch(&g_map_gen, 1, __ATOMIC_RELEASE);
    if (g_on) KSI("t=%.2f map %s is spawning%s\n", kl_shc_clock(), g_map,
                  g_warm_on ? ": its listed shaders warm first" : "");
}

// The next entry to warm: one tagged with the loading map first, then list order.
static uint32_t g_cursor;
static int ks_pick(void) {
    char pat[80] = "";
    pthread_mutex_lock(&g_wmu);
    if (g_map[0]) snprintf(pat, sizeof pat, ",%s,", g_map);
    int pick = -1;
    if (pat[0])
        for (uint32_t i = 0; i < g_nent; i++)
            if (!g_ent[i].done && g_ent[i].maps && strstr(g_ent[i].maps, pat) &&
                !(g_stop_shipped && g_ent[i].origin == 2)) { pick = (int)i; break; }
    while (pick < 0 && g_cursor < g_nent) {
        uint32_t i = g_cursor++;
        if (g_ent[i].done) continue;
        if (g_stop_shipped && g_ent[i].origin == 2) { g_w.skipped_shipped++; continue; }
        pick = (int)i;
    }
    if (pick >= 0) g_ent[pick].done = 1;
    pthread_mutex_unlock(&g_wmu);
    return pick;
}

static void ks_warm_report(const char *why) {
    KSI("t=%.2f warm %s: %llu of %u entries compiled (%.1f s of compile, %llu failed, %llu options unbuildable, %llu recorded with other options (warmed with this device's), "
        "%llu shipped skipped); guest compiles %llu: %llu served warm, %llu asked before the warm reached them, "
        "%llu not listed (%llu recorded) | pipeline states: %llu warmed (%.1f s), %llu skipped, %llu failed; "
        "guest %llu: %llu served warm, %llu early, %llu not listed (%llu recorded), %llu unrecordable\n",
        kl_shc_clock(), why,
        (unsigned long long)g_w.warmed, g_nent, (double)g_w.warm_ns / 1e9, (unsigned long long)g_w.failed, (unsigned long long)g_w.opt_bad,
        (unsigned long long)g_w.skipped_opt, (unsigned long long)g_w.skipped_shipped,
        (unsigned long long)g_w.real, (unsigned long long)g_w.hit_warm, (unsigned long long)g_w.hit_early,
        (unsigned long long)g_w.miss, (unsigned long long)g_w.recorded,
        (unsigned long long)g_w.pwarmed, (double)g_w.pwarm_ns / 1e9, (unsigned long long)g_w.pskipped,
        (unsigned long long)g_w.pfailed, (unsigned long long)g_w.preal, (unsigned long long)g_w.phit_warm,
        (unsigned long long)g_w.phit_early, (unsigned long long)g_w.pmiss, (unsigned long long)g_w.precorded,
        (unsigned long long)g_w.punrec);
}

// KL_SHC_WARM_SYNC=1 (measurement only): warm the whole list at install, with
// each entry's own options, BEFORE the guest goes on — "the warm had all the
// time it needed", the bound on what warming can buy.
static int g_sync, g_pso_warm = 1, g_yield = 1;
static void *ks_warm_main(void *arg) {
    (void)arg;
    pthread_setname_np("kl-shader-warm");
    t_warm = 1;
    while (!g_sync && !__atomic_load_n(&g_live_known, __ATOMIC_ACQUIRE)) usleep(20000);
    for (;;) {
        // Not in front of the guest: wait out a real compile (bounded).
        // KL_SHC_WARM_YIELD=0 keeps warming through it; on the host that bought
        // nothing (8 threads neither): Metal's compiler service is the limit.
        for (int w = 0; g_yield && __atomic_load_n(&g_real_inflight, __ATOMIC_ACQUIRE) > 0 && w < 3000; w++) usleep(1000);
        int i = ks_pick();
        if (i < 0) break;
        ks_ent *e = &g_ent[i];
        if (e->kind == 2) {
            if (!g_pso_warm) { e->done = 2; continue; }
            uint64_t t0 = ks_now();
            int r;
            @autoreleasepool { r = ks_warm_pso(e); }
            __atomic_add_fetch(&g_w.pwarm_ns, ks_now() - t0, __ATOMIC_RELAXED);
            __atomic_add_fetch(r > 0 ? &g_w.pwarmed : r == 0 ? &g_w.pskipped : &g_w.pfailed, 1, __ATOMIC_RELAXED);
            if (e->done == 1) e->done = 2;
            continue;
        }
        if (!e->z) { if (e->done == 1) e->done = 2; continue; }
        if (g_live_known && strcmp(e->opt, g_live_opt)) __atomic_add_fetch(&g_w.skipped_opt, 1, __ATOMIC_RELAXED);
        @autoreleasepool {
            uLongf rl = e->rawlen;
            char *raw = malloc((size_t)e->rawlen + 1);
            int bad = 0, own = 0;
            if (raw && uncompress((Bytef *)raw, &rl, e->z, e->zlen) == Z_OK && rl == e->rawlen) {
                raw[rl] = 0;
                NSString *src = [[NSString alloc] initWithBytesNoCopy:raw length:rl encoding:NSUTF8StringEncoding freeWhenDone:YES];
                raw = NULL;
                id opts = ks_warm_opts(e->opt, &own, &bad);
                if (src && !bad) {
                    NSError *err = nil;
                    uint64_t t0 = ks_now();
                    id lib = [g_dev newLibraryWithSource:src options:opts error:&err];
                    __atomic_add_fetch(&g_w.warm_ns, ks_now() - t0, __ATOMIC_RELAXED);
                    if (lib) __atomic_add_fetch(&g_w.warmed, 1, __ATOMIC_RELAXED);
                    else __atomic_add_fetch(&g_w.failed, 1, __ATOMIC_RELAXED);
                    KS_RELEASE(lib);
                } else if (!bad) __atomic_add_fetch(&g_w.failed, 1, __ATOMIC_RELAXED);
                if (own) KS_RELEASE(opts);
                KS_RELEASE(src);
            } else __atomic_add_fetch(&g_w.failed, 1, __ATOMIC_RELAXED);
            free(raw);
        }
        if (e->done == 1) e->done = 2;
    }
    if (__atomic_sub_fetch(&g_workers_live, 1, __ATOMIC_ACQ_REL) == 0) {
        ks_warm_report("done");
        ks_release_payloads();       // the lists' bytes are not read again
    }
    return NULL;
}

static void ks_warm_start(void) {
    g_record = kl_env_str("KL_SHC_RECORD", NULL);
    if (g_record && !*g_record) g_record = NULL;
    ks_load(g_record, 1);            // repairs a torn tail BEFORE anything appends
    if (g_record) {
        g_rec_fd = ks_rec_open(g_record);
        if (g_rec_fd < 0) KSI("record %s: cannot open for appending — not recording\n", g_record);
        else g_recq = dispatch_queue_create("kl-shader-record", DISPATCH_QUEUE_SERIAL);
    }
    ks_load(kl_env_str("KL_SHC_SHIPPED", NULL), 2);
    if (!g_nent || !kl_env_on("KL_SHC_WARM", 1)) {
        KSI("warm %s; record %s\n", !g_nent ? "off (no list)" : "off (KL_SHC_WARM=0)",
            g_record ? g_record : "off (KL_SHC_RECORD unset)");
        ks_release_payloads();
        return;
    }
    long ncomp = -1;
    SEL sc = sel_registerName("maximumConcurrentCompilationTaskCount");
    if ([(id)g_dev respondsToSelector:sc])
        ncomp = (long)((NSUInteger (*)(id, SEL))objc_msgSend)((id)g_dev, sc);
    int n = kl_env_int("KL_SHC_WARM_THREADS", 0);
    if (n <= 0) { n = ncomp > 1 ? (int)(ncomp - 1) : 2; if (n > 4) n = 4; }
    if (n > 8) n = 8;
    g_yield = kl_env_int("KL_SHC_WARM_YIELD", 1) != 0;
    g_sync = kl_env_on("KL_SHC_WARM_SYNC", 0);
    g_pso_warm = kl_env_on("KL_SHC_WARM_PSO", 1);
    g_warm_on = 1;
    g_workers_live = n;
    for (int i = 0; i < n; i++) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_set_qos_class_np(&a, QOS_CLASS_UTILITY, 0);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        if (pthread_create(&t, &a, ks_warm_main, NULL)) __atomic_sub_fetch(&g_workers_live, 1, __ATOMIC_ACQ_REL);
        pthread_attr_destroy(&a);
    }
    if (g_sync) {
        uint64_t t0 = ks_now();
        while (__atomic_load_n(&g_workers_live, __ATOMIC_ACQUIRE) > 0) usleep(5000);
        KSI("warm SYNC (KL_SHC_WARM_SYNC=1, measurement only): the guest waited %.1f s for the list\n",
            (double)(ks_now() - t0) / 1e9);
    }
    KSI("warm ON: %u entries, %d thread(s) at QoS utility (Metal compile tasks %ld), %s, starting at the "
        "guest's first compile; record %s\n", g_nent, n, ncomp,
        g_yield ? "yielding to guest compiles" : "not yielding (compile slots to spare)", g_record ? g_record : "off");
}

// --- timed sync calls: only the outermost on a thread counts.
#define KS_SYNC_BEGIN                                    \
    int outer_ = !t_depth++ && !t_warm;                   \
    uint64_t t0_ = outer_ ? ks_now() : 0;
#define KS_SYNC_END(cat, what)                           \
    t_depth--;                                           \
    if (outer_) {                                        \
        char nm_[32]; ks_thread_name(nm_);               \
        ks_charge(cat, ks_now() - t0_, t_ns, nm_, what); \
    }

static id ks_lib_err(id self, SEL cmd, NSString *src, id opts, NSError **err) {
    KS_SYNC_BEGIN
    uint64_t lk = outer_ ? ks_real_begin(src, opts) : 0;
    id r = ((id (*)(id, SEL, id, id, NSError **))ks_orig(self, cmd))(self, cmd, src, opts, err);
    if (lk && r) objc_setAssociatedObject(r, &kAssocLib, @(lk), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    t_depth--;
    if (outer_) {
        ks_real_end();
        char nm[32]; ks_thread_name(nm);
        uint64_t ns = ks_now() - t0_;
        ks_charge(KL_SHC_LIB, ns, t_ns, nm, NULL);
        ks_lib_seen(src, ns, nm, 0);
    }
    return r;
}

static void ks_lib_cb(id self, SEL cmd, NSString *src, id opts, id handler) {
    IMP o = ks_orig(self, cmd);
    if (t_depth || t_warm || !handler) { ((void (*)(id, SEL, id, id, id))o)(self, cmd, src, opts, handler); return; }
    uint64_t t0 = ks_now();
    struct ks_nm nm; ks_thread_name(nm.s);
    NSString *keep = src;
    uint64_t lk = ks_real_begin(src, opts);
    void (^h)(id, NSError *) = handler;
    void (^mine)(id, NSError *) = ^(id lib, NSError *e) {
        ks_real_end();
        if (lk && lib) objc_setAssociatedObject(lib, &kAssocLib, @(lk), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        uint64_t ns = ks_now() - t0;
        ks_charge(KL_SHC_LIB, ns, NULL, nm.s, NULL);
        ks_lib_seen(keep, ns, nm.s, 1);
        h(lib, e);
    };
    ((void (*)(id, SEL, id, id, id))o)(self, cmd, src, opts, mine);
}

static id ks_fn(id self, SEL cmd, NSString *name) {
    KS_SYNC_BEGIN
    id r = ((id (*)(id, SEL, id))ks_orig(self, cmd))(self, cmd, name);
    KS_SYNC_END(KL_SHC_FN, NULL)
    if (outer_) ks_fn_note(self, r, name, nil);
    return r;
}
static id ks_fn_cv_err(id self, SEL cmd, NSString *name, id cv, NSError **err) {
    KS_SYNC_BEGIN
    id r = ((id (*)(id, SEL, id, id, NSError **))ks_orig(self, cmd))(self, cmd, name, cv, err);
    KS_SYNC_END(KL_SHC_FN, "(specialised)")
    if (outer_) ks_fn_note(self, r, name, cv);
    return r;
}
static void ks_fn_cv_cb(id self, SEL cmd, id name, id cv, id handler) {
    IMP o = ks_orig(self, cmd);
    if (t_depth || t_warm || !handler) { ((void (*)(id, SEL, id, id, id))o)(self, cmd, name, cv, handler); return; }
    uint64_t t0 = ks_now();
    struct ks_nm nm; ks_thread_name(nm.s);
    void (^h)(id, NSError *) = handler;
    id lib = self;
    void (^mine)(id, NSError *) = ^(id fn, NSError *e) {
        ks_charge(KL_SHC_FN, ks_now() - t0, NULL, nm.s, "(specialised)");
        ks_fn_note(lib, fn, name, cv);
        h(fn, e);
    };
    ((void (*)(id, SEL, id, id, id))o)(self, cmd, name, cv, mine);
}

// One generic async wrapper shape per handler arity.
#define KS_ASYNC2(fn, cat, what, ARGDECL, ARGS)                                          \
    static void fn(id self, SEL cmd, ARGDECL, id handler) {                              \
        IMP o = ks_orig(self, cmd);                                                      \
        if (t_depth || t_warm || !handler) {                                             \
            ((void (*)(id, SEL, ARGDECL, id))o)(self, cmd, ARGS, handler); return;       \
        }                                                                                \
        uint64_t t0 = ks_now();                                             \
        struct ks_nm nm; ks_thread_name(nm.s);                                         \
        void (^h)(id, NSError *) = handler;                                              \
        void (^mine)(id, NSError *) = ^(id x, NSError *e) {                              \
            ks_charge(cat, ks_now() - t0, NULL, nm.s, what); h(x, e);                       \
        };                                                                               \
        ((void (*)(id, SEL, ARGDECL, id))o)(self, cmd, ARGS, mine);                      \
    }
#define KS_ASYNC3(fn, cat, what, ARGDECL, ARGS)                                          \
    static void fn(id self, SEL cmd, ARGDECL, id handler) {                              \
        IMP o = ks_orig(self, cmd);                                                      \
        if (t_depth || t_warm || !handler) {                                             \
            ((void (*)(id, SEL, ARGDECL, id))o)(self, cmd, ARGS, handler); return;       \
        }                                                                                \
        uint64_t t0 = ks_now();                                             \
        struct ks_nm nm; ks_thread_name(nm.s);                                         \
        void (^h)(id, id, NSError *) = handler;                                          \
        void (^mine)(id, id, NSError *) = ^(id x, id r, NSError *e) {                    \
            ks_charge(cat, ks_now() - t0, NULL, nm.s, what); h(x, r, e);                    \
        };                                                                               \
        ((void (*)(id, SEL, ARGDECL, id))o)(self, cmd, ARGS, mine);                      \
    }
#define COMMA ,
// Render pipeline states: timed, and (real ones) classified/recorded, holding
// the warm threads off while they compile.
static void ks_rps_cb(id self, SEL cmd, id d, id handler) {
    IMP o = ks_orig(self, cmd);
    if (t_depth || t_warm || !handler) { ((void (*)(id, SEL, id, id))o)(self, cmd, d, handler); return; }
    ks_pso_real(d);
    uint64_t t0 = ks_now();
    struct ks_nm nm; ks_thread_name(nm.s);
    void (^h)(id, NSError *) = handler;
    void (^mine)(id, NSError *) = ^(id x, NSError *e) {
        ks_real_end();
        ks_charge(KL_SHC_RPS, ks_now() - t0, NULL, nm.s, NULL); h(x, e);
    };
    ((void (*)(id, SEL, id, id))o)(self, cmd, d, mine);
}
static void ks_rps_opt_cb(id self, SEL cmd, id d, NSUInteger opt, id handler) {
    IMP o = ks_orig(self, cmd);
    if (t_depth || t_warm || !handler) { ((void (*)(id, SEL, id, NSUInteger, id))o)(self, cmd, d, opt, handler); return; }
    ks_pso_real(d);
    uint64_t t0 = ks_now();
    struct ks_nm nm; ks_thread_name(nm.s);
    void (^h)(id, id, NSError *) = handler;
    void (^mine)(id, id, NSError *) = ^(id x, id r, NSError *e) {
        ks_real_end();
        ks_charge(KL_SHC_RPS, ks_now() - t0, NULL, nm.s, NULL); h(x, r, e);
    };
    ((void (*)(id, SEL, id, NSUInteger, id))o)(self, cmd, d, opt, mine);
}
KS_ASYNC2(ks_cps_fn_cb, KL_SHC_CPS, NULL, id f, f)
KS_ASYNC3(ks_cps_fn_opt_cb, KL_SHC_CPS, NULL, id f COMMA NSUInteger opt, f COMMA opt)
KS_ASYNC3(ks_cps_desc_opt_cb, KL_SHC_CPS, NULL, id d COMMA NSUInteger opt, d COMMA opt)

static id ks_rps_err(id self, SEL cmd, id d, NSError **err) {
    KS_SYNC_BEGIN
    if (outer_) ks_pso_real(d);
    id r = ((id (*)(id, SEL, id, NSError **))ks_orig(self, cmd))(self, cmd, d, err);
    if (outer_) ks_real_end();
    KS_SYNC_END(KL_SHC_RPS, NULL)
    return r;
}
static id ks_rps_opt_err(id self, SEL cmd, id d, NSUInteger opt, id *refl, NSError **err) {
    KS_SYNC_BEGIN
    if (outer_) ks_pso_real(d);
    id r = ((id (*)(id, SEL, id, NSUInteger, id *, NSError **))ks_orig(self, cmd))(self, cmd, d, opt, refl, err);
    if (outer_) ks_real_end();
    KS_SYNC_END(KL_SHC_RPS, NULL)
    return r;
}
static id ks_cps_fn_err(id self, SEL cmd, id f, NSError **err) {
    KS_SYNC_BEGIN
    id r = ((id (*)(id, SEL, id, NSError **))ks_orig(self, cmd))(self, cmd, f, err);
    KS_SYNC_END(KL_SHC_CPS, NULL)
    return r;
}
static id ks_cps_opt_err(id self, SEL cmd, id f, NSUInteger opt, id *refl, NSError **err) {
    KS_SYNC_BEGIN
    id r = ((id (*)(id, SEL, id, NSUInteger, id *, NSError **))ks_orig(self, cmd))(self, cmd, f, opt, refl, err);
    KS_SYNC_END(KL_SHC_CPS, NULL)
    return r;
}
static id ks_dss(id self, SEL cmd, id d) {
    KS_SYNC_BEGIN
    id r = ((id (*)(id, SEL, id))ks_orig(self, cmd))(self, cmd, d);
    KS_SYNC_END(KL_SHC_DSS, NULL)
    return r;
}

void kl_shc_install(void *mtl_device) {
    static int done;
    if (done || !mtl_device) return;
    done = 1;
    // OFF by default on driver (it swizzles MTLDevice's compile entry points):
    // not yet confirmed on a headset. A port opts in with KL_SHC=1.
    if (!kl_env_on("KL_SHC", 0)) return;
    g_dev = (__bridge id<MTLDevice>)mtl_device;
    g_t0 = ks_now();
    g_slow_ms = kl_env_int("KL_SHC_SLOW_MS", 50);
    g_trace = kl_env_int("KL_SHC_TRACE", 1);
    @autoreleasepool {
        Class dc = object_getClass((id)g_dev);
        int n = 0;
        n += ks_swap(dc, "newLibraryWithSource:options:error:", (IMP)ks_lib_err);
        n += ks_swap(dc, "newLibraryWithSource:options:completionHandler:", (IMP)ks_lib_cb);
        n += ks_swap(dc, "newRenderPipelineStateWithDescriptor:error:", (IMP)ks_rps_err);
        n += ks_swap(dc, "newRenderPipelineStateWithDescriptor:options:reflection:error:", (IMP)ks_rps_opt_err);
        n += ks_swap(dc, "newRenderPipelineStateWithDescriptor:completionHandler:", (IMP)ks_rps_cb);
        n += ks_swap(dc, "newRenderPipelineStateWithDescriptor:options:completionHandler:", (IMP)ks_rps_opt_cb);
        n += ks_swap(dc, "newComputePipelineStateWithFunction:error:", (IMP)ks_cps_fn_err);
        n += ks_swap(dc, "newComputePipelineStateWithFunction:options:reflection:error:", (IMP)ks_cps_opt_err);
        n += ks_swap(dc, "newComputePipelineStateWithFunction:completionHandler:", (IMP)ks_cps_fn_cb);
        n += ks_swap(dc, "newComputePipelineStateWithFunction:options:completionHandler:", (IMP)ks_cps_fn_opt_cb);
        n += ks_swap(dc, "newComputePipelineStateWithDescriptor:options:reflection:error:", (IMP)ks_cps_opt_err);
        n += ks_swap(dc, "newComputePipelineStateWithDescriptor:options:completionHandler:", (IMP)ks_cps_desc_opt_cb);
        n += ks_swap(dc, "newDepthStencilStateWithDescriptor:", (IMP)ks_dss);
        // The library class: the one a source compile returns.
        NSError *e = nil;
        t_warm = 1;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"kernel void kl_shc_probe(){}" options:nil error:&e];
        t_warm = 0;
        Class lc = lib ? object_getClass(lib) : Nil;
#if !__has_feature(objc_arc)
        [lib release];
#endif
        int m = 0;
        if (lc) {
            m += ks_swap(lc, "newFunctionWithName:", (IMP)ks_fn);
            m += ks_swap(lc, "newFunctionWithName:constantValues:error:", (IMP)ks_fn_cv_err);
            m += ks_swap(lc, "newFunctionWithName:constantValues:completionHandler:", (IMP)ks_fn_cv_cb);
        }
        int cvn = 0;
        MTLFunctionConstantValues *cvp = [(Class)objc_getClass("MTLFunctionConstantValues") new];
        Class cvc = cvp ? object_getClass(cvp) : Nil;
        if (cvc) {
            cvn += ks_swap(cvc, "setConstantValue:type:atIndex:", (IMP)ks_cv_idx);
            cvn += ks_swap(cvc, "setConstantValues:type:withRange:", (IMP)ks_cv_range);
            cvn += ks_swap(cvc, "setConstantValue:type:withName:", (IMP)ks_cv_name);
            cvn += ks_swap(cvc, "reset", (IMP)ks_cv_reset);
        }
        KS_RELEASE(cvp);
        g_on = 1;
        ks_warm_start();
        KSI("census ON: %d device entry points on %s, %d library entry points on %s, %d constant-value setters on %s; "
            "slow > %d ms named\n", n, class_getName(dc), m, lc ? class_getName(lc) : "(no library)", cvn,
            cvc ? class_getName(cvc) : "-", g_slow_ms);
    }
}

// ---------------------------------------------------------------- tests
uint32_t kl_shc_test_load(const char *path, int origin) {
    pthread_mutex_lock(&g_wmu);
    memset(g_tab, 0, sizeof g_tab);
    g_nent = 0;
    pthread_mutex_unlock(&g_wmu);
    ks_release_payloads();
    return ks_load(path, (uint8_t)origin);
}

int kl_shc_test_append(const char *path, uint64_t key, const char *src) {
    int fd = ks_rec_open(path);
    if (fd < 0) return 0;
    uLong n = (uLong)strlen(src);
    uLongf zl = compressBound(n);
    uint8_t *z = malloc(zl);
    int ok = z && compress2(z, &zl, (const Bytef *)src, n, 6) == Z_OK &&
             ks_rec_write(fd, 0x31544e45u, key, (uint32_t)n, z, (uint32_t)zl, "nil", ",test,");
    free(z);
    close(fd);
    return ok;
}
