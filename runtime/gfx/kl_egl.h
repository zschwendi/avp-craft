// The EGL surface libunity.so imports.
//
// Exactly the 19 symbols in the unresolved list and nothing else. Notably that
// list contains no GL entry points at all: `eglGetProcAddress` is in it, so
// Unity resolves the whole of GLES through that single function at runtime.
// That is the entire graphics surface behind one door, which is what makes
// measuring it cheap — see kl_egl.c.
#ifndef KL_EGL_H
#define KL_EGL_H
#include <stdio.h>
#include <stdint.h>

// Resolve an EGL import by name. NULL if we do not provide it.
void *kl_egl_lookup(const char *name);

// The GLES level the device description reports (version string, GLSL version,
// MAJOR/MINOR limits). Default 3.2 for Beat Saber; call before any GL traffic.
void kl_egl_set_gles_version(int major, int minor);

// Unity reaches GLES through *two* doors, not one. eglGetProcAddress is the
// documented one; the other is dlopen("libGLESv2.so") followed by dlsym, and
// when that dlopen failed Unity called straight through the resulting NULL —
// a jump to address 0 with nothing on the stack to say why. So the loader hands
// out a synthetic handle for the GL sonames and resolves symbols on it through
// the same gateway eglGetProcAddress uses.
void *kl_egl_dlopen(const char *soname);   // NULL if this is not a GL library
// Same question with no side effects, for callers that only need to know
// whether this gateway would serve the name. See kl_can_dlopen (klepton.h).
int   kl_egl_claims(const char *soname);
int   kl_egl_is_handle(const void *h);
void *kl_egl_sym(const char *name);        // the gateway itself

// How many times the guest has called eglSwapBuffers. For the frame-complete
// seams that have to know whether the swap is the guest's presentation signal at
// all: Beat Saber 1.28 swaps, its 1.40 XR-SDK path never does, and neither does
// an OpenXR guest. Zero here means "the capture and frontend seams hanging off
// the swap have never fired, so this frame's completion is where they belong".
unsigned long kl_egl_swap_count(void);

// The EGLContext current on THIS thread, as the guest sees it. Diagnostic: a
// guest that keys resource ownership on the current context (Unity keys FBO
// ownership on it) fails in a way that names neither the context nor the
// resource, so a GL trace has to be able to say which context a name was made
// under. Not a GL call and not the ANGLE context — the handle the guest holds.
void *kl_egl_current_context(void);

// What the guest asked eglGetProcAddress for, and which of those it went on to
// call. This is the work list, in the order the guest wanted it.
void kl_egl_report(FILE *f);

// Shader sources the guest handed to glShaderSource. They are the input to the
// GLSL ES -> SPIR-V -> MSL pipeline needs, and this is the only place they
// exist in plain text — the APK stores them compressed inside Unity's assets.
unsigned kl_egl_shader_count(void);
void     kl_egl_dump_shaders(const char *dir);

// Write every uncompressed 8-bit glTexSubImage2D upload to <dir> as a PNG. There
// is no framebuffer to capture — nothing renders — but for a frame that is one
// textured quad these uploads are its visible content.
void kl_egl_dump_textures(const char *dir);
unsigned kl_egl_texture_count(void);

// The capability tables behind the query entry points, for kl_glfb: under the
// reference renderer the *description* of the device stays ours, but dynamic
// state (READ_BUFFER, bindings, viewport — anything the tables do not list)
// belongs to ANGLE, which actually tracks it. Each returns 1 when the pname
// is ours (answer written), 0 when it is not — no print, no zero-fill.
int kl_gl_cap_integerv(uint32_t pname, int32_t *params);
int kl_gl_cap_integeri_v(uint32_t target, uint32_t index, int32_t *data);
int kl_gl_cap_internalformativ(uint32_t target, uint32_t internalformat,
                               uint32_t pname, int32_t bufSize, int32_t *params);
int kl_gl_cap_floatv(uint32_t pname, float *data);
int kl_gl_cap_integer64v(uint32_t pname, int64_t *data);

// The texture-unit caps, and the door that stops them from being a second,
// hand-maintained copy of the vendored ANGLE's number.
//
// The description we hand the guest has to agree with what the real driver will
// enforce: Unity sizes its own texture-unit state from GL_MAX_COMBINED_TEXTURE_-
// IMAGE_UNITS and then indexes that state by unit with no bound of its own, so a
// number larger than ANGLE's does not produce a polite GL error, it produces a
// write past the end of Unity's array. Beat Saber 1.40 died exactly there.
//
// So under a real driver the answers are ADOPTED from it rather than asserted:
// kl_glfb calls this once with what ANGLE actually reports, and the built-in
// literals in kl_egl.c are only the null-renderer fallback. Values <= 0 are
// ignored, and a shrink is honoured as loudly as a grow.
void kl_gl_cap_adopt_texture_units(int32_t per_stage, int32_t combined);

// What the description currently promises for GL_MAX_COMBINED_TEXTURE_IMAGE_-
// UNITS — post-adoption if adoption has happened. The one number anything else
// that has to agree with the driver (kl_guestpoke's Unity cap poke) should ask
// for, rather than carrying its own copy.
int32_t kl_gl_cap_max_combined_texture_units(void);

// Extensions ADOPTED from the backend, the same way the texture-unit caps are:
// kl_glfb hands over ANGLE's own glGetString(GL_EXTENSIONS) once the real
// context exists, and a name from the runtime's candidate list is added to what
// the guest is told only if the driver named it first. See kl_egl.c for the
// candidates and why each is there — the short version is that srceng was
// decompressing a 4.9 GB DXT texture pack to RGBA8888 on the CPU because our
// extension string mentioned no compression, and sampling every floor at its
// worst axis because it mentioned no anisotropy.
void        kl_gl_cap_adopt_backend_extensions(const char *backend_ext);
int         kl_gl_cap_backend_has_dxt(void);
int         kl_gl_cap_adopted_extension_count(void);
const char *kl_gl_cap_adopted_extension(int i);

#endif
