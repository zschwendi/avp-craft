/* Force-included into upstream basevertex.c ONLY (Makefile target-specific flag), Follow-up 1.
 * Upstream implements glDrawElementsBaseVertex / glMultiDrawElementsBaseVertex for ES 3.2 (core)
 * and ES 3.1 (indirect-draw emulation) and silently drops them on ES 3.0. darwin/ltw_darwin_gl.c
 * owns the exported names, routes ES 3.0 + EXT/OES_draw_elements_base_vertex to the extension
 * entry points, and falls back to these renamed upstream versions otherwise. Inside basevertex.c
 * the es3_functions.glDrawElementsBaseVertex member is spelled with the prefix too (same layout). */
#ifndef LTW_DARWIN_RENAME_BASEVERTEX_H
#define LTW_DARWIN_RENAME_BASEVERTEX_H
#define glDrawElementsBaseVertex      ltw_upstream_glDrawElementsBaseVertex
#define glMultiDrawElementsBaseVertex ltw_upstream_glMultiDrawElementsBaseVertex
#endif
