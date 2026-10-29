/* Darwin GL layer, section 8: buffer uploads over ANGLE Metal (launch-12 stall round, 2026-10-13).
 * Compiled with upstream's include paths (context_t, es3_functions, host_eglGetProcAddress), like ltw_darwin_gl.c.
 *
 *  THE PROBLEM. Minecraft 26.2 (no ARB_buffer_storage here -> Sodium 0.9.2's MojangStagingBuffer "Fallback")
 *  uploads every chunk-section mesh with glBindBuffer + glBufferSubData straight into a Sodium ARENA buffer
 *  (tens of MB, shared by many sections, read by every terrain draw in flight). ANGLE Metal's
 *  BufferMtl::setSubDataImpl, for a buffer the GPU is still using, takes putDataInNewBufferAndStartUsingNewBuffer:
 *  a NEW Metal buffer of the WHOLE arena size + a GPU blit of everything around the few KB that changed. So one
 *  10 KB mesh upload = one arena-sized allocation + an arena-sized copy. A terrain storm uploads hundreds per
 *  frame (Sodium's budget is 25.6 MB/frame), and two ANGLE heuristics then cut the frame apart:
 *    - BufferManager::getBuffer: > 64 MB allocated since its last GC with buffers in flight ->
 *      flushCommandBuffer(WaitUntilFinished) = the render thread waits for the GPU to drain (a full CPU/GPU sync);
 *    - CommandBuffer::needsFlushForDrawCallLimits: > 400 MB of distinct resources referenced -> commit mid-frame.
 *  Both split the frame into many short command buffers and break render passes (load/store of the eye targets).
 *
 *  THE FIX (LTW_DARWIN_SUBDATA=stage, default): glBufferSubData into a buffer of >= LTW_DARWIN_STAGE_MIN bytes
 *  (default 1 MiB; GL_BUFFER_SIZE of the bound buffer) is written into an ltw-owned STAGING block instead and
 *  copied on the GPU with glCopyBufferSubData (GPU-ordered, exactly like desktop GL's mapped-staging path):
 *    - staging = one GL buffer per context, used as a ring of LTW_DARWIN_STAGE_BLOCK-byte blocks (default 4 MiB);
 *      data is appended at a cursor through glMapBufferRange(WRITE | UNSYNCHRONIZED | INVALIDATE_RANGE): never
 *      overwrites bytes a pending copy still reads (append-only within a block);
 *    - a full block is orphaned with glBufferData(block, NULL): ANGLE returns the old Metal buffer to its pool
 *      (reused only once the GPU is done with it) and hands back a same-size pooled one -> steady state allocates
 *      nothing, so neither ANGLE heuristic fires;
 *    - an upload larger than a block gets its own glBufferData(size, data);
 *    - ANGLE's copySubData is a ranged blit (only the uploaded bytes); consecutive uploads share one blit encoder.
 *  Smaller buffers (vanilla's 512 KB transient blocks, UBOs) keep ANGLE's own path: a mid-pass blit would end the
 *  render encoder each time, while their copy-on-write is small and pool-recycled.
 *  Unaligned offsets/sizes (Metal blits want 4-byte multiples) and calls with no current ltw context pass through.
 *  LTW_DARWIN_SUBDATA=off (or 0) = upstream behaviour (ANGLE's copy-on-write).
 *  Proof line, once per run on the first staged upload:
 *    "LTW-Darwin: buffer uploads >= N bytes staged through a M-byte ring (glCopyBufferSubData) ..."
 *
 *  STATS (LTW_DARWIN_BUFSTATS=1, or =<seconds>): one "[ltw-buf]" line per window on stderr counting, per frame,
 *  the buffer-traffic GL calls (glBufferSubData split by destination size: small / big-direct / big-staged, bytes,
 *  and the destination bytes ANGLE's copy-on-write would have copied), glBufferData, glMapBufferRange
 *  (unsynchronized or not), glCopyBufferSubData, glFenceSync, glClientWaitSync (+ ms blocked), glFlush, glFinish.
 *  Frames = eglSwapBuffers seen through ltw's eglGetProcAddress (host/flat path; 0 in VR, where the line is per
 *  second). LTW_DARWIN_FRAMELOG=<file> appends one "<t_s> <frame_ms>" line per eglSwapBuffers (host A/B tooling).
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <EGL/egl.h>
#include "proc.h"
#include "egl.h"

#define HIDDEN __attribute__((visibility("hidden")))

#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#endif
#ifndef GL_COPY_READ_BUFFER_BINDING
#define GL_COPY_READ_BUFFER_BINDING 0x8F36
#define GL_COPY_WRITE_BUFFER_BINDING 0x8F37
#endif

/* ---- knobs ------------------------------------------------------------------------------------- */
static int        k_mode = -1;          /* 0 off, 1 stage */
static GLsizeiptr k_min, k_block;
static int        k_stats_s;            /* 0 = no stats line */
static FILE      *k_framelog;

static GLsizeiptr env_bytes(const char *name, GLsizeiptr dflt) {
    const char *e = getenv(name);
    if (!e || !*e) return dflt;
    char *end; double v = strtod(e, &end);
    if (*end == 'k' || *end == 'K') v *= 1024; else if (*end == 'm' || *end == 'M') v *= 1024 * 1024;
    return v > 0 ? (GLsizeiptr)v : dflt;
}

static void buf_init_once(void) {
    const char *m = getenv("LTW_DARWIN_SUBDATA");
    k_mode = (m && (!strcmp(m, "0") || !strcasecmp(m, "off") || !strcasecmp(m, "angle"))) ? 0 : 1;
    k_min   = env_bytes("LTW_DARWIN_STAGE_MIN", 1 << 20);
    k_block = env_bytes("LTW_DARWIN_STAGE_BLOCK", 4 << 20);
    const char *s = getenv("LTW_DARWIN_BUFSTATS");
    k_stats_s = s && *s && strcmp(s, "0") ? (atoi(s) > 1 ? atoi(s) : 1) : 0;
    const char *f = getenv("LTW_DARWIN_FRAMELOG");
    if (f && *f) { k_framelog = fopen(f, "a"); if (k_framelog) setvbuf(k_framelog, NULL, _IOLBF, 1 << 12);   /* line-buffered: the host harness kill -9s the game */ }
    fprintf(stderr, "LTW-Darwin: buffer uploads: LTW_DARWIN_SUBDATA=%s (stage min %ld, block %ld)%s%s\n",
            k_mode ? "stage" : "off", (long)k_min, (long)k_block,
            k_stats_s ? ", [ltw-buf] stats on" : "", k_framelog ? ", frame log on" : "");
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static inline void buf_init(void) { if (k_mode < 0) pthread_once(&g_once, buf_init_once); }

/* ---- stats ------------------------------------------------------------------------------------- */
enum { C_SUB_SMALL, C_SUB_BIG, C_SUB_STAGED, C_SUB_BYTES, C_SUB_COW_BYTES, C_STAGE_BLOCKS, C_STAGE_SOLO,
       C_DATA, C_DATA_BYTES, C_MAP, C_MAP_UNSYNC, C_UNMAP, C_FLUSHMAP, C_COPY, C_COPY_BYTES,
       C_FENCE, C_CWAIT, C_CWAIT_NS, C_FLUSH, C_FINISH, C_FRAMES, C_FRAME_MAX_NS, C_N };
static _Atomic uint64_t g_c[C_N];
static _Atomic int64_t  g_t0, g_last_swap;
static int64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000LL + ts.tv_nsec; }
static inline void cnt(int i, uint64_t v) { if (k_stats_s) atomic_fetch_add_explicit(&g_c[i], v, memory_order_relaxed); }

static void stats_tick(void) {
    if (!k_stats_s) return;
    int64_t now = now_ns(), t0 = atomic_load(&g_t0);
    if (!t0) { atomic_compare_exchange_strong(&g_t0, &t0, now); return; }
    if (now - t0 < (int64_t)k_stats_s * 1000000000LL) return;
    if (!atomic_compare_exchange_strong(&g_t0, &t0, now)) return;      /* one thread prints */
    uint64_t c[C_N];
    for (int i = 0; i < C_N; i++) c[i] = atomic_exchange(&g_c[i], 0);
    double secs = (now - t0) / 1e9, f = c[C_FRAMES] ? (double)c[C_FRAMES] : secs;
    const char *per = c[C_FRAMES] ? "f" : "s";
    fprintf(stderr, "[ltw-buf] %.2f s, %llu frames (max %.1f ms) | per %s: subdata small %.1f big %.1f staged %.1f "
                    "(%.2f MB; ANGLE copy-on-write would copy %.1f MB) stage blocks %.2f solo %.2f | bufferData %.1f "
                    "(%.2f MB) | map %.1f (unsync %.1f) unmap %.1f flushmap %.1f | copy %.1f (%.2f MB) | fence %.1f "
                    "wait %.1f (%.2f ms) flush %.1f finish %.2f\n",
            secs, (unsigned long long)c[C_FRAMES], c[C_FRAME_MAX_NS] / 1e6, per,
            c[C_SUB_SMALL] / f, c[C_SUB_BIG] / f, c[C_SUB_STAGED] / f, c[C_SUB_BYTES] / 1e6 / f,
            c[C_SUB_COW_BYTES] / 1e6 / f, c[C_STAGE_BLOCKS] / f, c[C_STAGE_SOLO] / f, c[C_DATA] / f,
            c[C_DATA_BYTES] / 1e6 / f, c[C_MAP] / f, c[C_MAP_UNSYNC] / f, c[C_UNMAP] / f, c[C_FLUSHMAP] / f,
            c[C_COPY] / f, c[C_COPY_BYTES] / 1e6 / f, c[C_FENCE] / f, c[C_CWAIT] / f, c[C_CWAIT_NS] / 1e6 / f,
            c[C_FLUSH] / f, c[C_FINISH] / f);
}

/* ---- staging ----------------------------------------------------------------------------------- */
static _Thread_local struct { const void *ctx; GLuint buf; GLsizeiptr cur, cap; } st;

static bool stage_upload(GLenum target, GLintptr off, GLsizeiptr n, const void *data) {
    if (st.ctx != (const void *)current_context) {
        st.ctx = current_context; st.buf = 0; st.cur = st.cap = 0;
        es3_functions.glGenBuffers(1, &st.buf);
        if (!st.buf) return false;
    }
    GLenum sb = target == GL_COPY_READ_BUFFER ? GL_COPY_WRITE_BUFFER : GL_COPY_READ_BUFFER;
    GLint prev = 0;
    es3_functions.glGetIntegerv(sb == GL_COPY_READ_BUFFER ? GL_COPY_READ_BUFFER_BINDING : GL_COPY_WRITE_BUFFER_BINDING, &prev);
    es3_functions.glBindBuffer(sb, st.buf);
    GLintptr src;
    if (n > k_block) {                                   /* oversized: its own allocation */
        es3_functions.glBufferData(sb, n, data, GL_STREAM_DRAW);
        st.cur = st.cap = 0;                             /* the block is gone; next call starts a new one */
        src = 0;
        cnt(C_STAGE_SOLO, 1);
    } else {
        if (st.cur + n > st.cap) {                       /* orphan: ANGLE recycles the old block after the GPU */
            es3_functions.glBufferData(sb, k_block, NULL, GL_STREAM_DRAW);
            st.cur = 0; st.cap = k_block;
            cnt(C_STAGE_BLOCKS, 1);
        }
        src = st.cur;
        void *p = es3_functions.glMapBufferRange(sb, src, n,
                                                 GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (p) { memcpy(p, data, (size_t)n); es3_functions.glUnmapBuffer(sb); }
        else es3_functions.glBufferSubData(sb, src, n, data);
        st.cur += (n + 255) & ~(GLsizeiptr)255;
    }
    es3_functions.glCopyBufferSubData(sb, target, src, off, n);
    es3_functions.glBindBuffer(sb, (GLuint)prev);
    return true;
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    buf_init();
    stats_tick();
    if (size <= 0 || !data || !current_context) { es3_functions.glBufferSubData(target, offset, size, data); return; }
    GLint dst = 0;
    es3_functions.glGetBufferParameteriv(target, GL_BUFFER_SIZE, &dst);
    cnt(C_SUB_BYTES, (uint64_t)size);
    if (dst < k_min) { cnt(C_SUB_SMALL, 1); es3_functions.glBufferSubData(target, offset, size, data); return; }
    cnt(C_SUB_COW_BYTES, (uint64_t)dst);
    if (k_mode == 1 && !(offset & 3) && !(size & 3) && stage_upload(target, offset, size, data)) {
        static atomic_int said;
        if (!atomic_exchange(&said, 1))
            fprintf(stderr, "LTW-Darwin: buffer uploads >= %ld bytes staged through a %ld-byte ring "
                            "(glCopyBufferSubData) instead of ANGLE's whole-buffer copy-on-write; first: %ld bytes into a "
                            "%d-byte buffer (LTW_DARWIN_SUBDATA=off restores ANGLE's path)\n",
                    (long)k_min, (long)k_block, (long)size, dst);
        cnt(C_SUB_STAGED, 1);
        return;
    }
    cnt(C_SUB_BIG, 1);
    es3_functions.glBufferSubData(target, offset, size, data);
}

/* ---- counted pass-throughs (only routed when stats are on) -------------------------------------- */
static void c_BufferData(GLenum t, GLsizeiptr s, const void *d, GLenum u) {
    stats_tick(); cnt(C_DATA, 1); cnt(C_DATA_BYTES, s > 0 ? (uint64_t)s : 0); es3_functions.glBufferData(t, s, d, u);
}
static void *c_MapBufferRange(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a) {
    cnt(C_MAP, 1); if (a & GL_MAP_UNSYNCHRONIZED_BIT) cnt(C_MAP_UNSYNC, 1);
    return es3_functions.glMapBufferRange(t, o, l, a);
}
static GLboolean c_UnmapBuffer(GLenum t) { cnt(C_UNMAP, 1); return es3_functions.glUnmapBuffer(t); }
static void c_FlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l) {
    cnt(C_FLUSHMAP, 1); es3_functions.glFlushMappedBufferRange(t, o, l);
}
static void c_CopyBufferSubData(GLenum r, GLenum w, GLintptr ro, GLintptr wo, GLsizeiptr s) {
    cnt(C_COPY, 1); cnt(C_COPY_BYTES, s > 0 ? (uint64_t)s : 0); es3_functions.glCopyBufferSubData(r, w, ro, wo, s);
}
static GLsync c_FenceSync(GLenum c, GLbitfield f) { cnt(C_FENCE, 1); return es3_functions.glFenceSync(c, f); }
static void c_Flush(void) { cnt(C_FLUSH, 1); es3_functions.glFlush(); }
static void c_Finish(void) { cnt(C_FINISH, 1); es3_functions.glFinish(); }

/* glClientWaitSync lives in ltw_darwin_gl.c (section 4); it reports here. */
HIDDEN void ltw_darwin_buf_note_wait(int64_t ns) { cnt(C_CWAIT, 1); cnt(C_CWAIT_NS, ns > 0 ? (uint64_t)ns : 0); }
HIDDEN int ltw_darwin_buf_stats_on(void) { buf_init(); return k_stats_s; }
HIDDEN int64_t ltw_darwin_buf_now(void) { return now_ns(); }

static EGLBoolean (*real_SwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean c_SwapBuffers(EGLDisplay d, EGLSurface s) {
    EGLBoolean r = real_SwapBuffers(d, s);
    int64_t now = now_ns(), last = atomic_exchange(&g_last_swap, now);
    if (last) {
        int64_t dt = now - last;
        cnt(C_FRAMES, 1);
        if (k_stats_s) {
            uint64_t m = atomic_load(&g_c[C_FRAME_MAX_NS]);
            while ((uint64_t)dt > m && !atomic_compare_exchange_weak(&g_c[C_FRAME_MAX_NS], &m, (uint64_t)dt)) {}
        }
        if (k_framelog) fprintf(k_framelog, "%.4f %.3f\n", now / 1e9, dt / 1e6);
    }
    stats_tick();
    return r;
}

/* ---- lookup (called first by ltw_darwin_gl.c's eglGetProcAddress) ------------------------------- */
HIDDEN void *ltw_darwin_buf_lookup(const char *name) {
    buf_init();
    if (!strcmp(name, "glBufferSubData")) return (void *)glBufferSubData;
    if (!strcmp(name, "eglSwapBuffers") && (k_stats_s || k_framelog)) {
        if (!real_SwapBuffers) real_SwapBuffers = (EGLBoolean (*)(EGLDisplay, EGLSurface))host_eglGetProcAddress("eglSwapBuffers");
        return real_SwapBuffers ? (void *)c_SwapBuffers : NULL;
    }
    if (!k_stats_s || name[0] != 'g') return NULL;
#define C(n) if (!strcmp(name, "gl" #n)) return (void *)c_##n;
    C(BufferData) C(MapBufferRange) C(UnmapBuffer) C(FlushMappedBufferRange) C(CopyBufferSubData)
    C(FenceSync) C(Flush) C(Finish)
#undef C
    return NULL;
}
