// kl_perf — the per-second frame-cost line. See kl_perf.h for what it says
// and how each number is measured.
//
// Cost, by construction: one completed handler per MTLCommandBuffer (Metal
// already runs MoltenVK's own on each one) doing a mutex-guarded add; one
// thread-local store around each vkQueueSubmit; once per line a walk of the
// task's threads (~100 thread_info calls, a fraction of a millisecond per
// second). Nothing per draw, nothing per frame beyond a few adds.
#include "kl_perf.h"
#include "kl_env.h"
#include "kl_fove.h"
#include "kl_shc.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int64_t perf_now_ns(void) { return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
int64_t kl_perf_now_ns(void) { return perf_now_ns(); }

int kl_perf_on(void) {
    static int on = -1;
    // OFF by default on driver (it wraps -commit on every MTLCommandBuffer
    // class): a port opts in with KL_PERF=1 (hla bakes it).
    if (on < 0) on = kl_env_on("KL_PERF", 0);
    return on;
}

// ---------------------------------------------------------------- GPU time
#define PERF_ORD_N 32               // guest submit ordinals kept per frame
#define PERF_ORD_UNKNOWN (PERF_ORD_N - 1)

static __thread int t_tag;          // KL_PERF_GPU_* | ordinal << 8
static __thread const char *t_label; // kl_perf_pass_label
static __thread int t_gl;           // kl_perf_gl_frame: this thread is a GL guest's
void kl_perf_pass_label(const char *label) { t_label = label; }
static uint32_t g_ordinal;          // guest submits in the open frame (atomic)

static pthread_mutex_t g_gl = PTHREAD_MUTEX_INITIALIZER;
static struct {
    double   sum[KL_PERF_GPU_NTAG];         // seconds of GPU time, per tag
    uint64_t n[KL_PERF_GPU_NTAG];           // command buffers, per tag
    double   ord_sum[PERF_ORD_N];           // guest GPU time by submit ordinal
    uint64_t ord_n[PERF_ORD_N];
    double   busy, last_end;                // union of every interval (s)
    double   max_cb;                        // the longest single command buffer (s)
    uint64_t ord_passes[PERF_ORD_N], ord_copies[PERF_ORD_N], ord_px[PERF_ORD_N];
} g_gpu;

// Which tags a Metal queue has carried: a commit on a queue that carried
// exactly one tag inherits it when it arrives untagged (MoltenVK's
// asynchronous-submit mode, or a submit path that does not set one).
#define PERF_Q_N 8
static struct { void *q; unsigned mask; } g_q[PERF_Q_N];

static int perf_queue_tag(void *q, int tag) {         // g_gl held
    int base = tag & 0xff;
    for (int i = 0; i < PERF_Q_N; i++) {
        if (g_q[i].q == q) {
            if (base) { g_q[i].mask |= 1u << base; return tag; }
            unsigned m = g_q[i].mask;
            if (m == (1u << KL_PERF_GPU_GUEST)) return KL_PERF_GPU_GUEST | PERF_ORD_UNKNOWN << 8;
            if (m == (1u << KL_PERF_GPU_COPY)) return KL_PERF_GPU_COPY;
            return tag;
        }
        if (!g_q[i].q) {
            g_q[i].q = q;
            g_q[i].mask = base ? 1u << base : 1u;     // bit 0: carried untagged work
            return tag;
        }
    }
    return tag;
}

static void perf_gpu_add(int tag, double s, double e) {
    if (!(e > s) || s <= 0) return;
    int base = tag & 0xff, ord = (tag >> 8) & 0xff;
    if (base >= KL_PERF_GPU_NTAG) base = KL_PERF_GPU_OTHER;
    double d = e - s;
    pthread_mutex_lock(&g_gl);
    g_gpu.sum[base] += d;
    g_gpu.n[base]++;
    if (base == KL_PERF_GPU_GUEST) {
        if (ord >= PERF_ORD_N) ord = PERF_ORD_UNKNOWN;
        g_gpu.ord_sum[ord] += d;
        g_gpu.ord_n[ord]++;
    }
    if (d > g_gpu.max_cb) g_gpu.max_cb = d;
    // Completions arrive (nearly) in order; an interval that starts before the
    // last end only adds what sticks out past it.
    if (s >= g_gpu.last_end) { g_gpu.busy += d; g_gpu.last_end = e; }
    else if (e > g_gpu.last_end) { g_gpu.busy += e - g_gpu.last_end; g_gpu.last_end = e; }
    pthread_mutex_unlock(&g_gl);
}

void kl_perf_ord_note(int tag, uint32_t passes, uint32_t copies, uint64_t px) {
    int ord = (tag >> 8) & 0xff;
    if (ord >= PERF_ORD_N) return;
    pthread_mutex_lock(&g_gl);
    g_gpu.ord_passes[ord] += passes;
    g_gpu.ord_copies[ord] += copies;
    g_gpu.ord_px[ord] += px;
    pthread_mutex_unlock(&g_gl);
}

int kl_perf_gpu_tag(int tag) {
    int was = t_tag;
    t_tag = tag;
    return was;
}

// KL_PERF_GL_GUEST (default 1 under KL_PERF): ANGLE's command buffers are the
// GL guest's GPU work, but nothing tagged them — the [perf] line filed all of
// it, and the compositor's, under "other", and KL_PERF_PASSES (which times
// only guest-tagged command buffers) saw nothing on a GL guest. Called once a
// frame on the guest's frame thread (kl_openxr's xrWaitFrame, right after
// kl_perf_frame resets the ordinal): from here every command buffer ANGLE
// commits on this thread is a guest submit with its own ordinal. 0 restores
// the untagged filing.
void kl_perf_gl_frame(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_perf_on() && kl_env_on("KL_PERF_GL_GUEST", 1);
        if (on)
            fprintf(stderr, "  [perf] GL guest: ANGLE's command buffers on the frame thread are "
                            "filed as guest GPU time by submit ordinal (KL_PERF_GL_GUEST=1; 0 "
                            "files them under other)\n");
    }
    if (!on) return;
    t_gl = 1;
    t_tag = kl_perf_gpu_guest_tag();
}

int kl_perf_gpu_guest_tag(void) {
    uint32_t o = __atomic_fetch_add(&g_ordinal, 1, __ATOMIC_RELAXED);
    if (o >= PERF_ORD_UNKNOWN) o = PERF_ORD_UNKNOWN - 1;
    return KL_PERF_GPU_GUEST | (int)(o << 8);
}

// The wrapped -commit. One entry per concrete class whose method we replaced;
// a subclass that inherits the wrapper finds its ancestor's original.
#define PERF_CLS_N 8
static struct { Class cls; IMP orig; } g_cls[PERF_CLS_N];
static int g_ncls;
static SEL s_commit, s_queue, s_add, s_start, s_end;

static void pp_on_commit(id cb);       // KL_PERF_PASSES (below)
static void pp_wrap_class(Class c);
static int  pp_on(void);

static void perf_commit(id self, SEL cmd) {
    IMP orig = NULL;
    for (Class c = object_getClass(self); c && !orig; c = class_getSuperclass(c))
        for (int i = 0; i < g_ncls; i++)
            if (g_cls[i].cls == c) { orig = g_cls[i].orig; break; }
    if (!orig) orig = g_cls[0].orig;            // unreachable: only our classes reach here
    int tag = t_tag;
    id q = ((id (*)(id, SEL))objc_msgSend)(self, s_queue);
    pthread_mutex_lock(&g_gl);
    tag = perf_queue_tag((__bridge void *)q, tag);
    pthread_mutex_unlock(&g_gl);
    void (^h)(id) = ^(id cb) {
        double s = ((double (*)(id, SEL))objc_msgSend)(cb, s_start);
        double e = ((double (*)(id, SEL))objc_msgSend)(cb, s_end);
        perf_gpu_add(tag, s, e);
    };
    ((void (*)(id, SEL, id))objc_msgSend)(self, s_add, h);
    if (pp_on()) pp_on_commit(self);
    ((void (*)(id, SEL))orig)(self, cmd);
    // A GL guest's thread: ANGLE commits several command buffers a frame, each
    // the guest's; the next one gets the next ordinal (kl_perf_gl_frame).
    if (t_gl) t_tag = kl_perf_gpu_guest_tag();
}

static void perf_wrap_class(Class c) {
    if (!c || g_ncls >= PERF_CLS_N) return;
    for (int i = 0; i < g_ncls; i++) if (g_cls[i].cls == c) return;
    IMP cur = class_getMethodImplementation(c, s_commit);
    if (!cur || cur == (IMP)perf_commit) return;      // an ancestor is already wrapped
    g_cls[g_ncls].cls = c;
    g_cls[g_ncls].orig = cur;
    __atomic_store_n(&g_ncls, g_ncls + 1, __ATOMIC_RELEASE);
    class_replaceMethod(c, s_commit, (IMP)perf_commit, "v@:");
    fprintf(stderr, "  [perf] GPU timing: -commit wrapped on %s\n", class_getName(c));
    if (pp_on()) pp_wrap_class(c);
}

void kl_perf_gpu_install(void *mtl_device) {
    static int done;
    if (done || !mtl_device || !kl_perf_on() || !kl_env_on("KL_PERF_GPU", 1)) return;
    done = 1;
    s_commit = sel_registerName("commit");
    s_queue  = sel_registerName("commandQueue");
    s_add    = sel_registerName("addCompletedHandler:");
    s_start  = sel_registerName("GPUStartTime");
    s_end    = sel_registerName("GPUEndTime");
    @autoreleasepool {
        id dev = (__bridge id)mtl_device;
        id q = ((id (*)(id, SEL))objc_msgSend)(dev, sel_registerName("newCommandQueue"));
        if (!q) { fprintf(stderr, "  [perf] GPU timing: no command queue — off\n"); return; }
        id a = ((id (*)(id, SEL))objc_msgSend)(q, sel_registerName("commandBuffer"));
        id b = ((id (*)(id, SEL))objc_msgSend)(q, sel_registerName("commandBufferWithUnretainedReferences"));
        id c = nil;
        Class dc = objc_getClass("MTLCommandBufferDescriptor");
        if (dc) {
            id d = ((id (*)(id, SEL))objc_msgSend)((id)dc, sel_registerName("new"));
            if (d) {
                c = ((id (*)(id, SEL, id))objc_msgSend)(q, sel_registerName("commandBufferWithDescriptor:"), d);
                ((void (*)(id, SEL))objc_msgSend)(d, sel_registerName("release"));
            }
        }
        if (a) perf_wrap_class(object_getClass(a));
        if (b) perf_wrap_class(object_getClass(b));
        if (c) perf_wrap_class(object_getClass(c));
        ((void (*)(id, SEL))objc_msgSend)(q, sel_registerName("release"));
    }
}


// ---------------------------------------------------------------- per-pass GPU time
// KL_PERF_PASSES=1 (diagnostic, off; round N15): every Metal encoder MoltenVK
// opens on a GUEST-tagged command buffer gets a timestamp counter sample buffer
// attached at its stage boundaries (Apple GPUs sample there, not per draw), so
// the guest's one big command buffer is split into a per-encoder cost table:
// encoder index within the command buffer, kind (R render / C compute / B blit),
// what it draws into (size, format, samples, load/store, texture label), its
// span (first stage start -> last stage end) and its EXCLUSIVE time (end minus
// the later of its start and the previous encoder's end: encoders overlap on
// a tiler, the exclusive column sums to the command buffer's span). Averaged per
// (submit ordinal, encoder index) and printed every KL_PERF_PASSES_SECS (10).
// Encoder creation goes through a copied descriptor; nothing else changes.
#define PP_SB_N   24                // sample buffers = guest command buffers in flight (ANGLE: ~6 a frame)
#define PP_ENC_N  1024              // encoders timed per command buffer
#define PP_SAMPLES 4096             // 4 per render encoder (32 KiB, Metal's cap)
#define PP_ORDS   8                 // submit ordinals kept apart (the last collects the rest)
#define PP_KEYS   (PP_ORDS * PP_ENC_N) // ordinals 0..PP_ORDS-1 x encoder index

typedef struct {
    char kind;
    char desc[200];
} pp_enc;

typedef struct {
    void *cb;                       // the MTLCommandBuffer (unretained key)
    int   sbi, ord, n;
    pp_enc e[PP_ENC_N];
} pp_cb;

static int g_pp_on = -1;
// No stage-boundary counters (the simulator's MTLSim GPU): still COUNT the
// encoders and describe their targets, with no times — the pass census.
static int g_pp_census;
static uint64_t g_pp_frame_no;      // frames since start (KL_PERF_PASSES_DUMP_AT)
static id<MTLCounterSet> g_pp_set;
static id<MTLCounterSampleBuffer> g_pp_sb[PP_SB_N];
static int g_pp_sb_busy[PP_SB_N];
static pp_cb *g_pp_open[PP_SB_N];
static double g_pp_ns_per_tick = 1.0;
static pthread_mutex_t g_pp_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    double span[PP_KEYS], excl[PP_KEYS], vtx[PP_KEYS];
    uint64_t n[PP_KEYS];
    char kind[PP_KEYS];
    char desc[PP_KEYS][200];
    uint64_t cbs, encs, enc_min, enc_max, dropped;
    double cb_span;
    int64_t t0;
} g_pp;

static IMP pp_o_render, pp_o_cdesc, pp_o_bdesc, pp_o_c0, pp_o_cdt, pp_o_b0;

static int pp_on(void) {
    if (g_pp_on < 0) g_pp_on = kl_perf_on() && kl_env_on("KL_PERF_PASSES", 0);
    return g_pp_on;
}

static pp_cb *pp_cb_for(id cb, int create) {          // g_pp_mu held
    void *k = (__bridge void *)cb;
    for (int i = 0; i < PP_SB_N; i++)
        if (g_pp_open[i] && g_pp_open[i]->cb == k) return g_pp_open[i];
    if (!create) return NULL;
    int tag = t_tag;
    if ((tag & 0xff) != KL_PERF_GPU_GUEST || (!g_pp_set && !g_pp_census)) return NULL;
    for (int i = 0; i < PP_SB_N; i++) {
        if (g_pp_sb_busy[i] || (!g_pp_sb[i] && !g_pp_census)) continue;
        pp_cb *p = calloc(1, sizeof *p);
        if (!p) return NULL;
        p->cb = k; p->sbi = i; p->ord = (tag >> 8) & 0xff;
        g_pp_sb_busy[i] = 1;
        for (int j = 0; j < PP_SB_N; j++) if (!g_pp_open[j]) { g_pp_open[j] = p; return p; }
        g_pp_sb_busy[i] = 0;
        free(p);
        return NULL;
    }
    g_pp.dropped++;
    return NULL;
}

static const char *pp_load(MTLLoadAction a) { return a == MTLLoadActionLoad ? "L" : a == MTLLoadActionClear ? "C" : "-"; }
static const char *pp_store(MTLStoreAction a) {
    switch (a) { case MTLStoreActionStore: return "S"; case MTLStoreActionMultisampleResolve: return "R";
                 case MTLStoreActionStoreAndMultisampleResolve: return "SR"; default: return "-"; }
}

static void pp_desc_render(MTLRenderPassDescriptor *d, char *out, size_t cap) {
    int o = 0, nc = 0;
    id<MTLTexture> first = nil;
    for (int i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (!a.texture) continue;
        nc++;
        if (!first) {
            first = a.texture;
            o += snprintf(out + o, cap - (size_t)o, "c%d %lux%lu f%lu s%lu %s%s%s '%s'", i,
                          (unsigned long)a.texture.width, (unsigned long)a.texture.height,
                          (unsigned long)a.texture.pixelFormat, (unsigned long)a.texture.sampleCount,
                          pp_load(a.loadAction), pp_store(a.storeAction),
                          a.resolveTexture ? "+rt" : "", a.texture.label ? a.texture.label.UTF8String : "");
        }
    }
    if (nc > 1 && o < (int)cap) o += snprintf(out + o, cap - (size_t)o, " (+%d col)", nc - 1);
    id<MTLTexture> dt = d.depthAttachment.texture;
    if (dt && o < (int)cap)
        o += snprintf(out + o, cap - (size_t)o, " | d %lux%lu f%lu s%lu %s%s '%s'",
                      (unsigned long)dt.width, (unsigned long)dt.height, (unsigned long)dt.pixelFormat,
                      (unsigned long)dt.sampleCount, pp_load(d.depthAttachment.loadAction),
                      pp_store(d.depthAttachment.storeAction), dt.label ? dt.label.UTF8String : "");
    if (!first && !dt && o < (int)cap)
        o += snprintf(out + o, cap - (size_t)o, "no attachments %lux%lu",
                      (unsigned long)d.renderTargetWidth, (unsigned long)d.renderTargetHeight);
    if (d.renderTargetArrayLength > 1 && o < (int)cap)
        o += snprintf(out + o, cap - (size_t)o, " layers %lu", (unsigned long)d.renderTargetArrayLength);
    if (d.rasterizationRateMap && o < (int)cap) {
        MTLSize p = [d.rasterizationRateMap physicalSizeForLayer:0];
        o += snprintf(out + o, cap - (size_t)o, " [rate map, physical %lux%lu]",
                      (unsigned long)p.width, (unsigned long)p.height);
    }
    if (t_label && o < (int)cap) snprintf(out + o, cap - (size_t)o, " [%s]", t_label);
}

static id pp_render(id self, SEL cmd, MTLRenderPassDescriptor *d) {
    pthread_mutex_lock(&g_pp_mu);
    pp_cb *p = pp_cb_for(self, 1);
    int idx = p && p->n < PP_ENC_N ? p->n++ : -1;
    id<MTLCounterSampleBuffer> sb = p && idx >= 0 ? g_pp_sb[p->sbi] : nil;
    pthread_mutex_unlock(&g_pp_mu);
    if (g_pp_census && p && idx >= 0) {
        p->e[idx].kind = 'R';
        pp_desc_render(d, p->e[idx].desc, sizeof p->e[idx].desc);
        return ((id (*)(id, SEL, id))pp_o_render)(self, cmd, d);
    }
    if (!sb) return ((id (*)(id, SEL, id))pp_o_render)(self, cmd, d);
    MTLRenderPassDescriptor *c = [d copy];
    MTLRenderPassSampleBufferAttachmentDescriptor *a = c.sampleBufferAttachments[0];
    a.sampleBuffer = sb;
    a.startOfVertexSampleIndex = (NSUInteger)idx * 4;
    a.endOfVertexSampleIndex = (NSUInteger)idx * 4 + 1;
    a.startOfFragmentSampleIndex = (NSUInteger)idx * 4 + 2;
    a.endOfFragmentSampleIndex = (NSUInteger)idx * 4 + 3;
    p->e[idx].kind = 'R';
    pp_desc_render(d, p->e[idx].desc, sizeof p->e[idx].desc);
    return ((id (*)(id, SEL, id))pp_o_render)(self, cmd, c);
}

static id pp_compute_with(id self, MTLComputePassDescriptor *c, char kind, const char *what) {
    pthread_mutex_lock(&g_pp_mu);
    pp_cb *p = pp_cb_for(self, 1);
    int idx = p && p->n < PP_ENC_N ? p->n++ : -1;
    id<MTLCounterSampleBuffer> sb = p && idx >= 0 ? g_pp_sb[p->sbi] : nil;
    pthread_mutex_unlock(&g_pp_mu);
    if (sb) {
        MTLComputePassSampleBufferAttachmentDescriptor *a = c.sampleBufferAttachments[0];
        a.sampleBuffer = sb;
        a.startOfEncoderSampleIndex = (NSUInteger)idx * 4;
        a.endOfEncoderSampleIndex = (NSUInteger)idx * 4 + 3;
        p->e[idx].kind = kind;
        snprintf(p->e[idx].desc, sizeof p->e[idx].desc, "%s [%s]", what, t_label ? t_label : "");
    } else if (g_pp_census && p && idx >= 0) {
        p->e[idx].kind = kind;
        snprintf(p->e[idx].desc, sizeof p->e[idx].desc, "%s [%s]", what, t_label ? t_label : "");
    }
    return ((id (*)(id, SEL, id))pp_o_cdesc)(self, sel_registerName("computeCommandEncoderWithDescriptor:"), c);
}
static id pp_c0(id self, SEL cmd) {
    MTLComputePassDescriptor *c = [(Class)objc_getClass("MTLComputePassDescriptor") computePassDescriptor];
    return pp_compute_with(self, c, 'C', "compute");
}
static id pp_cdt(id self, SEL cmd, MTLDispatchType t) {
    MTLComputePassDescriptor *c = [(Class)objc_getClass("MTLComputePassDescriptor") computePassDescriptor];
    c.dispatchType = t;
    return pp_compute_with(self, c, 'C', t == MTLDispatchTypeConcurrent ? "compute (concurrent)" : "compute");
}
static id pp_cdesc(id self, SEL cmd, MTLComputePassDescriptor *d) {
    return pp_compute_with(self, [d copy], 'C', "compute (desc)");
}
static id pp_blit_with(id self, MTLBlitPassDescriptor *c) {
    pthread_mutex_lock(&g_pp_mu);
    pp_cb *p = pp_cb_for(self, 1);
    int idx = p && p->n < PP_ENC_N ? p->n++ : -1;
    id<MTLCounterSampleBuffer> sb = p && idx >= 0 ? g_pp_sb[p->sbi] : nil;
    pthread_mutex_unlock(&g_pp_mu);
    if (sb) {
        MTLBlitPassSampleBufferAttachmentDescriptor *a = c.sampleBufferAttachments[0];
        a.sampleBuffer = sb;
        a.startOfEncoderSampleIndex = (NSUInteger)idx * 4;
        a.endOfEncoderSampleIndex = (NSUInteger)idx * 4 + 3;
        p->e[idx].kind = 'B';
        snprintf(p->e[idx].desc, sizeof p->e[idx].desc, "blit [%s]", t_label ? t_label : "");
    } else if (g_pp_census && p && idx >= 0) {
        p->e[idx].kind = 'B';
        snprintf(p->e[idx].desc, sizeof p->e[idx].desc, "blit [%s]", t_label ? t_label : "");
    }
    return ((id (*)(id, SEL, id))pp_o_bdesc)(self, sel_registerName("blitCommandEncoderWithDescriptor:"), c);
}
static id pp_b0(id self, SEL cmd) { return pp_blit_with(self, [(Class)objc_getClass("MTLBlitPassDescriptor") blitPassDescriptor]); }
static id pp_bdesc(id self, SEL cmd, MTLBlitPassDescriptor *d) { return pp_blit_with(self, [d copy]); }

static void pp_swap(Class c, const char *sel, IMP mine, IMP *orig) {
    SEL s = sel_registerName(sel);
    Method m = class_getInstanceMethod(c, s);
    if (!m) { fprintf(stderr, "  [perf] passes: %s has no %s\n", class_getName(c), sel); return; }
    IMP cur = method_getImplementation(m);
    if (cur == mine) return;
    if (*orig && *orig != cur)
        fprintf(stderr, "  [perf] passes: %s's %s differs from the first class's — timing only the first\n",
                class_getName(c), sel);
    if (!*orig) *orig = cur;
    if (*orig == cur) class_replaceMethod(c, s, mine, method_getTypeEncoding(m));
}

static void pp_wrap_class(Class c) {
    static int setup;
    if (!setup) {
        setup = 1;
        id<MTLDevice> dev = nil;
        // The device is the one -commit was installed for; ask Metal for the
        // system default when nothing else is at hand (one GPU on Apple Silicon).
        // Looked up, not linked: the test binaries link the runtime without Metal.
        id<MTLDevice> (*mk)(void) = (id<MTLDevice> (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
        dev = mk ? mk() : nil;
        if (![dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            fprintf(stderr, "  [perf] passes: no stage-boundary counter sampling on this GPU — "
                            "CENSUS only (render encoders counted and described, no times)\n");
            g_pp_census = 1;
            g_pp.enc_min = UINT64_MAX;
        }
        if (!g_pp_census) {
            for (id<MTLCounterSet> cs in dev.counterSets)
                if ([cs.name isEqualToString:@"timestamp"]) g_pp_set = cs;
            if (!g_pp_set) { fprintf(stderr, "  [perf] passes: no timestamp counter set — off\n"); g_pp_on = 0; return; }
            for (int i = 0; i < PP_SB_N; i++) {
                MTLCounterSampleBufferDescriptor *d = [(Class)objc_getClass("MTLCounterSampleBufferDescriptor") new];
                d.counterSet = g_pp_set;
                d.storageMode = MTLStorageModeShared;
                d.sampleCount = PP_SAMPLES;
                NSError *err = nil;
                g_pp_sb[i] = [dev newCounterSampleBufferWithDescriptor:d error:&err];
                if (!g_pp_sb[i]) fprintf(stderr, "  [perf] passes: sample buffer %d: %s\n", i,
                                         err ? err.localizedDescription.UTF8String : "?");
            }
            // GPU ticks -> ns, from two correlated samples 50 ms apart.
            MTLTimestamp c0 = 0, g0 = 0, c1 = 0, g1 = 0;
            int64_t w0 = perf_now_ns();
            [dev sampleTimestamps:&c0 gpuTimestamp:&g0];
            usleep(50000);
            [dev sampleTimestamps:&c1 gpuTimestamp:&g1];
            int64_t w1 = perf_now_ns();
            if (g1 > g0) g_pp_ns_per_tick = (double)(w1 - w0) / (double)(g1 - g0);   // 1.0 on Apple GPUs
            g_pp.enc_min = UINT64_MAX;
            fprintf(stderr, "  [perf] passes: ON — %d sample buffers x %d, %.4f ns per GPU tick\n",
                    PP_SB_N, PP_SAMPLES, g_pp_ns_per_tick);
        }
    }
    if (!g_pp_on) return;
    pp_swap(c, "renderCommandEncoderWithDescriptor:", (IMP)pp_render, &pp_o_render);
    pp_swap(c, "computeCommandEncoderWithDescriptor:", (IMP)pp_cdesc, &pp_o_cdesc);
    pp_swap(c, "blitCommandEncoderWithDescriptor:", (IMP)pp_bdesc, &pp_o_bdesc);
    pp_swap(c, "computeCommandEncoder", (IMP)pp_c0, &pp_o_c0);
    pp_swap(c, "computeCommandEncoderWithDispatchType:", (IMP)pp_cdt, &pp_o_cdt);
    pp_swap(c, "blitCommandEncoder", (IMP)pp_b0, &pp_o_b0);
}

static void pp_on_commit(id cb) {
    pthread_mutex_lock(&g_pp_mu);
    pp_cb *p = pp_cb_for(cb, 0);
    if (p) for (int j = 0; j < PP_SB_N; j++) if (g_pp_open[j] == p) g_pp_open[j] = NULL;
    pthread_mutex_unlock(&g_pp_mu);
    if (!p) return;
    // KL_PERF_PASSES_DUMP=N (with _AT=frame): the encoder sequence of N guest
    // command buffers from that frame on, in order — one frame's passes as
    // ANGLE cut them, which the averaged table cannot show.
    static int dump_n = -1, dump_at;
    if (dump_n < 0) {
        dump_n = kl_env_int("KL_PERF_PASSES_DUMP", 0);
        dump_at = kl_env_int("KL_PERF_PASSES_DUMP_AT", 3000);
    }
    if (dump_n > 0 && g_pp_frame_no >= (uint64_t)dump_at) {
        dump_n--;
        fprintf(stderr, "  [passdump] frame %llu cb ord %d: %d encoders\n",
                (unsigned long long)g_pp_frame_no, p->ord, p->n);
        for (int i = 0; i < p->n; i++)
            fprintf(stderr, "  [passdump]   %2d %c %s\n", i, p->e[i].kind ? p->e[i].kind : '?', p->e[i].desc);
    }
    if (g_pp_census) {                  // counted now; nothing to wait for
        pthread_mutex_lock(&g_pp_mu);
        g_pp.cbs++;
        g_pp.encs += (uint64_t)p->n;
        if ((uint64_t)p->n < g_pp.enc_min) g_pp.enc_min = (uint64_t)p->n;
        if ((uint64_t)p->n > g_pp.enc_max) g_pp.enc_max = (uint64_t)p->n;
        for (int i = 0; i < p->n; i++) {
            if (!p->e[i].kind) continue;
            int key = (p->ord < PP_ORDS ? p->ord : PP_ORDS - 1) * PP_ENC_N + i;
            g_pp.n[key]++;
            g_pp.kind[key] = p->e[i].kind;
            memcpy(g_pp.desc[key], p->e[i].desc, sizeof g_pp.desc[key]);
        }
        g_pp_sb_busy[p->sbi] = 0;
        pthread_mutex_unlock(&g_pp_mu);
        free(p);
        return;
    }
    [(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        id<MTLCounterSampleBuffer> sb = g_pp_sb[p->sbi];
        NSData *data = p->n ? [sb resolveCounterRange:NSMakeRange(0, (NSUInteger)p->n * 4)] : nil;
        const MTLCounterResultTimestamp *ts = data ? (const MTLCounterResultTimestamp *)data.bytes : NULL;
        // Metal's timestamps are CLOCK_UPTIME_RAW ns, the clock GPUStartTime
        // reads in seconds; a stage that did not run leaves the slot's value
        // from an earlier command buffer, so only samples inside this one's
        // own GPU interval count.
        uint64_t lo = (uint64_t)(done.GPUStartTime * 1e9) - 100000;
        uint64_t hi = (uint64_t)(done.GPUEndTime * 1e9) + 100000;
        pthread_mutex_lock(&g_pp_mu);
        if (ts && done.status == MTLCommandBufferStatusCompleted) {
            g_pp.cbs++;
            g_pp.encs += (uint64_t)p->n;
            if ((uint64_t)p->n < g_pp.enc_min) g_pp.enc_min = (uint64_t)p->n;
            if ((uint64_t)p->n > g_pp.enc_max) g_pp.enc_max = (uint64_t)p->n;
            uint64_t prev_end = 0, first = 0, last = 0;
            for (int i = 0; i < p->n; i++) {
                uint64_t v[4];
                for (int k = 0; k < 4; k++) {
                    v[k] = ts[i * 4 + k].timestamp;
                    if (v[k] == ~0ull || v[k] < lo || v[k] > hi) v[k] = 0;
                }
                uint64_t s = 0, e = 0;
                for (int k = 0; k < 4; k++) if (v[k]) { if (!s || v[k] < s) s = v[k]; if (v[k] > e) e = v[k]; }
                if (!s || e < s) continue;          // the encoder ran no stage
                if (!first || s < first) first = s;
                if (e > last) last = e;
                uint64_t from = s > prev_end ? s : prev_end;
                double excl = e > from ? (double)(e - from) : 0;
                if (e > prev_end) prev_end = e;
                int key = (p->ord < PP_ORDS ? p->ord : PP_ORDS - 1) * PP_ENC_N + i;
                g_pp.span[key] += (double)(e - s) * g_pp_ns_per_tick;
                g_pp.excl[key] += excl * g_pp_ns_per_tick;
                if (p->e[i].kind == 'R' && v[0] && v[1] > v[0])
                    g_pp.vtx[key] += (double)(v[1] - v[0]) * g_pp_ns_per_tick;
                g_pp.n[key]++;
                g_pp.kind[key] = p->e[i].kind;
                memcpy(g_pp.desc[key], p->e[i].desc, sizeof g_pp.desc[key]);
            }
            if (last > first) g_pp.cb_span += (double)(last - first) * g_pp_ns_per_tick;
        }
        g_pp_sb_busy[p->sbi] = 0;
        pthread_mutex_unlock(&g_pp_mu);
        free(p);
    }];
}

// The table, from the frame owner's thread.
static void pp_emit(int64_t now, uint64_t frames) {
    static int every = -1;
    if (every < 0) every = kl_env_int("KL_PERF_PASSES_SECS", 10);
    if (!g_pp.t0) { g_pp.t0 = now; return; }
    if (every <= 0 || now - g_pp.t0 < (int64_t)every * 1000000000LL || !frames) return;
    pthread_mutex_lock(&g_pp_mu);
    double f = (double)frames;
    double tot_excl = 0;
    for (int k = 0; k < PP_KEYS; k++) tot_excl += g_pp.excl[k];
    fprintf(stderr, "  [perf] passes over %.0f frames: %llu guest cbs, %.1f encoders/cb (min %llu max %llu), "
            "%llu cbs untimed, timed span %.2f ms/f, exclusive sum %.2f ms/f\n",
            f, (unsigned long long)g_pp.cbs, g_pp.cbs ? (double)g_pp.encs / (double)g_pp.cbs : 0,
            (unsigned long long)(g_pp.enc_min == UINT64_MAX ? 0 : g_pp.enc_min), (unsigned long long)g_pp.enc_max,
            (unsigned long long)g_pp.dropped, g_pp.cb_span / 1e6 / f, tot_excl / 1e6 / f);
    for (int k = 0; k < PP_KEYS; k++) {
        if (!g_pp.n[k]) continue;
        fprintf(stderr, "  [pass] #%d.%03d %c n%.2f span %.3f excl %.3f vtx %.3f ms/f | %s\n",
                k / PP_ENC_N, k % PP_ENC_N, g_pp.kind[k], (double)g_pp.n[k] / f,
                g_pp.span[k] / 1e6 / f, g_pp.excl[k] / 1e6 / f, g_pp.vtx[k] / 1e6 / f, g_pp.desc[k]);
    }
    memset(g_pp.span, 0, sizeof g_pp.span);
    memset(g_pp.excl, 0, sizeof g_pp.excl);
    memset(g_pp.vtx, 0, sizeof g_pp.vtx);
    memset(g_pp.n, 0, sizeof g_pp.n);
    g_pp.cbs = g_pp.encs = g_pp.enc_max = g_pp.dropped = 0;
    g_pp.enc_min = UINT64_MAX;
    g_pp.cb_span = 0;
    g_pp.t0 = now;
    pthread_mutex_unlock(&g_pp_mu);
}

// ---------------------------------------------------------------- CPU time
#define PERF_THR_N 512
static struct { uint64_t tid, last_ns; uint32_t gen; } g_thr[PERF_THR_N];
static uint32_t g_gen;
static uint64_t g_role_tid[KL_PERF_ROLE_N];

void kl_perf_role(int role) {
    if (role < 0 || role >= KL_PERF_ROLE_N) return;
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    if (__atomic_load_n(&g_role_tid[role], __ATOMIC_RELAXED) != tid)
        __atomic_store_n(&g_role_tid[role], tid, __ATOMIC_RELAXED);
}

typedef struct { uint64_t tid, ns; char name[24]; } perf_top;

// Walks the task's threads; returns total CPU ns since the last walk and the
// busiest few (by ns, descending) in `top`.
static uint64_t perf_threads(perf_top *top, int ntop, uint64_t *role_ns) {
    thread_act_array_t list = NULL;
    mach_msg_type_number_t n = 0;
    for (int r = 0; r < KL_PERF_ROLE_N; r++) role_ns[r] = 0;
    memset(top, 0, sizeof *top * (size_t)ntop);
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return 0;
    g_gen++;
    uint64_t total = 0;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        thread_identifier_info_data_t id;
        mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
        thread_extended_info_data_t ex;
        mach_msg_type_number_t c2 = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &c) == KERN_SUCCESS &&
            thread_info(list[i], THREAD_EXTENDED_INFO, (thread_info_t)&ex, &c2) == KERN_SUCCESS) {
            uint64_t ns = ex.pth_user_time + ex.pth_system_time, d = 0;
            unsigned h = (unsigned)(id.thread_id * 2654435761u) % PERF_THR_N;
            for (int k = 0; k < 16; k++) {
                unsigned at = (h + (unsigned)k) % PERF_THR_N;
                if (g_thr[at].tid == id.thread_id || !g_thr[at].tid ||
                    g_thr[at].gen + 2 < g_gen) {
                    if (g_thr[at].tid == id.thread_id && ns >= g_thr[at].last_ns)
                        d = ns - g_thr[at].last_ns;
                    g_thr[at].tid = id.thread_id;
                    g_thr[at].last_ns = ns;
                    g_thr[at].gen = g_gen;
                    break;
                }
            }
            total += d;
            for (int r = 0; r < KL_PERF_ROLE_N; r++)
                if (id.thread_id == g_role_tid[r]) role_ns[r] = d;
            for (int k = 0; k < ntop; k++) {
                if (d > top[k].ns) {
                    memmove(&top[k + 1], &top[k], sizeof *top * (size_t)(ntop - k - 1));
                    top[k].tid = id.thread_id;
                    top[k].ns = d;
                    snprintf(top[k].name, sizeof top[k].name, "%s", ex.pth_name);
                    break;
                }
            }
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof *list);
    return total;
}

static uint64_t perf_footprint(void) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c) != KERN_SUCCESS) return 0;
    return vi.phys_footprint;
}

// ---------------------------------------------------------------- the line
static struct {
    int64_t  t0, last_frame, max_frame;
    uint64_t frames, ticks;
    int64_t  xr_wait, fence, qsubmit;
    int64_t  ord_t0;
    uint64_t ord_frames;
    int      primed;
} g_win;

void kl_perf_fence_wait(int64_t ns) {
    if (ns > 0) __atomic_add_fetch(&g_win.fence, ns, __ATOMIC_RELAXED);
}

void kl_perf_guest_submit(int64_t ns) {
    if (ns > 0) __atomic_add_fetch(&g_win.qsubmit, ns, __ATOMIC_RELAXED);
    kl_perf_role(KL_PERF_ROLE_QUEUE);
}

static void perf_emit(int64_t now) {
    double wall = (double)(now - g_win.t0) / 1e9;
    double f = (double)(g_win.frames ? g_win.frames : 1);
    double sum[KL_PERF_GPU_NTAG], busy, maxcb, ord[PERF_ORD_N];
    uint64_t ordn[PERF_ORD_N], opass[PERF_ORD_N], ocopy[PERF_ORD_N], opx[PERF_ORD_N];
    pthread_mutex_lock(&g_gl);
    memcpy(sum, g_gpu.sum, sizeof sum);
    busy = g_gpu.busy; maxcb = g_gpu.max_cb;
    memset(g_gpu.sum, 0, sizeof g_gpu.sum);
    memset(g_gpu.n, 0, sizeof g_gpu.n);
    g_gpu.busy = 0; g_gpu.max_cb = 0;
    memcpy(ord, g_gpu.ord_sum, sizeof ord);
    memcpy(ordn, g_gpu.ord_n, sizeof ordn);
    memcpy(opass, g_gpu.ord_passes, sizeof opass);
    memcpy(ocopy, g_gpu.ord_copies, sizeof ocopy);
    memcpy(opx, g_gpu.ord_px, sizeof opx);
    pthread_mutex_unlock(&g_gl);

    perf_top top[5];
    uint64_t role[KL_PERF_ROLE_N];
    uint64_t cpu = perf_threads(top, 5, role);
    char tops[256];
    int o = 0;
    for (int k = 0; k < 5 && top[k].ns && o < (int)sizeof tops - 48; k++) {
        char r[8];
        int ro = 0;
        r[0] = 0;
        if (top[k].tid == g_role_tid[KL_PERF_ROLE_WAIT])   ro += snprintf(r + ro, sizeof r - (size_t)ro, "%sW", ro ? "" : " ");
        if (top[k].tid == g_role_tid[KL_PERF_ROLE_SUBMIT]) ro += snprintf(r + ro, sizeof r - (size_t)ro, "%sS", ro ? "" : " ");
        if (top[k].tid == g_role_tid[KL_PERF_ROLE_QUEUE])  ro += snprintf(r + ro, sizeof r - (size_t)ro, "%sQ", ro ? "" : " ");
        o += snprintf(tops + o, sizeof tops - (size_t)o, "%s%s#%llu %.1f%s",
                      k ? ", " : "", top[k].name[0] ? top[k].name : "",
                      (unsigned long long)(top[k].tid % 100000), (double)top[k].ns / 1e6 / f, r);
    }
    double gpu_all = sum[0] + sum[1] + sum[2];
    char fove[200], shc[256];
    kl_fove_perf_field(fove, sizeof fove);
    kl_shc_perf_field(shc, sizeof shc);
    fprintf(stderr,
            "  [perf] %.1f fps %.1f ms (max %.1f) | ticks %.2f/f | gpu %.1f ms/f busy %.0f%% "
            "(guest %.1f copy %.2f other %.1f; longest cb %.1f) | wait xr %.1f fence %.1f ms/f | "
            "vkQueueSubmit %.1f ms/f | cpu %.1f ms/f, W %.1f S %.1f Q %.1f: %s | mtl %llu MiB "
            "foot %llu MiB%s%s\n",
            (double)g_win.frames / wall, wall * 1e3 / f, (double)g_win.max_frame / 1e6,
            (double)g_win.ticks / f, gpu_all * 1e3 / f, 100.0 * busy / wall,
            sum[KL_PERF_GPU_GUEST] * 1e3 / f, sum[KL_PERF_GPU_COPY] * 1e3 / f,
            sum[KL_PERF_GPU_OTHER] * 1e3 / f, maxcb * 1e3,
            (double)g_win.xr_wait / 1e6 / f, (double)__atomic_load_n(&g_win.fence, __ATOMIC_RELAXED) / 1e6 / f,
            (double)__atomic_load_n(&g_win.qsubmit, __ATOMIC_RELAXED) / 1e6 / f,
            (double)cpu / 1e6 / f, (double)role[KL_PERF_ROLE_WAIT] / 1e6 / f,
            (double)role[KL_PERF_ROLE_SUBMIT] / 1e6 / f, (double)role[KL_PERF_ROLE_QUEUE] / 1e6 / f, tops,
            kl_vulkan_mtl_allocated_bytes() >> 20, (unsigned long long)(perf_footprint() >> 20), fove, shc);

    // The guest's GPU time by submit ordinal, over a longer window.
    static int every_s = -1;
    if (every_s < 0) every_s = kl_env_int("KL_PERF_SUBMIT_SECS", 10);
    if (every_s > 0 && now - g_win.ord_t0 >= (int64_t)every_s * 1000000000LL && g_win.ord_frames) {
        char line[2048];
        int p = 0;
        double of = (double)g_win.ord_frames;
        for (int i = 0; i < PERF_ORD_N && p < (int)sizeof line - 80; i++) {
            if (!ordn[i]) continue;
            p += snprintf(line + p, sizeof line - (size_t)p, " #%d%s %.2f", i,
                          i == PERF_ORD_UNKNOWN ? "(untagged)" : "", ord[i] * 1e3 / of);
            if (opass[i] || ocopy[i])       // KL_PERF_SPLIT_SUBMIT: what it recorded
                p += snprintf(line + p, sizeof line - (size_t)p, " (%.1f pass %.1f Mpx%s%.1f copy)",
                              (double)opass[i] / of, (double)opx[i] / 1e6 / of, " ",
                              (double)ocopy[i] / of);
        }
        fprintf(stderr, "  [perf] guest GPU by submit ordinal (ms/frame over %.0f frames):%s\n",
                of, line);
        pthread_mutex_lock(&g_gl);
        memset(g_gpu.ord_sum, 0, sizeof g_gpu.ord_sum);
        memset(g_gpu.ord_n, 0, sizeof g_gpu.ord_n);
        memset(g_gpu.ord_passes, 0, sizeof g_gpu.ord_passes);
        memset(g_gpu.ord_copies, 0, sizeof g_gpu.ord_copies);
        memset(g_gpu.ord_px, 0, sizeof g_gpu.ord_px);
        pthread_mutex_unlock(&g_gl);
        g_win.ord_t0 = now;
        g_win.ord_frames = 0;
    }
    g_win.t0 = now;
    g_win.frames = g_win.ticks = 0;
    g_win.xr_wait = 0;
    g_win.max_frame = 0;
    __atomic_store_n(&g_win.fence, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_win.qsubmit, 0, __ATOMIC_RELAXED);
}

void kl_perf_frame(uint32_t ticks, int64_t xr_wait_ns) {
    if (!kl_perf_on()) return;
    __atomic_store_n(&g_ordinal, 0, __ATOMIC_RELAXED);
    int64_t now = perf_now_ns();
    if (!g_win.primed) {
        g_win.primed = 1;
        g_win.t0 = g_win.ord_t0 = g_win.last_frame = now;
        perf_top top[1];
        uint64_t role[KL_PERF_ROLE_N];
        (void)perf_threads(top, 1, role);
        return;
    }
    int64_t d = now - g_win.last_frame;
    if (d > g_win.max_frame) g_win.max_frame = d;
    g_win.last_frame = now;
    g_win.frames++;
    g_win.ord_frames++;
    g_win.ticks += ticks;
    g_pp_frame_no++;
    if (pp_on()) {
        static uint64_t pp_frames;
        static int64_t pp_mark;
        pp_frames++;
        if (!pp_mark) pp_mark = now;
        int64_t before = g_pp.t0;
        pp_emit(now, pp_frames);
        if (g_pp.t0 != before) pp_frames = 0;
    }
    g_win.xr_wait += xr_wait_ns > 0 ? xr_wait_ns : 0;
    static int64_t period = -1;
    if (period < 0) {
        float s = kl_env_float("KL_PERF_SECS", 1.0f);
        period = (int64_t)((s > 0.05f ? s : 1.0f) * 1e9);
    }
    if (now - g_win.t0 >= period) perf_emit(now);
}
