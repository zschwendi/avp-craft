/* Force-included into upstream shader_wrapper.c ONLY (Makefile target-specific flag), M3 stage A.
 * Renames ltw's glShaderSource so darwin/ltw_darwin_gl.c (section 6) can own the exported name, rewrite
 * texture-buffer samplers in the desktop GLSL source, and then delegate here. Inside shader_wrapper.c the
 * macro also renames the es3_functions member of the same name (consistently, struct layout unchanged). */
#ifndef LTW_DARWIN_RENAME_SHADER_H
#define LTW_DARWIN_RENAME_SHADER_H
#define glShaderSource ltw_upstream_glShaderSource
#endif
