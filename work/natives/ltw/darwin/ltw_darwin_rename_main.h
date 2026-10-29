/* Force-included into upstream main.c ONLY (Makefile target-specific flag), Follow-up 1.
 * Renames ltw's glGetString / glGetIntegerv so darwin/ltw_darwin_gl.c can own the exported names
 * and answer GL_VERSION / GL_MAJOR_VERSION / GL_MINOR_VERSION (LTW_REPORT_GL_VERSION) before
 * delegating here. Inside main.c the macro also renames the es3_functions members of the same
 * name; the struct layout is unchanged (only the member spelling differs in this one TU), and
 * ltw's internal ES-version detection (egl.c find_esversion) still reads the RAW host strings. */
#ifndef LTW_DARWIN_RENAME_MAIN_H
#define LTW_DARWIN_RENAME_MAIN_H
#define glGetString   ltw_upstream_glGetString
#define glGetIntegerv ltw_upstream_glGetIntegerv
/* M3 stage C (section 7, darwin/ltw_darwin_gl33.c): pending honest errors, filtered extension list, and
 * glBufferStorage that errors instead of returning silently without GL_EXT_buffer_storage. */
#define glGetError    ltw_upstream_glGetError
#define glGetStringi  ltw_upstream_glGetStringi
#define glBufferStorage ltw_upstream_glBufferStorage
#endif
