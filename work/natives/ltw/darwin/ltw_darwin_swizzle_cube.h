/* Force-included into upstream swizzle.c ONLY (Makefile target-specific flag), M3 stage A (Minecraft 1.21.11).
 * swizzle.c's per-texture swizzle tracker (run on every glTexImage2D/glTexSubImage2D upload) calls
 * glGetTexParameteriv / glTexParameteri with the UPLOAD target. For a cube-map face
 * (GL_TEXTURE_CUBE_MAP_POSITIVE_X..NEGATIVE_Z) that is not a valid texture-parameter target, so ES raises
 * GL_INVALID_ENUM (1280) on every face upload. Desktop GL drivers are equally strict, but 1.21.5 never
 * uploaded a cube map; 1.21.6+'s GlDevice.createTexture builds the title panorama as a real cube map,
 * checks glGetError right after, and threw "OpenGL error 1280", which rolled back the whole resource
 * reload (logs/t4-m3a-jvm-world2.txt). The binding query already maps faces to GL_TEXTURE_BINDING_CUBE_MAP
 * (main.c get_textarget_query_param), so only the two parameter calls need the face -> GL_TEXTURE_CUBE_MAP
 * mapping. Function-like macros: `es3_functions.glTexParameteri(t, ...)` expands to
 * `es3_functions.glTexParameteri(ltw_darwin_param_target(t), ...)`; the struct member declarations in
 * proc.h are not followed by '(' and stay untouched. */
#ifndef LTW_DARWIN_SWIZZLE_CUBE_H
#define LTW_DARWIN_SWIZZLE_CUBE_H
/* pull in every header swizzle.c includes FIRST (all include-guarded), so the GLES prototypes are declared
 * before the macros below exist */
#include <string.h>
#include "proc.h"
#include "egl.h"
#include "libraryinternal.h"
static inline unsigned ltw_darwin_param_target(unsigned t) {
    return (t >= 0x8515u && t <= 0x851Au) ? 0x8513u /* GL_TEXTURE_CUBE_MAP */ : t;
}
#define glGetTexParameteriv(t, p, v) glGetTexParameteriv(ltw_darwin_param_target(t), p, v)
#define glTexParameteri(t, p, v)     glTexParameteri(ltw_darwin_param_target(t), p, v)
#endif
