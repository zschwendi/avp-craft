/* Force-included into upstream proc.c ONLY (Makefile target-specific flag). proc.c is the one file
 * with Android/ELF loader assumptions:
 *   - dlopen("libEGL.so")       -> ANGLE's libEGL (search order in ltw_darwin_dlopen)
 *   - dlsym(NULL, "stub_glX")   -> NULL is not RTLD_DEFAULT on Darwin (it is RTLD_DEFAULT=-2);
 *                                  look in ltw's own image (RTLD_SELF) first, then everywhere.
 * Done by macro so the upstream clone stays pristine. */
#ifndef LTW_DARWIN_PROC_H
#define LTW_DARWIN_PROC_H
#include <dlfcn.h>
void *ltw_darwin_dlopen(const char *path, int mode) __attribute__((visibility("hidden")));
void *ltw_darwin_dlsym(void *handle, const char *sym) __attribute__((visibility("hidden")));
#define dlopen ltw_darwin_dlopen
#define dlsym  ltw_darwin_dlsym
/* Follow-up 1: ltw's lookup becomes the INNER table. darwin/ltw_darwin_gl.c defines the exported
 * eglGetProcAddress / glXGetProcAddress (and ltw_darwin.c OSMesaGetProcAddress), which answer the
 * Darwin-routed names (base-vertex draws, version queries) first and then call this one. The
 * renamed symbols are kept out of the export table by -unexported_symbol _ltw_upstream_* . */
#define eglGetProcAddress ltw_upstream_eglGetProcAddress
#define glXGetProcAddress ltw_upstream_glXGetProcAddress
#endif
