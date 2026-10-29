/* Darwin GL layer, section 7: the GL 3.3 surface Minecraft 26.x sees (M3 stage C, 2026-10-12).
 * Compiled with upstream's include paths (context_t, es3_functions, host_eglGetProcAddress), like ltw_darwin_gl.c.
 *
 *  7a. Honest errors. ltw answers every desktop name the host (ANGLE, ES 3.0) lacks with a generated stub_ that
 *      does NOTHING and reports no error, so a caller cannot tell "worked" from "dropped". For the entry points
 *      Minecraft 26.2 references (GlCommandEncoder, DirectStateAccess$Core/$Emulated, VertexArrayCache$Separate,
 *      GlDevice, GlStateManager, GlQueryPool) this file returns a wrapper instead that either
 *        - routes to the ES extension that implements it, when the CURRENT context's raw extension list has it
 *          (resolved per context, cached), or to an exactly-equivalent ES 3.0 call (base instance 0, FILL, COPY,
 *          query results through glGetQueryObjectuiv), or
 *        - records GL_INVALID_OPERATION (returned by the next glGetError, first error wins, per thread) and logs the
 *          name once; out-parameters are zeroed, map calls return NULL.
 *      None of these features is advertised (no ARB_* names, version 3.3 implies none of them), so 26.2 takes its
 *      fallbacks and should never reach the error path; the errors exist so a wrong turn is visible.
 *  7b. Extension filtering. When ltw reports GL 3.3+ (LTW_REPORT_GL_VERSION, set by klglfw for a 3.3 request),
 *      GL_ARB_direct_state_access and GL_ARB_buffer_storage are removed from glGetString(GL_EXTENSIONS),
 *      glGetStringi and GL_NUM_EXTENSIONS (consistently). Upstream ltw ADDS GL_ARB_buffer_storage whenever the host
 *      has GL_EXT_buffer_storage; 26.2's DirectStateAccess$Emulated would then take the persistent-mapping path.
 *      LTW_DARWIN_HIDE_EXTENSIONS="A B ..." replaces the list in any mode ("none" = hide nothing).
 *      GL 3.2 mode (Minecraft 1.21.x) hides nothing by default: unchanged behaviour.
 *  The GLSL version string and GL_CONTEXT_FLAGS answer live in ltw_darwin_gl.c section 2.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "proc.h"
#include "egl.h"

#define HIDDEN __attribute__((visibility("hidden")))

/* from ltw_darwin_gl.c / upstream (renamed by darwin/ltw_darwin_rename_main.h) */
HIDDEN bool ltw_darwin_report_version(int *maj, int *min);
HIDDEN void ltw_darwin_tb_after_draw(void);
void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
                                       GLsizei instancecount, GLint basevertex);
GLenum ltw_upstream_glGetError(void);
void glDrawBuffer(GLenum buffer);   /* upstream framebuffer.c */
const GLubyte *ltw_upstream_glGetStringi(GLenum name, GLuint index);
void ltw_upstream_glGetIntegerv(GLenum pname, GLint *data);

#ifndef GL_INVALID_OPERATION
#define GL_INVALID_OPERATION 0x0502
#endif
#define GL_FILL_ 0x1B02
#define GL_COPY_ 0x1503
#define GL_QUERY_RESULT_ 0x8866
#define GL_QUERY_RESULT_AVAILABLE_ 0x8867

/* ---- 7a. pending error ------------------------------------------------------------------------- */
static _Thread_local GLenum pending_err;

static void d33_fail(const char *name) {
    if (!pending_err) pending_err = GL_INVALID_OPERATION;
    /* once per name (tiny fixed table; names are string literals, so pointer identity is enough) */
    static const char *seen[64]; static atomic_int nseen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int n = atomic_load(&nseen), hit = 0;
    for (int i = 0; i < n; i++) if (seen[i] == name) { hit = 1; break; }
    if (!hit && n < 64) { seen[n] = name; atomic_store(&nseen, n + 1); }
    pthread_mutex_unlock(&mu);
    if (!hit) printf("LTW-Darwin: %s is not available on this ES context -> GL_INVALID_OPERATION\n", name);
}

GLenum glGetError(void) {
    static int noerror = -1;   /* upstream LIBGL_NOERROR semantics: set and not '0' -> always GL_NO_ERROR */
    if (noerror < 0) { const char *e = getenv("LIBGL_NOERROR"); noerror = e && *e != '0'; }
    if (!current_context || noerror) return ltw_upstream_glGetError();
    if (pending_err) { GLenum e = pending_err; pending_err = 0; return e; }
    return ltw_upstream_glGetError();
}

/* ---- per-context resolution of the ES extension routes ---------------------------------------- */
static bool d33_has_ext(const char *list, const char *name) {
    if (!list) return false;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
    return false;
}

typedef struct {
    const context_t *ctx; EGLContext phys;
    void (*clipControl)(GLenum, GLenum);
    void (*polygonMode)(GLenum, GLenum);
    void (*logicOp)(GLenum);
    void (*queryCounter)(GLuint, GLenum);
    void (*getQi64)(GLuint, GLenum, GLint64 *);
    void (*getQui64)(GLuint, GLenum, GLuint64 *);
    void (*daibi)(GLenum, GLint, GLsizei, GLsizei, GLuint);                                   /* DrawArraysInstancedBaseInstance */
    void (*deibi)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLuint);                    /* DrawElementsInstancedBaseInstance (EXT) */
    void (*deibvbi)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLint, GLuint);           /* ...BaseVertexBaseInstance */
    void (*mdai)(GLenum, const void *, GLsizei, GLsizei);                                       /* MultiDrawArraysIndirect */
    void (*mdei)(GLenum, GLenum, const void *, GLsizei, GLsizei);                               /* MultiDrawElementsIndirect */
    void (*dai)(GLenum, const void *);                                                          /* ES 3.1 core */
    void (*dei)(GLenum, GLenum, const void *);
    void (*bindVertexBuffer)(GLuint, GLuint, GLintptr, GLsizei);
    void (*vaFormat)(GLuint, GLint, GLenum, GLboolean, GLuint);
    void (*vaIFormat)(GLuint, GLint, GLenum, GLuint);
    void (*vaBinding)(GLuint, GLuint);
    void (*bindingDivisor)(GLuint, GLuint);
    void (*bufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield);
} d33_routes_t;
static _Thread_local d33_routes_t R;

#define HOSTFN(ext, fn) (d33_has_ext(x, ext) ? (void *)host_eglGetProcAddress(fn) : NULL)
static const d33_routes_t *routes(void) {
    context_t *c = current_context;
    if (R.ctx == c && R.phys == c->phys_context) return &R;
    memset(&R, 0, sizeof R);
    R.ctx = c; R.phys = c->phys_context;
    const char *x = (const char *)es3_functions.glGetString(GL_EXTENSIONS);   /* RAW host list */
    R.clipControl  = HOSTFN("GL_EXT_clip_control", "glClipControlEXT");
    R.polygonMode  = HOSTFN("GL_ANGLE_polygon_mode", "glPolygonModeANGLE");
    if (!R.polygonMode) R.polygonMode = HOSTFN("GL_NV_polygon_mode", "glPolygonModeNV");
    R.logicOp      = HOSTFN("GL_ANGLE_logic_op", "glLogicOpANGLE");
    R.queryCounter = HOSTFN("GL_EXT_disjoint_timer_query", "glQueryCounterEXT");
    R.getQi64      = HOSTFN("GL_EXT_disjoint_timer_query", "glGetQueryObjecti64vEXT");
    R.getQui64     = HOSTFN("GL_EXT_disjoint_timer_query", "glGetQueryObjectui64vEXT");
    R.daibi        = HOSTFN("GL_ANGLE_base_vertex_base_instance", "glDrawArraysInstancedBaseInstanceANGLE");
    R.deibvbi      = HOSTFN("GL_ANGLE_base_vertex_base_instance", "glDrawElementsInstancedBaseVertexBaseInstanceANGLE");
    if (!R.daibi)   R.daibi   = HOSTFN("GL_EXT_base_instance", "glDrawArraysInstancedBaseInstanceEXT");
    if (!R.deibvbi) R.deibvbi = HOSTFN("GL_EXT_base_instance", "glDrawElementsInstancedBaseVertexBaseInstanceEXT");
    R.deibi        = HOSTFN("GL_EXT_base_instance", "glDrawElementsInstancedBaseInstanceEXT");
    R.mdai         = HOSTFN("GL_EXT_multi_draw_indirect", "glMultiDrawArraysIndirectEXT");
    R.mdei         = HOSTFN("GL_EXT_multi_draw_indirect", "glMultiDrawElementsIndirectEXT");
    if (c->es31) {
        R.dai = (void *)host_eglGetProcAddress("glDrawArraysIndirect");
        R.dei = (void *)host_eglGetProcAddress("glDrawElementsIndirect");
        R.bindVertexBuffer = (void *)host_eglGetProcAddress("glBindVertexBuffer");
        R.vaFormat  = (void *)host_eglGetProcAddress("glVertexAttribFormat");
        R.vaIFormat = (void *)host_eglGetProcAddress("glVertexAttribIFormat");
        R.vaBinding = (void *)host_eglGetProcAddress("glVertexAttribBinding");
        R.bindingDivisor = (void *)host_eglGetProcAddress("glVertexBindingDivisor");
    }
    if (c->buffer_storage) R.bufferStorage = (void *)host_eglGetProcAddress("glBufferStorageEXT");
    return &R;
}
#undef HOSTFN

#define NEED_CTX(...) do { if (!current_context) return __VA_ARGS__; } while (0)

/* ---- 7a. the wrappers -------------------------------------------------------------------------- */
static void d33_ClipControl(GLenum origin, GLenum depth) {
    NEED_CTX();
    if (routes()->clipControl) R.clipControl(origin, depth); else d33_fail("glClipControl");
}
static void d33_PolygonMode(GLenum face, GLenum mode) {
    NEED_CTX();
    if (routes()->polygonMode) R.polygonMode(face, mode);
    else if (mode != GL_FILL_) d33_fail("glPolygonMode");   /* FILL is what ES always does: exact no-op */
}
static void d33_LogicOp(GLenum op) {
    NEED_CTX();
    if (routes()->logicOp) R.logicOp(op);
    else if (op != GL_COPY_) d33_fail("glLogicOp");          /* COPY = no logic op: exact no-op */
}
static void d33_QueryCounter(GLuint id, GLenum target) {
    NEED_CTX();
    if (routes()->queryCounter) R.queryCounter(id, target); else d33_fail("glQueryCounter");
}
/* 64-bit query results: the timer-query extension when present; otherwise RESULT / RESULT_AVAILABLE of any ES 3.0
 * query through the core 32-bit getter (exact for occlusion / transform-feedback queries), anything else = error. */
static void d33_GetQueryObjecti64v(GLuint id, GLenum pname, GLint64 *params) {
    NEED_CTX();
    if (routes()->getQi64) { R.getQi64(id, pname, params); return; }
    if (pname == GL_QUERY_RESULT_ || pname == GL_QUERY_RESULT_AVAILABLE_) {
        GLuint v = 0; es3_functions.glGetQueryObjectuiv(id, pname, &v); if (params) *params = v; return;
    }
    if (params) *params = 0;
    d33_fail("glGetQueryObjecti64v");
}
static void d33_GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params) {
    NEED_CTX();
    if (routes()->getQui64) { R.getQui64(id, pname, params); return; }
    if (pname == GL_QUERY_RESULT_ || pname == GL_QUERY_RESULT_AVAILABLE_) {
        GLuint v = 0; es3_functions.glGetQueryObjectuiv(id, pname, &v); if (params) *params = v; return;
    }
    if (params) *params = 0;
    d33_fail("glGetQueryObjectui64v");
}
static void d33_DrawArraysInstancedBaseInstance(GLenum m, GLint first, GLsizei count, GLsizei n, GLuint bi) {
    NEED_CTX();
    if (routes()->daibi) R.daibi(m, first, count, n, bi);
    else if (bi == 0) es3_functions.glDrawArraysInstanced(m, first, count, n);
    else { d33_fail("glDrawArraysInstancedBaseInstance"); return; }
    ltw_darwin_tb_after_draw();
}
static void d33_DrawElementsInstancedBaseInstance(GLenum m, GLsizei count, GLenum type, const void *ind, GLsizei n, GLuint bi) {
    NEED_CTX();
    if (routes()->deibi) R.deibi(m, count, type, ind, n, bi);
    else if (R.deibvbi) R.deibvbi(m, count, type, ind, n, 0, bi);
    else if (bi == 0) es3_functions.glDrawElementsInstanced(m, count, type, ind, n);
    else { d33_fail("glDrawElementsInstancedBaseInstance"); return; }
    ltw_darwin_tb_after_draw();
}
static void d33_DrawElementsInstancedBaseVertexBaseInstance(GLenum m, GLsizei count, GLenum type, const void *ind,
                                                            GLsizei n, GLint bv, GLuint bi) {
    NEED_CTX();
    if (routes()->deibvbi) R.deibvbi(m, count, type, ind, n, bv, bi);
    else if (bi == 0) glDrawElementsInstancedBaseVertex(m, count, type, ind, n, bv);  /* ltw's (EXT / emulation) */
    else { d33_fail("glDrawElementsInstancedBaseVertexBaseInstance"); return; }
    ltw_darwin_tb_after_draw();
}
static void d33_DrawArraysIndirect(GLenum m, const void *ind) {
    NEED_CTX();
    if (routes()->dai) { R.dai(m, ind); ltw_darwin_tb_after_draw(); } else d33_fail("glDrawArraysIndirect");
}
static void d33_DrawElementsIndirect(GLenum m, GLenum type, const void *ind) {
    NEED_CTX();
    if (routes()->dei) { R.dei(m, type, ind); ltw_darwin_tb_after_draw(); } else d33_fail("glDrawElementsIndirect");
}
static void d33_MultiDrawArraysIndirect(GLenum m, const void *ind, GLsizei n, GLsizei stride) {
    NEED_CTX();
    if (routes()->mdai) { R.mdai(m, ind, n, stride); ltw_darwin_tb_after_draw(); } else d33_fail("glMultiDrawArraysIndirect");
}
static void d33_MultiDrawElementsIndirect(GLenum m, GLenum type, const void *ind, GLsizei n, GLsizei stride) {
    NEED_CTX();
    if (routes()->mdei) { R.mdei(m, type, ind, n, stride); ltw_darwin_tb_after_draw(); } else d33_fail("glMultiDrawElementsIndirect");
}
static void d33_BindVertexBuffer(GLuint bi, GLuint buf, GLintptr off, GLsizei stride) {
    NEED_CTX();
    if (routes()->bindVertexBuffer) R.bindVertexBuffer(bi, buf, off, stride); else d33_fail("glBindVertexBuffer");
}
static void d33_VertexAttribFormat(GLuint i, GLint size, GLenum type, GLboolean norm, GLuint rel) {
    NEED_CTX();
    if (routes()->vaFormat) R.vaFormat(i, size, type, norm, rel); else d33_fail("glVertexAttribFormat");
}
static void d33_VertexAttribIFormat(GLuint i, GLint size, GLenum type, GLuint rel) {
    NEED_CTX();
    if (routes()->vaIFormat) R.vaIFormat(i, size, type, rel); else d33_fail("glVertexAttribIFormat");
}
static void d33_VertexAttribBinding(GLuint i, GLuint b) {
    NEED_CTX();
    if (routes()->vaBinding) R.vaBinding(i, b); else d33_fail("glVertexAttribBinding");
}
static void d33_VertexBindingDivisor(GLuint b, GLuint d) {
    NEED_CTX();
    if (routes()->bindingDivisor) R.bindingDivisor(b, d); else d33_fail("glVertexBindingDivisor");
}
/* upstream returns silently when the host has no GL_EXT_buffer_storage */
static void d33_BufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags) {
    NEED_CTX();
    if (routes()->bufferStorage) R.bufferStorage(target, size, data, flags); else d33_fail("glBufferStorage");
}
/* the exported name (upstream's GLESOVERRIDE table in proc.c refers to it); 7a off = upstream (silent) */
void ltw_upstream_glBufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags);
static int d33_on(void);
void glBufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags) {
    if (d33_on()) d33_BufferStorage(target, size, data, flags);
    else ltw_upstream_glBufferStorage(target, size, data, flags);
}
/* ARB_debug_output (ANGLE has KHR_debug only, so the ARB cap is never set; the KHR names go to ANGLE/ltw) */
static void d33_DebugMessageCallbackARB(void *cb, const void *user) { NEED_CTX(); d33_fail("glDebugMessageCallbackARB"); }
static void d33_DebugMessageControlARB(GLenum s, GLenum t, GLenum sev, GLsizei n, const GLuint *ids, GLboolean en) {
    NEED_CTX(); d33_fail("glDebugMessageControlARB");
}
/* DSA (GL 4.5 / ARB_direct_state_access): never advertised, never implemented. */
static void d33_CreateBuffers(GLsizei n, GLuint *o) { NEED_CTX(); if (o) memset(o, 0, sizeof *o * (n > 0 ? n : 0)); d33_fail("glCreateBuffers"); }
static void d33_CreateFramebuffers(GLsizei n, GLuint *o) { NEED_CTX(); if (o) memset(o, 0, sizeof *o * (n > 0 ? n : 0)); d33_fail("glCreateFramebuffers"); }
static void d33_CreateTextures(GLenum t, GLsizei n, GLuint *o) { NEED_CTX(); if (o) memset(o, 0, sizeof *o * (n > 0 ? n : 0)); d33_fail("glCreateTextures"); }
static void d33_CreateVertexArrays(GLsizei n, GLuint *o) { NEED_CTX(); if (o) memset(o, 0, sizeof *o * (n > 0 ? n : 0)); d33_fail("glCreateVertexArrays"); }
static void d33_NamedBufferData(GLuint b, GLsizeiptr s, const void *d, GLenum u) { NEED_CTX(); d33_fail("glNamedBufferData"); }
static void d33_NamedBufferStorage(GLuint b, GLsizeiptr s, const void *d, GLbitfield f) { NEED_CTX(); d33_fail("glNamedBufferStorage"); }
static void d33_NamedBufferSubData(GLuint b, GLintptr o, GLsizeiptr s, const void *d) { NEED_CTX(); d33_fail("glNamedBufferSubData"); }
static void d33_NamedFramebufferTexture(GLuint f, GLenum a, GLuint t, GLint l) { NEED_CTX(); d33_fail("glNamedFramebufferTexture"); }
static void d33_NamedFramebufferDrawBuffers(GLuint f, GLsizei n, const GLenum *b) { NEED_CTX(); d33_fail("glNamedFramebufferDrawBuffers"); }
static void d33_NamedFramebufferDrawBuffer(GLuint f, GLenum b) { NEED_CTX(); d33_fail("glNamedFramebufferDrawBuffer"); }
static void d33_BlitNamedFramebuffer(GLuint r, GLuint d, GLint a, GLint b, GLint c, GLint e, GLint f, GLint g, GLint h,
                                     GLint i, GLbitfield m, GLenum fl) { NEED_CTX(); d33_fail("glBlitNamedFramebuffer"); }
static void d33_CopyNamedBufferSubData(GLuint r, GLuint w, GLintptr ro, GLintptr wo, GLsizeiptr s) { NEED_CTX(); d33_fail("glCopyNamedBufferSubData"); }
static void *d33_MapNamedBufferRange(GLuint b, GLintptr o, GLsizeiptr l, GLbitfield a) { NEED_CTX(NULL); d33_fail("glMapNamedBufferRange"); return NULL; }
static void d33_FlushMappedNamedBufferRange(GLuint b, GLintptr o, GLsizeiptr l) { NEED_CTX(); d33_fail("glFlushMappedNamedBufferRange"); }
static GLboolean d33_UnmapNamedBuffer(GLuint b) { NEED_CTX(GL_FALSE); d33_fail("glUnmapNamedBuffer"); return GL_FALSE; }

/* ---- 7b. extension filtering ------------------------------------------------------------------- */
typedef struct {
    const context_t *ctx; EGLContext phys;
    int active;          /* 0: nothing hidden -> passthrough */
    int n;               /* filtered count */
    const GLubyte **list;
    char *string;
} d33_ext_t;
static _Thread_local d33_ext_t X;

/* the hide list for this process: env, else the GL 3.3 default, else none */
static const char *hide_list(void) {
    const char *e = getenv("LTW_DARWIN_HIDE_EXTENSIONS");
    if (e) return (!*e || !strcasecmp(e, "none")) ? "" : e;
    int maj = 0, min = 0;
    if (ltw_darwin_report_version(&maj, &min) && (maj > 3 || (maj == 3 && min >= 3)))
        return "GL_ARB_direct_state_access GL_ARB_buffer_storage";
    return "";
}
static bool hidden(const char *hl, const char *name) {
    size_t n = strlen(name);
    for (const char *p = hl; (p = strstr(p, name)) != NULL; p += n)
        if ((p == hl || p[-1] == ' ' || p[-1] == ',') && (p[n] == ' ' || p[n] == ',' || p[n] == 0)) return true;
    return false;
}
static const d33_ext_t *ext(void) {
    context_t *c = current_context;
    if (X.ctx == c && X.phys == c->phys_context) return &X;
    free(X.list); free(X.string);
    memset(&X, 0, sizeof X);
    X.ctx = c; X.phys = c->phys_context;
    const char *hl = hide_list();
    if (!*hl) return &X;
    GLint up = 0;
    ltw_upstream_glGetIntegerv(GL_NUM_EXTENSIONS, &up);
    X.list = calloc(up > 0 ? up : 1, sizeof *X.list);
    size_t len = 1;
    int dropped = 0;
    for (GLint i = 0; i < up; i++) {
        const GLubyte *s = ltw_upstream_glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (!s) continue;
        if (hidden(hl, (const char *)s)) { dropped++; printf("LTW-Darwin: hiding %s from GL_EXTENSIONS\n", s); continue; }
        X.list[X.n++] = s; len += strlen((const char *)s) + 1;
    }
    X.string = malloc(len); X.string[0] = 0;
    char *w = X.string;
    for (int i = 0; i < X.n; i++) { size_t l = strlen((const char *)X.list[i]); memcpy(w, X.list[i], l); w += l; *w++ = ' '; }
    if (w > X.string) w[-1] = 0; else *w = 0;
    X.active = 1;
    printf("LTW-Darwin: GL_EXTENSIONS filtered: %d of %d kept (hide list \"%s\")\n", X.n, (int)up, hl);
    (void)dropped;
    return &X;
}
/* called by ltw_darwin_gl.c's glGetString / glGetIntegerv */
HIDDEN const GLubyte *ltw_d33_extensions_string(void) {
    const d33_ext_t *e = ext();
    return e->active ? (const GLubyte *)e->string : NULL;
}
HIDDEN bool ltw_d33_num_extensions(GLint *data) {
    const d33_ext_t *e = ext();
    if (!e->active) return false;
    *data = e->n; return true;
}
const GLubyte *glGetStringi(GLenum name, GLuint index) {
    if (!current_context) return NULL;
    if (name == GL_EXTENSIONS) {
        const d33_ext_t *e = ext();
        if (e->active) return index < (GLuint)e->n ? e->list[index] : NULL;
    }
    return ltw_upstream_glGetStringi(name, index);
}

/* ---- lookup (consulted first by ltw_darwin_gl.c's eglGetProcAddress) --------------------------- */
/* Section 7a is ON in GL 3.3+ report mode (Minecraft 26.x) and OFF in 3.2 mode (1.21.x: upstream routing, unchanged).
 * LTW_DARWIN_GL33=1 / 0 forces it. Decided with a current context only (the report version is parsed lazily, after
 * klglfw set it for the game's 3.3 request): LWJGL looks every function up inside GL.createCapabilities, i.e. with
 * the context current; a lookup without a context (none of 7a's names) gets upstream routing. */
static int d33_on(void) {
    const char *e = getenv("LTW_DARWIN_GL33");
    if (e && *e) return e[0] != '0';
    if (!current_context) return 0;
    int maj = 0, min = 0;
    return ltw_darwin_report_version(&maj, &min) && (maj > 3 || (maj == 3 && min >= 3));
}
HIDDEN void *ltw_d33_lookup(const char *n) {
    if (!strcmp(n, "glGetError")) return (void *)glGetError;
    if (!strcmp(n, "glGetStringi")) return (void *)glGetStringi;
    if (!d33_on()) return NULL;
    /* upstream implements glDrawBuffer (framebuffer.c: glDrawBuffers(1, &b) with its virtual-attachment mapping)
     * but never listed it in its override table, so the lookup handed out the silent stub_glDrawBuffer. */
    if (!strcmp(n, "glDrawBuffer")) return (void *)glDrawBuffer;
#define R33(name) if (!strcmp(n, "gl" #name)) return (void *)d33_##name;
    R33(ClipControl) R33(PolygonMode) R33(LogicOp) R33(QueryCounter) R33(GetQueryObjecti64v) R33(GetQueryObjectui64v)
    R33(DrawArraysInstancedBaseInstance) R33(DrawElementsInstancedBaseInstance) R33(DrawElementsInstancedBaseVertexBaseInstance)
    R33(DrawArraysIndirect) R33(DrawElementsIndirect) R33(MultiDrawArraysIndirect) R33(MultiDrawElementsIndirect)
    R33(BindVertexBuffer) R33(VertexAttribFormat) R33(VertexAttribIFormat) R33(VertexAttribBinding) R33(VertexBindingDivisor)
    R33(BufferStorage) R33(DebugMessageCallbackARB) R33(DebugMessageControlARB)
    R33(CreateBuffers) R33(CreateFramebuffers) R33(CreateTextures) R33(CreateVertexArrays)
    R33(NamedBufferData) R33(NamedBufferStorage) R33(NamedBufferSubData) R33(NamedFramebufferTexture)
    R33(NamedFramebufferDrawBuffers) R33(NamedFramebufferDrawBuffer) R33(BlitNamedFramebuffer) R33(CopyNamedBufferSubData)
    R33(MapNamedBufferRange) R33(FlushMappedNamedBufferRange) R33(UnmapNamedBuffer)
#undef R33
    return NULL;
}
