/* Darwin GL layer over upstream ltw (Follow-up 1). Compiled with upstream's include paths so it
 * sees ltw's context_t / es3_functions / host_eglGetProcAddress.
 *
 *  1. Base-vertex draws on ES 3.0. Upstream handles glDrawElementsBaseVertex and
 *     glMultiDrawElementsBaseVertex only on ES 3.2 (core entry point) or ES 3.1 (indirect-draw
 *     emulation); on ES 3.0 (ANGLE on Apple) they are silently dropped. ANGLE's ES 3.0 context
 *     exposes GL_EXT_draw_elements_base_vertex / GL_OES_draw_elements_base_vertex, so:
 *        ES 3.2                     -> host core entry points
 *        EXT (or OES) extension     -> gl*BaseVertexEXT (or *OES); multi-draw = a loop
 *        otherwise                  -> EMULATION (below), unless LTW_DARWIN_BASEVERTEX_EMU=0
 *        emulation off              -> upstream (ES 3.1 indirect path, or its silent drop)
 *     The emulation exists because the visionOS SIMULATOR's ANGLE (Metal, "Apple xrOS simulator
 *     GPU") advertises NEITHER extension (M2 terrain bug: Sodium's chunk draws were all dropped).
 *     It is the standard trick: for a draw with basevertex B, every enabled, non-instanced vertex
 *     attrib pointer of the bound VAO is re-specified at offset + B * stride, the plain
 *     glDrawElements[Instanced] is issued, and the pointers are put back. The VAO's attrib layout
 *     is read with glGetVertexAttrib* and cached per (context, VAO); every attrib-changing entry
 *     point (glVertexAttrib[I]Pointer, glEnable/DisableVertexAttribArray, glVertexAttribDivisor,
 *     glDeleteVertexArrays) is wrapped here and bumps a generation that invalidates the cache.
 *     Covers glDrawElementsBaseVertex, glDrawRangeElementsBaseVertex,
 *     glDrawElementsInstancedBaseVertex, glMultiDrawElementsBaseVertex (all four are what
 *     LWJGL's GLCapabilities.check_GL32 requires). LTW_DARWIN_BASEVERTEX_EXT=0 disables the
 *     extension route; LTW_DARWIN_BASEVERTEX_EMU=0 disables the emulation (both = upstream
 *     behaviour; `make test`'s negative pass). LTW_DARWIN_BASEVERTEX_EMU=1 with _EXT=0 forces
 *     the emulation where the extension exists (the macOS test's emulation pass).
 *
 *  2. GL version reported to LWJGL. Minecraft 1.21.5 asks GLFW for a 3.2 core context and
 *     LWJGL's GLCapabilities sets OpenGL31/32/33 from GL_MAJOR_VERSION/GL_MINOR_VERSION. On Quest
 *     ltw says 3.2 because the ES there is 3.2; over ANGLE it would say 3.0. With
 *     LTW_REPORT_GL_VERSION="M.N" (default "3.2"; "0", "off" or "" = upstream behaviour):
 *        glGetIntegerv(GL_MAJOR_VERSION / GL_MINOR_VERSION) -> M / N
 *        glGetIntegerv(GL_CONTEXT_PROFILE_MASK)             -> GL_CONTEXT_CORE_PROFILE_BIT
 *            (LWJGL queries it whenever the version is >= 3.2; ES has no such enum, so without
 *             this ANGLE would queue GL_INVALID_ENUM right after createCapabilities)
 *        glGetString(GL_VERSION)                            -> "M.N OpenLTW"
 *     ltw's internal ES detection (egl.c find_esversion) reads the RAW host strings through
 *     es3_functions and is untouched: it still sees ES 3.0.
 *
 *  4. glClientWaitSync always flushes (M3 stage A, Minecraft 1.21.11). 1.21.6+'s GlFence /
 *     MappableRingBuffer (GUI renderer) calls glClientWaitSync(fence, 0, Long.MAX_VALUE) on a fence
 *     it created earlier in the SAME frame and never flushed. Without GL_SYNC_FLUSH_COMMANDS_BIT
 *     the ES spec allows an infinite wait, and ANGLE (Metal) does exactly that: the first frame
 *     hung in nglClientWaitSync forever (logs/m3a-jvm-hang.jstack.txt). Desktop drivers flush
 *     implicitly. Fix: OR in GL_SYNC_FLUSH_COMMANDS_BIT (LTW_DARWIN_SYNC_FLUSH=0 = upstream).
 *
 *  5. Texture buffers on ES 3.0 (M3 stage A, Minecraft 1.21.11). 1.21.6+'s cloud pipeline
 *     (core/rendertype_clouds.vsh) reads its cell list from `uniform isamplerBuffer CloudFaces` with
 *     texelFetch, fed by glBindTexture(GL_TEXTURE_BUFFER) + glTexBuffer per draw. ES 3.0 has neither
 *     (ANGLE: "'isamplerBuffer' : Illegal use of reserved word"), and Minecraft treats the clouds
 *     pipeline as REQUIRED, so the failure rolled back every resource reload. Emulation:
 *       shader : glShaderSource rewrites `[iu]samplerBuffer N` -> `highp [iu]sampler2D N` and every
 *                `texelFetch(N, i)` -> `ltw_tbf_N(i)` = texelFetch(N, ivec2(i % 2048, i / 2048), 0)
 *                (desktop source, before upstream's GLSL conversion; upstream glShaderSource is renamed
 *                by darwin/ltw_darwin_rename_shader.h);
 *       bind   : glBindTexture(GL_TEXTURE_BUFFER, t) binds t as GL_TEXTURE_2D on the active unit and
 *                remembers the unit's previous 2D binding;
 *       data   : glTexBuffer / glTexBufferRange (re)allocate t as a 2048-wide 2D texture of the same
 *                internal format and copy the buffer into it ON THE GPU (buffer bound as
 *                GL_PIXEL_UNPACK_BUFFER, glTexSubImage2D from offsets; full rows + one partial row),
 *                NEAREST / MAX_LEVEL 0 (integer textures must not be filtered), sampler 0 on the unit;
 *       restore: after the next draw, the unit's previous 2D binding is put back, because Minecraft's
 *                GlStateManager caches per-unit 2D bindings and would otherwise skip a rebind.
 *     LTW_DARWIN_TEXBUFFER=0 = upstream behaviour (shader fails, glTexBuffer = ANGLE's ES 3.2 entry).
 *
 *  8. Buffer uploads (darwin/ltw_darwin_buf.c): big glBufferSubData staged through a ring + glCopyBufferSubData
 *     (LTW_DARWIN_SUBDATA), [ltw-buf] counters (LTW_DARWIN_BUFSTATS), host frame log (LTW_DARWIN_FRAMELOG).
 *
 *  3. Lookup. eglGetProcAddress / glXGetProcAddress (and OSMesaGetProcAddress in ltw_darwin.c)
 *     answer the names above with these functions, then defer to upstream's table
 *     (ltw_upstream_eglGetProcAddress). Upstream never returned glDrawElementsBaseVertex from its
 *     table (it fell through to ANGLE's raw ES 3.2 entry point).
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "proc.h"
#include "egl.h"

#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#endif
#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#endif

/* upstream, renamed by darwin/ltw_darwin_rename_*.h and darwin/ltw_darwin_proc.h */
eglMustCastToProperFunctionPointerType ltw_upstream_eglGetProcAddress(const char *procname);
const GLubyte *ltw_upstream_glGetString(GLenum name);
void ltw_upstream_glGetIntegerv(GLenum pname, GLint *data);
void ltw_upstream_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                           const void *indices, GLint basevertex);
void ltw_upstream_glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count, GLenum type,
                                                const void *const *indices, GLsizei drawcount,
                                                const GLint *basevertex);

#define LOG_ONCE(...) do { static atomic_bool once_; \
        if (!atomic_exchange(&once_, true)) printf(__VA_ARGS__); } while (0)

/* ---- 2. reported GL version -------------------------------------------------------------------- */
static int rep_major = -1, rep_minor = -1; /* -1: not parsed yet; 0/0: disabled */
static char rep_string[32];

static void rep_init(void) {
    if (rep_major >= 0) return;
    const char *env = getenv("LTW_REPORT_GL_VERSION");
    int maj = 3, min = 2;
    if (env) {
        if (!*env || !strcmp(env, "0") || !strcasecmp(env, "off") || !strcasecmp(env, "no")) {
            maj = min = 0;
        } else if (sscanf(env, "%d.%d", &maj, &min) != 2 || maj < 1 || min < 0) {
            printf("LTW-Darwin: LTW_REPORT_GL_VERSION=\"%s\" is not \"M.N\"; using 3.2\n", env);
            maj = 3; min = 2;
        }
    }
    if (maj) snprintf(rep_string, sizeof rep_string, "%d.%d OpenLTW", maj, min);
    rep_minor = min;
    rep_major = maj; /* written last: a racing reader sees -1 and re-parses (same result) */
}

static inline bool rep_on(void) {
    if (rep_major < 0) rep_init();
    return rep_major > 0;
}

/* section 7 (ltw_darwin_gl33.c) */
__attribute__((visibility("hidden"))) const GLubyte *ltw_d33_extensions_string(void);
__attribute__((visibility("hidden"))) bool ltw_d33_num_extensions(GLint *data);
__attribute__((visibility("hidden"))) void *ltw_d33_lookup(const char *name);

__attribute__((visibility("hidden"))) bool ltw_darwin_report_version(int *maj, int *min) {
    if (!rep_on()) return false;
    *maj = rep_major; *min = rep_minor;
    return true;
}
static inline bool rep_33(void) { return rep_on() && (rep_major > 3 || (rep_major == 3 && rep_minor >= 3)); }
static char rep_glsl[32];

const GLubyte *glGetString(GLenum name) {
    if (current_context && name == GL_VERSION && rep_on()) return (const GLubyte *)rep_string;
    /* GL 3.3+ report (Minecraft 26.x): GLSL "3.30 OpenLTW" (a 3.3 context's GLSL is 3.30). 3.2 mode keeps
     * upstream's "4.60 LTW" (1.21.x unchanged). */
    if (current_context && name == GL_SHADING_LANGUAGE_VERSION && rep_33()) {
        if (!rep_glsl[0]) snprintf(rep_glsl, sizeof rep_glsl, "%d.%d0 OpenLTW", rep_major, rep_minor);
        return (const GLubyte *)rep_glsl;
    }
    if (current_context && name == GL_EXTENSIONS) {
        const GLubyte *f = ltw_d33_extensions_string();   /* section 7b: hidden ARB names removed */
        if (f) return f;
    }
    return ltw_upstream_glGetString(name);
}

#ifndef GL_CONTEXT_FLAGS
#define GL_CONTEXT_FLAGS 0x821E
#endif
void glGetIntegerv(GLenum pname, GLint *data) {
    if (current_context && data && rep_on()) {
        switch (pname) {
            case GL_MAJOR_VERSION: *data = rep_major; return;
            case GL_MINOR_VERSION: *data = rep_minor; return;
            case GL_CONTEXT_PROFILE_MASK: *data = GL_CONTEXT_CORE_PROFILE_BIT; return;
        }
        /* GL 3.3 mode: GL_CONTEXT_FLAGS is a desktop 3.0 enum (ES 3.2 only); on ANGLE ES 3.0 it queued
         * GL_INVALID_ENUM inside LWJGL 3.4's GL.createCapabilities (which queries it at 3.0+). No flags: not debug,
         * not forward-compatible (LWJGL then reads the CORE profile mask and still treats the context as
         * forward-compatible). 3.2 mode (1.21.x, LWJGL 3.3.3 never asks) is left to upstream. */
        if (pname == GL_CONTEXT_FLAGS && rep_33()) { *data = 0; return; }
    }
    if (current_context && data && pname == GL_NUM_EXTENSIONS && ltw_d33_num_extensions(data)) return;
    ltw_upstream_glGetIntegerv(pname, data);
}

/* ---- 1. base-vertex draws ---------------------------------------------------------------------- */
typedef void (*dev_t_)(GLenum, GLsizei, GLenum, const void *, GLint);
typedef void (*drev_t)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint);
typedef void (*deibv_t)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLint);

enum { BV_UPSTREAM = 0, BV_CORE, BV_EXT, BV_EMU };
typedef struct {
    const context_t *ctx;   /* cache key: ltw context + its host context */
    EGLContext phys;
    int route;
    dev_t_ draw;
    drev_t range;
    deibv_t inst;
} bv_route_t;
static thread_local bv_route_t bv;

static bool has_ext(const char *list, const char *name) {
    if (!list) return false;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += n) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
    }
    return false;
}

static const bv_route_t *bv_get(void) {
    context_t *c = current_context;
    if (bv.ctx == c && bv.phys == c->phys_context) return &bv;
    memset(&bv, 0, sizeof bv);
    bv.ctx = c;
    bv.phys = c->phys_context;
    const char *sfx = NULL;
    if (c->es32) {
        bv.route = BV_CORE;
        sfx = "";
    } else {
        const char *env = getenv("LTW_DARWIN_BASEVERTEX_EXT");
        bool allowed = !(env && (!strcmp(env, "0") || !strcasecmp(env, "off")));
        const char *ext = (const char *)es3_functions.glGetString(GL_EXTENSIONS); /* raw host list */
        if (allowed && has_ext(ext, "GL_EXT_draw_elements_base_vertex")) sfx = "EXT";
        else if (allowed && has_ext(ext, "GL_OES_draw_elements_base_vertex")) sfx = "OES";
        if (sfx) bv.route = BV_EXT;
    }
    if (sfx) {
        char n[64];
        snprintf(n, sizeof n, "glDrawElementsBaseVertex%s", sfx);
        bv.draw = (dev_t_)host_eglGetProcAddress(n);
        snprintf(n, sizeof n, "glDrawRangeElementsBaseVertex%s", sfx);
        bv.range = (drev_t)host_eglGetProcAddress(n);
        snprintf(n, sizeof n, "glDrawElementsInstancedBaseVertex%s", sfx);
        bv.inst = (deibv_t)host_eglGetProcAddress(n);
        if (!bv.draw) { bv.route = BV_UPSTREAM; sfx = NULL; }
    }
    if (bv.route == BV_UPSTREAM && !c->es31) {   /* ES 3.1 keeps upstream's indirect path */
        const char *emu = getenv("LTW_DARWIN_BASEVERTEX_EMU");
        if (!(emu && (!strcmp(emu, "0") || !strcasecmp(emu, "off")))) bv.route = BV_EMU;
    }
    printf("LTW-Darwin: base-vertex draws use %s\n",
           bv.route == BV_CORE ? "ES 3.2 core entry points"
           : bv.route == BV_EXT ? (sfx[0] == 'E' ? "GL_EXT_draw_elements_base_vertex"
                                                 : "GL_OES_draw_elements_base_vertex")
           : bv.route == BV_EMU ? "EMULATION (attrib-pointer offset; host has no base-vertex extension)"
           : "upstream ltw (ES 3.1 indirect emulation, or dropped)");
    return &bv;
}


/* ---- 1b. base-vertex emulation (ES 3.0 without EXT/OES) ---------------------------------------- */
#define EMU_MAX_ATTRIBS 16
#define EMU_CACHE 32
typedef struct {
    GLuint index, buffer;
    GLint size;
    GLenum type;
    GLboolean norm, integer;
    GLsizei stride;      /* as specified (0 = tightly packed) */
    GLsizei step;        /* bytes per vertex: stride, or the packed element size */
    uintptr_t ptr;
} emu_attr_t;
typedef struct {
    bool valid;
    GLuint vao;
    unsigned gen;
    int n;
    emu_attr_t a[EMU_MAX_ATTRIBS];
} emu_layout_t;
static atomic_uint emu_gen = 1;       /* bumped by every attrib-changing call (any context) */
static thread_local struct {
    const context_t *ctx;
    EGLContext phys;
    GLint max_attribs;
    emu_layout_t cache[EMU_CACHE];
} emu;

static GLsizei emu_type_bytes(GLenum type, GLint size) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return size;
        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2 * size;
        case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
        default: return 4 * size; /* GL_FLOAT, GL_INT, GL_UNSIGNED_INT, GL_FIXED */
    }
}

static const emu_layout_t *emu_layout(void) {
    context_t *c = current_context;
    if (emu.ctx != c || emu.phys != c->phys_context) {
        memset(&emu, 0, sizeof emu);
        emu.ctx = c;
        emu.phys = c->phys_context;
        es3_functions.glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &emu.max_attribs);
        if (emu.max_attribs > EMU_MAX_ATTRIBS || emu.max_attribs <= 0) emu.max_attribs = EMU_MAX_ATTRIBS;
    }
    GLint vao = 0;
    es3_functions.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    unsigned gen = atomic_load_explicit(&emu_gen, memory_order_relaxed);
    emu_layout_t *l = &emu.cache[(GLuint)vao % EMU_CACHE];
    if (l->valid && l->vao == (GLuint)vao && l->gen == gen) return l;
    l->valid = true;
    l->vao = (GLuint)vao;
    l->gen = gen;
    l->n = 0;
    for (GLint i = 0; i < emu.max_attribs; i++) {
        GLint v = 0;
        es3_functions.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &v);
        if (!v) continue;
        v = 0;
        es3_functions.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &v);
        if (v) continue;                         /* per-instance: basevertex does not apply */
        emu_attr_t *a = &l->a[l->n++];
        GLint t;
        a->index = i;
        es3_functions.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
        es3_functions.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &t); a->type = t;
        es3_functions.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &t); a->norm = t != 0;
        es3_functions.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &t); a->integer = t != 0;
        es3_functions.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &t); a->stride = t;
        es3_functions.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &t); a->buffer = t;
        void *p = NULL;
        es3_functions.glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &p);
        a->ptr = (uintptr_t)p;
        a->step = a->stride ? a->stride : emu_type_bytes(a->type, a->size);
    }
    return l;
}

/* Point every attrib at vertex `bv`; returns the ARRAY_BUFFER left bound. */
static GLuint emu_point(const emu_layout_t *l, GLint bv, GLuint bound) {
    for (int i = 0; i < l->n; i++) {
        const emu_attr_t *a = &l->a[i];
        if (a->buffer != bound) { es3_functions.glBindBuffer(GL_ARRAY_BUFFER, a->buffer); bound = a->buffer; }
        const void *p = (const void *)(a->ptr + (intptr_t)bv * a->step);
        if (a->integer) es3_functions.glVertexAttribIPointer(a->index, a->size, a->type, a->stride, p);
        else es3_functions.glVertexAttribPointer(a->index, a->size, a->type, a->norm, a->stride, p);
    }
    return bound;
}

/* instances < 0: non-instanced draw. Multi-draw: n entries. */
static void emu_draw(GLenum mode, const GLsizei *count, GLenum type, const void *const *indices,
                     GLsizei n, const GLint *basevertex, GLsizei instances) {
    const emu_layout_t *l = NULL;
    GLuint array_buffer = current_context->bound_buffers[0];  /* GL_ARRAY_BUFFER, ltw-tracked */
    GLuint bound = array_buffer;
    GLint cur = 0;                                            /* what the attribs point at now */
    for (GLsizei i = 0; i < n; i++) {
        if (count[i] <= 0) continue;
        if (basevertex[i] != cur) {
            if (!l) l = emu_layout();
            bound = emu_point(l, basevertex[i], bound);
            cur = basevertex[i];
        }
        if (instances < 0) es3_functions.glDrawElements(mode, count[i], type, indices[i]);
        else es3_functions.glDrawElementsInstanced(mode, count[i], type, indices[i], instances);
    }
    if (cur != 0) bound = emu_point(l, 0, bound);
    if (bound != array_buffer) es3_functions.glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
}

/* Attrib-state wrappers: forward, and invalidate the layout cache. */
#define EMU_BUMP() atomic_fetch_add_explicit(&emu_gen, 1, memory_order_relaxed)
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void *pointer) {
    EMU_BUMP();
    es3_functions.glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}
void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
                            const void *pointer) {
    EMU_BUMP();
    es3_functions.glVertexAttribIPointer(index, size, type, stride, pointer);
}
void glEnableVertexAttribArray(GLuint index) {
    EMU_BUMP();
    es3_functions.glEnableVertexAttribArray(index);
}
void glDisableVertexAttribArray(GLuint index) {
    EMU_BUMP();
    es3_functions.glDisableVertexAttribArray(index);
}
void glVertexAttribDivisor(GLuint index, GLuint divisor) {
    EMU_BUMP();
    es3_functions.glVertexAttribDivisor(index, divisor);
}
void glDeleteVertexArrays(GLsizei n, const GLuint *arrays) {
    EMU_BUMP();
    es3_functions.glDeleteVertexArrays(n, arrays);
}

void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
                              GLint basevertex) {
    if (!current_context) return;
    const bv_route_t *r = bv_get();
    if (r->route == BV_EMU) {
        if (basevertex == 0) es3_functions.glDrawElements(mode, count, type, indices);
        else emu_draw(mode, &count, type, &indices, 1, &basevertex, -1);
        return;
    }
    if (r->route != BV_UPSTREAM) { r->draw(mode, count, type, indices, basevertex); return; }
    ltw_upstream_glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
}

void glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count, GLenum type,
                                   const void *const *indices, GLsizei drawcount,
                                   const GLint *basevertex) {
    if (!current_context) return;
    const bv_route_t *r = bv_get();
    if (r->route == BV_EMU) { emu_draw(mode, count, type, indices, drawcount, basevertex, -1); return; }
    if (r->route != BV_UPSTREAM) {
        for (GLsizei i = 0; i < drawcount; i++)
            if (count[i] > 0) r->draw(mode, count[i], type, indices[i], basevertex[i]);
        return;
    }
    ltw_upstream_glMultiDrawElementsBaseVertex(mode, count, type, indices, drawcount, basevertex);
}

void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                   GLenum type, const void *indices, GLint basevertex) {
    if (!current_context) return;
    const bv_route_t *r = bv_get();
    if (r->route != BV_UPSTREAM && r->range) {
        r->range(mode, start, end, count, type, indices, basevertex);
        return;
    }
    /* start/end are only a hint: the plain base-vertex draw is equivalent */
    glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
}

void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                       const void *indices, GLsizei instancecount,
                                       GLint basevertex) {
    if (!current_context) return;
    const bv_route_t *r = bv_get();
    if (r->route != BV_UPSTREAM && r->inst) {
        r->inst(mode, count, type, indices, instancecount, basevertex);
        return;
    }
    if (r->route == BV_EMU) {
        emu_draw(mode, &count, type, &indices, 1, &basevertex, instancecount);
        return;
    }
    if (instancecount == 1) { glDrawElementsBaseVertex(mode, count, type, indices, basevertex); return; }
    LOG_ONCE("LTW-Darwin: glDrawElementsInstancedBaseVertex unavailable on this ES; dropped\n");
}

/* ---- 4. glClientWaitSync flush ------------------------------------------------------------------ */
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
__attribute__((visibility("hidden"))) int ltw_darwin_buf_stats_on(void);
__attribute__((visibility("hidden"))) int64_t ltw_darwin_buf_now(void);
__attribute__((visibility("hidden"))) void ltw_darwin_buf_note_wait(int64_t ns);
__attribute__((visibility("hidden"))) void *ltw_darwin_buf_lookup(const char *name);
GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LTW_DARWIN_SYNC_FLUSH"); on = !(e && e[0] == '0'); }
    if (on) flags |= GL_SYNC_FLUSH_COMMANDS_BIT;
    if (!ltw_darwin_buf_stats_on()) return es3_functions.glClientWaitSync(sync, flags, timeout);
    int64_t t0 = ltw_darwin_buf_now();
    GLenum r = es3_functions.glClientWaitSync(sync, flags, timeout);
    ltw_darwin_buf_note_wait(ltw_darwin_buf_now() - t0);
    return r;
}

/* ---- 5. texture buffers (emulated as 2D textures) ------------------------------------------------ */
#define TB_TARGET 0x8C2A            /* GL_TEXTURE_BUFFER */
#define TB_W      2048              /* row width; ES 3.0 guarantees MAX_TEXTURE_SIZE >= 2048 */
static int tb_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LTW_DARWIN_TEXBUFFER"); on = !(e && e[0] == '0'); }
    return on;
}
static _Thread_local GLint tb_unit;            /* active texture unit index, tracked by glActiveTexture */
static _Thread_local int   tb_pending;         /* a buffer texture replaced unit tb_pend_unit's 2D binding */
static _Thread_local GLint tb_pend_unit, tb_pend_prev;

void glActiveTexture(GLenum unit) {
    tb_unit = (GLint)unit - GL_TEXTURE0;
    es3_functions.glActiveTexture(unit);
}
void glBindTexture(GLenum target, GLuint tex) {
    if (target == TB_TARGET && tb_on()) {
        if (!tb_pending) {
            GLint prev = 0; es3_functions.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
            tb_pending = 1; tb_pend_unit = tb_unit; tb_pend_prev = prev;
        }
        es3_functions.glBindTexture(GL_TEXTURE_2D, tex);
        return;
    }
    /* the app re-took the 2D binding of the pending unit itself: nothing to restore any more */
    if (tb_pending && target == GL_TEXTURE_2D && tb_unit == tb_pend_unit) tb_pending = 0;
    es3_functions.glBindTexture(target, tex);
}
static void tb_after_draw(void) {
    if (!tb_pending) return;
    tb_pending = 0;
    if (tb_unit != tb_pend_unit) es3_functions.glActiveTexture(GL_TEXTURE0 + tb_pend_unit);
    es3_functions.glBindTexture(GL_TEXTURE_2D, (GLuint)tb_pend_prev);
    if (tb_unit != tb_pend_unit) es3_functions.glActiveTexture(GL_TEXTURE0 + tb_unit);
}
__attribute__((visibility("hidden"))) void ltw_darwin_tb_after_draw(void) { tb_after_draw(); }
static int tb_format(GLenum ifmt, GLenum *fmt, GLenum *type, int *bpp) {
    switch (ifmt) {
    case GL_R8:       *fmt = GL_RED;          *type = GL_UNSIGNED_BYTE;  *bpp = 1;  return 1;
    case GL_R8I:      *fmt = GL_RED_INTEGER;  *type = GL_BYTE;           *bpp = 1;  return 1;
    case GL_R8UI:     *fmt = GL_RED_INTEGER;  *type = GL_UNSIGNED_BYTE;  *bpp = 1;  return 1;
    case GL_R16I:     *fmt = GL_RED_INTEGER;  *type = GL_SHORT;          *bpp = 2;  return 1;
    case GL_R16UI:    *fmt = GL_RED_INTEGER;  *type = GL_UNSIGNED_SHORT; *bpp = 2;  return 1;
    case GL_R16F:     *fmt = GL_RED;          *type = GL_HALF_FLOAT;     *bpp = 2;  return 1;
    case GL_R32I:     *fmt = GL_RED_INTEGER;  *type = GL_INT;            *bpp = 4;  return 1;
    case GL_R32UI:    *fmt = GL_RED_INTEGER;  *type = GL_UNSIGNED_INT;   *bpp = 4;  return 1;
    case GL_R32F:     *fmt = GL_RED;          *type = GL_FLOAT;          *bpp = 4;  return 1;
    case GL_RG8:      *fmt = GL_RG;           *type = GL_UNSIGNED_BYTE;  *bpp = 2;  return 1;
    case GL_RG32F:    *fmt = GL_RG;           *type = GL_FLOAT;          *bpp = 8;  return 1;
    case GL_RGBA8:    *fmt = GL_RGBA;         *type = GL_UNSIGNED_BYTE;  *bpp = 4;  return 1;
    case GL_RGBA8I:   *fmt = GL_RGBA_INTEGER; *type = GL_BYTE;           *bpp = 4;  return 1;
    case GL_RGBA8UI:  *fmt = GL_RGBA_INTEGER; *type = GL_UNSIGNED_BYTE;  *bpp = 4;  return 1;
    case GL_RGBA16F:  *fmt = GL_RGBA;         *type = GL_HALF_FLOAT;     *bpp = 8;  return 1;
    case GL_RGBA32F:  *fmt = GL_RGBA;         *type = GL_FLOAT;          *bpp = 16; return 1;
    case GL_RGBA32I:  *fmt = GL_RGBA_INTEGER; *type = GL_INT;            *bpp = 16; return 1;
    case GL_RGBA32UI: *fmt = GL_RGBA_INTEGER; *type = GL_UNSIGNED_INT;   *bpp = 16; return 1;
    }
    return 0;
}
static void tb_upload(GLenum ifmt, GLuint buffer, GLintptr off, GLsizeiptr size) {
    if (!current_context) return;
    GLenum fmt, type; int bpp;
    if (!tb_format(ifmt, &fmt, &type, &bpp)) {
        LOG_ONCE("LTW-Darwin: texture buffer internal format 0x%x not emulated; dropped\n", ifmt);
        return;
    }
    GLint prev_pbo = 0, row = 0, srows = 0, spix = 0, align = 4;
    es3_functions.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pbo);
    es3_functions.glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
    es3_functions.glGetIntegerv(GL_UNPACK_SKIP_ROWS, &srows);
    es3_functions.glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &spix);
    es3_functions.glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    if (size < 0) {
        GLint64 total = 0;
        es3_functions.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        es3_functions.glGetBufferParameteri64v(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &total);
        size = (GLsizeiptr)total - off;
    }
    long n = size > 0 ? (long)(size / bpp) : 0;
    long rows = n > 0 ? (n + TB_W - 1) / TB_W : 1;
    es3_functions.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    es3_functions.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    es3_functions.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    es3_functions.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    es3_functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    es3_functions.glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, TB_W, (GLsizei)rows, 0, fmt, type, NULL);
    if (n > 0) {
        long full = n / TB_W, rem = n % TB_W;
        es3_functions.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        if (full) es3_functions.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TB_W, (GLsizei)full, fmt, type,
                                                (const void *)(uintptr_t)off);
        if (rem) es3_functions.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)full, (GLsizei)rem, 1, fmt, type,
                                               (const void *)(uintptr_t)(off + full * TB_W * bpp));
    }
    es3_functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    es3_functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    es3_functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    es3_functions.glBindSampler((GLuint)tb_unit, 0);
    es3_functions.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)prev_pbo);
    es3_functions.glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
    es3_functions.glPixelStorei(GL_UNPACK_SKIP_ROWS, srows);
    es3_functions.glPixelStorei(GL_UNPACK_SKIP_PIXELS, spix);
    es3_functions.glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    static atomic_bool said;
    if (!atomic_exchange(&said, true))
        fprintf(stderr, "LTW-Darwin: texture buffer emulated as %dx%ld 2D (ifmt 0x%x, %ld texels)\n",
                TB_W, rows, ifmt, n);
}
void glTexBuffer(GLenum target, GLenum ifmt, GLuint buffer) { tb_upload(ifmt, buffer, 0, -1); }
void glTexBufferRange(GLenum target, GLenum ifmt, GLuint buffer, GLintptr off, GLsizeiptr size) {
    tb_upload(ifmt, buffer, off, size);
}
/* draw hooks: put back the 2D binding a buffer texture displaced (only does work right after one) */
static void tbh_DrawArrays(GLenum m, GLint f, GLsizei c) { es3_functions.glDrawArrays(m, f, c); tb_after_draw(); }
static void tbh_DrawElements(GLenum m, GLsizei c, GLenum t, const void *i) {
    es3_functions.glDrawElements(m, c, t, i); tb_after_draw();
}
static void tbh_DrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i) {
    es3_functions.glDrawRangeElements(m, s, e, c, t, i); tb_after_draw();
}
static void tbh_DrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) {
    es3_functions.glDrawArraysInstanced(m, f, c, n); tb_after_draw();
}
static void tbh_DrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) {
    es3_functions.glDrawElementsInstanced(m, c, t, i, n); tb_after_draw();
}
static void tbh_DrawElementsBaseVertex(GLenum m, GLsizei c, GLenum t, const void *i, GLint b) {
    glDrawElementsBaseVertex(m, c, t, i, b); tb_after_draw();
}
static void tbh_DrawRangeElementsBaseVertex(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b) {
    glDrawRangeElementsBaseVertex(m, s, e, c, t, i, b); tb_after_draw();
}
static void tbh_DrawElementsInstancedBaseVertex(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n, GLint b) {
    glDrawElementsInstancedBaseVertex(m, c, t, i, n, b); tb_after_draw();
}
static void tbh_MultiDrawElementsBaseVertex(GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n,
                                            const GLint *b) {
    glMultiDrawElementsBaseVertex(m, c, t, i, n, b); tb_after_draw();
}

/* ---- 6. shader source rewrite for texture buffers (see 5) ------------------------------------------ */
void ltw_upstream_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
static int tb_ident(char c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static void sb_put(char **b, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) { *cap = (*len + n + 1) * 2; *b = realloc(*b, *cap); }
    memcpy(*b + *len, s, n); *len += n; (*b)[*len] = 0;
}
/* returns a malloc'd rewritten source, or NULL when the source has no buffer sampler */
static char *tb_rewrite(const char *src) {
    if (!strstr(src, "samplerBuffer")) return NULL;
    char names[8][64]; char kinds[8]; int nn = 0;
    char *out = NULL; size_t len = 0, cap = 0;
    const char *p = src;
    /* pass 1: declarations -> 2D samplers + helper functions right after each declaration */
    while (*p) {
        const char *hit = strstr(p, "samplerBuffer");
        if (!hit) { sb_put(&out, &len, &cap, p, strlen(p)); break; }
        const char *start = hit; char kind = 0;
        if (hit > src && (hit[-1] == 'i' || hit[-1] == 'u')) { kind = hit[-1]; start = hit - 1; }
        if ((start > src && tb_ident(start[-1])) || tb_ident(hit[13])) {      /* part of a longer word */
            sb_put(&out, &len, &cap, p, (size_t)(hit + 13 - p)); p = hit + 13; continue;
        }
        /* precision qualifier already present? look back over spaces for lowp/mediump/highp */
        const char *q = start; while (q > src && (q[-1] == ' ' || q[-1] == '\t')) q--;
        int has_prec = (q - src >= 4 && !strncmp(q - 4, "lowp", 4)) || (q - src >= 5 && !strncmp(q - 5, "highp", 5)) ||
                       (q - src >= 7 && !strncmp(q - 7, "mediump", 7));
        sb_put(&out, &len, &cap, p, (size_t)(start - p));
        const char *t2d = kind == 'i' ? "isampler2D" : kind == 'u' ? "usampler2D" : "sampler2D";
        if (!has_prec) sb_put(&out, &len, &cap, "highp ", 6);
        sb_put(&out, &len, &cap, t2d, strlen(t2d));
        p = hit + 13;
        const char *n0 = p; while (*n0 == ' ' || *n0 == '\t') n0++;
        const char *n1 = n0; while (tb_ident(*n1)) n1++;
        const char *semi = strchr(n1, ';');
        if (n1 > n0 && n1 - n0 < 63 && nn < 8 && semi) {
            memcpy(names[nn], n0, (size_t)(n1 - n0)); names[nn][n1 - n0] = 0; kinds[nn] = kind;
            sb_put(&out, &len, &cap, p, (size_t)(semi + 1 - p)); p = semi + 1;
            char fn[512];
            const char *rt = kind == 'i' ? "ivec4" : kind == 'u' ? "uvec4" : "vec4";
            snprintf(fn, sizeof fn, "\n%s ltw_tbf_%s(int i) { return texelFetch(%s, ivec2(i %% %d, i / %d), 0); }\n",
                     rt, names[nn], names[nn], TB_W, TB_W);
            sb_put(&out, &len, &cap, fn, strlen(fn));
            nn++;
        }
    }
    /* pass 2: texelFetch(N, x) -> ltw_tbf_N(x), skipping the helpers' own texelFetch(N, ivec2(...)) */
    for (int k = 0; k < nn; k++) {
        char *o2 = NULL; size_t l2 = 0, c2 = 0; const char *r = out;
        char helper[128]; snprintf(helper, sizeof helper, "ltw_tbf_%s(int i)", names[k]);
        while (*r) {
            const char *h = strstr(r, "texelFetch");
            if (!h) { sb_put(&o2, &l2, &c2, r, strlen(r)); break; }
            const char *a = h + 10; while (*a == ' ' || *a == '\t') a++;
            int ok = *a == '(' && !(h > out && tb_ident(h[-1]));
            const char *b = a + 1; while (*b == ' ' || *b == '\t') b++;
            size_t nl = strlen(names[k]);
            ok = ok && !strncmp(b, names[k], nl) && !tb_ident(b[nl]);
            const char *c = b + nl; while (*c == ' ' || *c == '\t') c++;
            ok = ok && *c == ',';
            /* the helper body itself: the line containing "ltw_tbf_<N>(int i)" */
            const char *ls = h; while (ls > out && ls[-1] != '\n') ls--;
            if (ok && strstr(ls, helper) && strstr(ls, helper) < h) ok = 0;
            if (!ok) { sb_put(&o2, &l2, &c2, r, (size_t)(h + 10 - r)); r = h + 10; continue; }
            sb_put(&o2, &l2, &c2, r, (size_t)(h - r));
            char rep[96]; snprintf(rep, sizeof rep, "ltw_tbf_%s(", names[k]);
            sb_put(&o2, &l2, &c2, rep, strlen(rep));
            r = c + 1;
        }
        free(out); out = o2; len = l2; cap = c2;
    }
    return out;
}
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    if (!tb_on() || count <= 0 || !string) { ltw_upstream_glShaderSource(shader, count, string, length); return; }
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
    char *joined = malloc(total + 1); size_t off = 0;
    for (GLsizei i = 0; i < count; i++) {
        size_t l = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        memcpy(joined + off, string[i], l); off += l;
    }
    joined[total] = 0;
    char *rw = tb_rewrite(joined);
    const GLchar *one = rw ? rw : joined;
    if (rw) LOG_ONCE("LTW-Darwin: shader with a texture buffer rewritten to a 2D-texture fetch\n");
    ltw_upstream_glShaderSource(shader, 1, &one, NULL);
    free(rw); free(joined);
}

/* ---- 3. lookup --------------------------------------------------------------------------------- */
#define ROUTE(name) if (!strcmp(procname, #name)) return (eglMustCastToProperFunctionPointerType)name;
eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) {
    if (!procname) return NULL;
    void *b = ltw_darwin_buf_lookup(procname);  /* section 8: buffer uploads (+ counters / frame log) */
    if (b) return (eglMustCastToProperFunctionPointerType)b;
    if (procname[0] == 'g' && procname[1] == 'l') {
        void *d33 = ltw_d33_lookup(procname);   /* section 7: GL 3.3 surface (honest errors, routes, glGetError) */
        if (d33) return (eglMustCastToProperFunctionPointerType)d33;
        if (tb_on()) {
            ROUTE(glActiveTexture)
            ROUTE(glBindTexture)
            ROUTE(glTexBuffer)
            ROUTE(glTexBufferRange)
            ROUTE(glShaderSource)
#define TBH(name) if (!strcmp(procname, "gl" #name)) return (eglMustCastToProperFunctionPointerType)tbh_##name;
            TBH(DrawArrays) TBH(DrawElements) TBH(DrawRangeElements) TBH(DrawArraysInstanced)
            TBH(DrawElementsInstanced) TBH(DrawElementsBaseVertex) TBH(DrawRangeElementsBaseVertex)
            TBH(DrawElementsInstancedBaseVertex) TBH(MultiDrawElementsBaseVertex)
#undef TBH
        }
        ROUTE(glGetString)
        ROUTE(glGetIntegerv)
        ROUTE(glDrawElementsBaseVertex)
        ROUTE(glDrawRangeElementsBaseVertex)
        ROUTE(glDrawElementsInstancedBaseVertex)
        ROUTE(glMultiDrawElementsBaseVertex)
        ROUTE(glVertexAttribPointer)
        ROUTE(glVertexAttribIPointer)
        ROUTE(glEnableVertexAttribArray)
        ROUTE(glDisableVertexAttribArray)
        ROUTE(glVertexAttribDivisor)
        ROUTE(glDeleteVertexArrays)
        ROUTE(glClientWaitSync)
    }
    return ltw_upstream_eglGetProcAddress(procname);
}
#undef ROUTE

/* LWJGL's LINUX provider name; kept as upstream exports it. */
__attribute__((used)) eglMustCastToProperFunctionPointerType glXGetProcAddress(const char *procname) {
    return eglGetProcAddress(procname);
}
