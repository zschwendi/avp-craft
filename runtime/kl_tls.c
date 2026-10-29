// S0.1b — per-thread TLS blocks for guest modules. See runtime/kl_tls.h for the
// model and why it is the same model as S0.1's thread-pointer rewrite.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "kl_tls.h"

// Sixteen was far past what any Android guest has: three libraries in that
// corpus carry a PT_TLS reached through TLSDESC. The Alyx payload has 27 (Valve
// builds every Source 2 module with C++ thread_locals), so 64. Registration
// REFUSES past the end rather than growing, because the per-thread array is
// indexed by the resolver's fast path with no bound check — a silently
// truncated table would be a wrong load address, and there is nothing to gain
// from a resizable one.
#define KL_TLS_MAX_MODS 64

uint32_t kl_tlsdesc_slot;              // the TSD key, read inline by the thunk

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static kl_tlsmod  g_mods[KL_TLS_MAX_MODS];
static unsigned   g_nmods;
static int        g_ready;             // kl_tls_init succeeded
static pthread_key_t g_key;

// Thread exit: free this thread's blocks. Darwin runs this with the value
// already cleared from the slot, so nothing can reach a freed block through
// the fast path afterwards.
static void tls_thread_free(void *p) {
    void **blocks = (void **)p;
    if (!blocks) return;
    for (unsigned i = 0; i < KL_TLS_MAX_MODS; i++) free(blocks[i]);
    free(blocks);
}

int kl_tls_init(void) {
    pthread_mutex_lock(&g_mu);
    if (g_ready) { pthread_mutex_unlock(&g_mu); return 0; }

    uint64_t tp;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    // Same check, and for the same reason, as kl_x18_init's: Darwin's os/tsd.h
    // masks the low three bits off as a CPU number. They measure zero on Apple
    // silicon, and BOTH sides of this mechanism depend on that — ours when it
    // indexes the TSD array, and the guest's when it adds our offset to its own
    // reading of the register. If they were ever non-zero AND cpu-dependent,
    // the guest could add an offset computed against a different TP and land a
    // few bytes off. Fail out loud rather than compute addresses from it.
    if (tp & 7) {
        fprintf(stderr, "  [klepton] tls: tpidrro_el0 has low bits set (%#llx) — "
                        "TP-relative TLS offsets are not trustworthy on this OS\n",
                (unsigned long long)tp);
        pthread_mutex_unlock(&g_mu);
        return -1;
    }

    if (pthread_key_create(&g_key, tls_thread_free) != 0) {
        fprintf(stderr, "  [klepton] tls: no pthread key available\n");
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    // The thunk reads [tp + key*8] directly rather than calling
    // pthread_getspecific, so prove the direct access agrees with the API on
    // THIS key before any descriptor points at it.
    pthread_setspecific(g_key, (void *)(uintptr_t)0x715C0DE5U);
    if (((void **)(uintptr_t)tp)[(unsigned)g_key] != (void *)(uintptr_t)0x715C0DE5U) {
        fprintf(stderr, "  [klepton] tls: pthread key %u is not at tsd[%u] on this OS\n",
                (unsigned)g_key, (unsigned)g_key);
        pthread_setspecific(g_key, NULL);
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    pthread_setspecific(g_key, NULL);

    kl_tlsdesc_slot = (uint32_t)g_key;
    g_ready = 1;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

kl_tlsmod *kl_tls_register(const char *path, const void *init,
                           uint64_t filesz, uint64_t memsz, uint64_t align) {
    if (kl_tls_init() != 0) return NULL;
    pthread_mutex_lock(&g_mu);
    if (g_nmods >= KL_TLS_MAX_MODS) {
        fprintf(stderr, "  [klepton] tls: more than %d modules with TLSDESC "
                        "relocations (%s)\n", KL_TLS_MAX_MODS, path ? path : "?");
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }
    kl_tlsmod *m = &g_mods[g_nmods];
    m->index  = g_nmods++;
    m->init   = filesz ? init : NULL;
    m->filesz = filesz;
    m->memsz  = memsz;
    m->align  = align < 16 ? 16 : align;
    m->path   = path;
    pthread_mutex_unlock(&g_mu);
    return m;
}

int64_t kl_tlsdesc_slow(const kl_tlsdesc_arg *a) {
    uint64_t tp;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    kl_tlsmod *m = a->mod;

    void **blocks = (void **)pthread_getspecific(g_key);
    if (!blocks) {
        blocks = (void **)calloc(KL_TLS_MAX_MODS, sizeof *blocks);
        if (!blocks) abort();
        pthread_setspecific(g_key, blocks);
    }
    uint8_t *b = (uint8_t *)blocks[m->index];
    if (!b) {
        // ELF semantics: filesz bytes from the initialisation image, the rest
        // zero. posix_memalign rather than malloc because a module may ask for
        // more than malloc's 16.
        void *p = NULL;
        if (posix_memalign(&p, (size_t)m->align, (size_t)m->memsz ? (size_t)m->memsz : 8) != 0)
            abort();
        b = (uint8_t *)p;
        if (m->filesz) memcpy(b, m->init, (size_t)m->filesz);
        memset(b + m->filesz, 0, (size_t)(m->memsz - m->filesz));
        blocks[m->index] = b;
    }
    return (int64_t)((uint64_t)(uintptr_t)(b + a->off) - tp);
}

// ---- static TLS (R_AARCH64_TLS_TPREL64) ----
//
// See kl_tls.h. The same run-of-keys placement the Alyx door gives the host's
// linker (kl_alyx.c alyx_static_tls), here for the runtime's own loader: Darwin
// hands keys out in order, so the first attempt is almost always one run, and a
// gap restarts it. A key is never freed — the block lives as long as the image.
//
// "Seeded" is tracked per thread in a key of its own (how many blocks this
// thread has had copied in), so the seed is idempotent: kl_thread_init runs on
// a thread more than once (the door's configure, then a guest thread's start),
// and a second copy would reset whatever the guest had stored by then —
// libmimalloc's _mi_heap_default, the very value this exists for. (The loader
// installs the seed as a kl_thread_init hook; this file stays free of the shim
// so t_tls can link it alone.)

typedef struct { const uint8_t *init; uint64_t filesz, memsz; unsigned slot0, nslots; } kl_stls;
static kl_stls       g_stls[8];
static unsigned      g_nstls;
static pthread_key_t g_stls_done;
static int           g_stls_done_ok;

void kl_tls_static_seed(void) {
    unsigned n = __atomic_load_n(&g_nstls, __ATOMIC_ACQUIRE);
    if (!n || !g_stls_done_ok) return;
    unsigned done = (unsigned)(uintptr_t)pthread_getspecific(g_stls_done);
    if (done >= n) return;
    uint64_t tp;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    for (unsigned i = done; i < n; i++) {
        uint8_t *blk = (uint8_t *)(uintptr_t)tp + 8u * g_stls[i].slot0;
        memset(blk, 0, 8u * g_stls[i].nslots);
        if (g_stls[i].filesz) memcpy(blk, g_stls[i].init, (size_t)g_stls[i].filesz);
    }
    pthread_setspecific(g_stls_done, (void *)(uintptr_t)n);
}

int64_t kl_tls_static_block(const char *path, const void *init, uint64_t filesz,
                            uint64_t memsz, uint64_t align) {
    pthread_mutex_lock(&g_mu);
    if (g_nstls == sizeof g_stls / sizeof *g_stls || align > 8 || !memsz) {
        fprintf(stderr, "  [klepton] static TLS: %s needs %llu bytes aligned %llu — NOT "
                        "served (table full, or alignment above a TSD slot's 8)\n",
                path ? path : "?", (unsigned long long)memsz, (unsigned long long)align);
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    if (!g_stls_done_ok) {
        if (pthread_key_create(&g_stls_done, NULL) != 0) {
            pthread_mutex_unlock(&g_mu);
            return -1;
        }
        g_stls_done_ok = 1;
    }
    unsigned need = (unsigned)((memsz + 7) / 8), have = 0, first = 0;
    for (unsigned tries = 0; have < need && tries < 64; tries++) {
        pthread_key_t k;
        if (pthread_key_create(&k, NULL) != 0) break;
        if (have && (unsigned)k == first + have) have++;
        else { first = (unsigned)k; have = 1; }
    }
    if (have < need) {
        fprintf(stderr, "  [klepton] static TLS: no run of %u consecutive TSD slots for %s\n",
                need, path ? path : "?");
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    kl_stls *st = &g_stls[g_nstls];
    st->init = init; st->filesz = filesz; st->memsz = memsz;
    st->slot0 = first; st->nslots = need;
    __atomic_store_n(&g_nstls, g_nstls + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "  [klepton] static TLS: %s — %llu bytes in TSD slots %u..%u (TP+%#x) "
                    "on every thread\n", path ? path : "?", (unsigned long long)memsz,
            first, first + need - 1, 8u * first);
    return (int64_t)(8u * first);
}
