/* Force-included into every ltw/glsl_optimizer translation unit on Darwin. Keep this tiny. */
#ifndef LTW_DARWIN_PREFIX_H
#define LTW_DARWIN_PREFIX_H
#ifndef __APPLE__
#error "darwin prefix used on a non-Apple target"
#endif
/* vgpu_shaderconv/shaderconv.c uses EOF without <stdio.h>; bionic pulls it in transitively. */
#include <stdio.h>
#endif
