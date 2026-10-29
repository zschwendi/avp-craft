# libltw on Darwin: inventory notes (T5)

Source: `work/src/PojavLauncherTeam_ltw` @ 7703f1c, `ltw/src/main/tinywrapper/`. It is compiled in place
and never edited. Every Darwin difference lives in `darwin/` here.

## Build system upstream
- Gradle `com.android.library` → ndk-build `Android.mk` (NDK 25.2, `APP_STL := c++_static`, android-21).
- Two modules:
  - `glsl_optimizer`: a static Mesa GLSL/NIR subset. About 330 .c/.cpp files, `-fvisibility=hidden`, with flags
    `-DANDROID -DDETECT_OS_ANDROID -D__STDC_NO_THREADS__ -DUNIX -DUTIL_ARCH_LITTLE_ENDIAN ...`.
    The generated parser and lexer and the NIR tables (glsl_parser.cpp, glcpp-parse.c, nir_opcodes.c, …) are
    checked in, so no bison, flex or python is needed.
  - `ltw` (shared): 17 C files, `LDLIBS := -llog -lEGL`. Default visibility, so every non-static symbol is
    exported, the helpers included.
- There are no include dirs in Android.mk because ndk-build adds `$(LOCAL_PATH)`. Mesa's own includes are
  relative (`../../util/...`, `"c11/threads.h"`, etc.).
- `glsl_optimizer/src/compiler/glsl/CMakeLists.txt` is empty and CMakePresets.json is Windows-only, so
  there is no usable CMake. Our `Makefile` reads the source lists straight out of `Android.mk` with awk.

## Android/Linux-specific dependencies found (and what was done)
| Where | What | Darwin handling |
|---|---|---|
| proc.c | `#include <android/log.h>`, `__android_log_print` (2 calls) | `darwin/include/android/log.h` overlay + impl in `darwin/ltw_darwin.c` (stderr + os_log) |
| proc.c `proc_init` (`__attribute__((constructor))`) | `dlopen("libEGL.so")` | macro-redirected (proc.c only, via `-include darwin/ltw_darwin_proc.h`) to `ltw_darwin_dlopen`: `$LTW_EGL_LIBRARY`, `@rpath/libEGL.framework/libEGL`, `@rpath/libEGL.dylib`, `libEGL.dylib` |
| proc.c `resolve_stub` | `dlsym(NULL, "stub_gl…")` (NULL = global scope on bionic; invalid on Darwin where RTLD_DEFAULT = -2) | `ltw_darwin_dlsym`: NULL → `RTLD_SELF` then `RTLD_DEFAULT` |
| proc.h | `#include <threads.h>` (only for `thread_local`) | `darwin/include/threads.h` → Mesa's bundled `c11/threads.h` (posix backend) + `#define thread_local _Thread_local` |
| vgpu_shaderconv/shaderconv.c | uses `EOF` without `<stdio.h>` (bionic pulls it in transitively) | `darwin/ltw_darwin_prefix.h` force-includes `<stdio.h>` |
| nir_print.c | `#include <vulkan/vulkan_core.h>` (the NDK sysroot has it; used only for enum names) | `-idirafter` Klepton ANGLE's `third_party/vulkan-headers/src/include` (header-only) |
| util/u_debug_stack_android.cpp | Android backtrace | dropped. `u_debug_stack.c` has the generic path |
| util/futex.c | Linux futex | nothing needed: `UTIL_FUTEX_SUPPORTED` is 0 on Apple, so simple_mtx uses mtx_t |
| util/os_memory_fd.c, anon_file.c (memfd) | memfd_create | nothing needed: guarded and compiles to the non-Linux path |
| util/log.c, os_misc.c (android log) | under `DETECT_OS_ANDROID` | nothing needed: we drop `-DANDROID -DDETECT_OS_ANDROID`, and Mesa detects `DETECT_OS_APPLE` |
| c11/threads.h | `HAVE_PTHREAD` branch is EMPTY in this vendored copy | do NOT define HAVE_PTHREAD (that gives the empty branch and no mtx_t). Leave it undefined, as upstream does, to get threads_posix.h |

Upstream flags kept: everything except `-DANDROID -DDETECT_OS_ANDROID`. Added `-DHAVE_TIMESPEC_GET`.
`-Wno-everything` is set because the Mesa subset is very noisy. Real errors still stop the build.

## How the GL entry points are produced
- `es3_functions.h` / `es3_extended.h` are X-macro lists (`GLESFUNC(name, PFN…)`). `init_es3_proc()` fills
  `es3_functions_t` from the host `eglGetProcAddress` at load time. A missing core ES function aborts.
  The extended ones (glDrawElementsIndirect, glMultiDraw*EXT, glGetTexLevelParameter*,
  glDrawElementsBaseVertex, glBufferStorageEXT) may be NULL.
- `es3_overrides.h`: a `GLESOVERRIDE(name)` list. `eglGetProcAddress(name)` returns ltw's own function
  for these, printing `LTW: Overridden <name>` on EVERY lookup. Any other `gl*` name goes to the host
  `eglGetProcAddress` (ANGLE), and if that is NULL, to `stub_<name>`.
- `stubs.c` (generated, 7685 lines) holds 1279 `stub_gl*` no-ops that print once.
- Exported names: `eglGetProcAddress`, `glXGetProcAddress`, `eglCreateContext`, `eglDestroyContext`,
  `eglMakeCurrent`, 113 real `gl*`, 1279 `stub_gl*`, plus internal helpers (shaderconv, unordered_map,
  gl4es_* string utils). On Darwin we add `OSMesaGetProcAddress`. The total is 1488.
- Context model: ltw's `eglCreateContext` wraps the host's and keys a `context_t` by EGLContext.
  `eglMakeCurrent` sets the thread-local `current_context`. If ltw does not know the context, it aborts.
  Every override starts with `if(!current_context) return;`, so a context that was not created and made
  current THROUGH ltw silently turns every overridden call into a no-op.
- ES version detection (`egl.c find_esversion`) uses the RAW ES `glGetString(GL_VERSION)`. On ANGLE that
  is "OpenGL ES 3.0 (ANGLE …)", so es31 = es32 = false. ltw itself reports "3.0 OpenLTW" / "4.60 LTW".

## How Pojav hooks it (Android)
- `JREUtils.java:217-219`: renderer `opengles3_ltw` sets `LIBGL_ES=3` and `POJAVEXEC_EGL=libltw.so`.
  `loadGraphicsLibrary()` dlopens `libltw.so`, and `-Dorg.lwjgl.opengl.libname=libltw.so` is passed to LWJGL.
- `ctxbridges/egl_loader.c dlsym_EGL()`: dlopen(`$POJAVEXEC_EGL`), then `dlsym(eglGetProcAddress)` and then
  EVERY egl* function through it. ltw returns its wrappers for Create/Destroy/MakeCurrent and passes
  the rest through to the system EGL.
- LWJGL on LINUX: `GL$1` calls `glXGetProcAddress` from the GL library, which is why ltw exports that alias.

## LWJGL on MACOSX (the fork in work/native-image/lwjgl-fork, read-only look)
- `GL.create()` MACOSX branch: if `org.lwjgl.opengl.libname` is set, `Library.loadNative(GL, "org.lwjgl.opengl", <name>)`,
  otherwise the `com.apple.opengl` framework.
- `GL$1` (the FunctionProvider): LINUX uses `glXGetProcAddress`/`…ARB`, WINDOWS uses `wglGetProcAddress`, and
  EVERY platform falls back to **`OSMesaGetProcAddress`** when that is still 0. On MACOSX that is the
  only one tried. Then plain `dlsym` on the library if the GetProcAddress returns 0.
  So exporting `OSMesaGetProcAddress` from libltw routes all of LWJGL's GL lookups through ltw with
  **no Java change**.
- `fixPojavGLContext()` only runs when `Platform == LINUX && POJAV_RENDERER` is set, so it is irrelevant on Darwin.
- `createCapabilities` reads `GL_MAJOR_VERSION`/`GL_MINOR_VERSION` through `glGetIntegerv` (ltw passes them
  through to ANGLE, giving 3/0) and only falls back to parsing `GL_VERSION` on error. `GLCapabilities.check_GLxx`
  loads every function address regardless and ANDs the result with `ext.contains("OpenGLxx")`.
