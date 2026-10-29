# T5: libltw on Darwin over ANGLE ES 3.0 (2026-10-12)

**Result:** PASS on macOS (rendered through ltw → ANGLE Metal, pixel-checked).
xros and xrsimulator dylibs compile and link cleanly (platforms 11/12, minos
26.0, SDK 27.0); not run. Nothing downloaded; inputs are the existing
`work/src/PojavLauncherTeam_ltw` clone (untouched), Klepton's ANGLE at
`vendor/out/Debug` (headers `vendor/include`) and the header-only
`vendor/third_party/vulkan-headers`.

## What ltw needs
- An EGL whose `eglGetProcAddress` resolves the ES 3.0 core and the EGL core
  functions (ANGLE's libEGL does). A missing core ES function aborts at load.
- Contexts created AND made current through ltw's own `eglCreateContext` /
  `eglMakeCurrent`: ltw keys per-context state by context and aborts on one it
  does not know; a context made current behind its back makes every override a
  silent no-op (renders nothing, no error).
- No futex/memfd/ELF/bionic: the Mesa subset has Apple fallbacks once the
  Android defines are dropped.

## Shims (all under `darwin/`)
- `include/android/log.h` — logging declarations; stderr + os_log impl.
- `include/threads.h` — Mesa's bundled c11/threads.h (posix) + `thread_local`.
- `ltw_darwin_prefix.h` — includes `<stdio.h>` (shaderconv.c uses EOF).
- `ltw_darwin_proc.h` — applied to proc.c only: `dlopen("libEGL.so")` →
  `$LTW_EGL_LIBRARY`, `@rpath/libEGL.framework/libEGL`, `@rpath/libEGL.dylib`,
  `libEGL.dylib`; `dlsym(NULL)` → RTLD_SELF then RTLD_DEFAULT.
- `ltw_darwin.c` — the implementations + exported `OSMesaGetProcAddress` alias.
Build changes: drop `-DANDROID -DDETECT_OS_ANDROID` and
`u_debug_stack_android.cpp`; `-idirafter` vulkan-headers (nir_print.c);
`-Wl,-rpath,@loader_path`. Identical across the three SDKs; only `-target`
differs. xros imports: libc, libc++, pthread, dl, os_log, sysctl only.

## Test (`make test` → `test/ltw_test.log`, `test/ltw_test.png` 128x128)
- GL_VERSION direct: `OpenGL ES 3.0 (ANGLE 2.1.1 git hash: 25e721127e1c)`,
  GLSL `OpenGL ES GLSL ES 3.00`, renderer `ANGLE Metal Renderer: Apple M4 Pro`.
- GL_VERSION through ltw: `3.0 OpenLTW`, GLSL `4.60 LTW`; ltw logs "Running on
  OpenGL ES 3.0 with ESSL 300".
- Every pointer comes from ltw's `OSMesaGetProcAddress`: override
  (glClearDepth), stub via dlsym(NULL) (glPolygonMode), ANGLE passthrough
  (glDrawArrays), EGL wrapper (eglMakeCurrent), NULL for unknown names.
- `#version 330 core` fragment shader: ANGLE alone rejects it; through ltw both
  shaders compile and link.
- `glTexImage2D(GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8)` reads back
  centre `ff 80 20 ff`, corner `00 00 40 ff`; no GL errors.
- `glBindFragDataLocation` is a no-op upstream (not proven, not reachable).
- ANGLE extensions present: EXT/OES_draw_elements_base_vertex,
  EXT_texture_format_BGRA8888, EXT_clip_control, EXT_depth_clamp,
  ANGLE_polygon_mode, EXT_draw_buffers_indexed. Absent: EXT_buffer_storage,
  ANGLE_multi_draw, EXT_multi_draw_arrays, ANGLE_base_vertex_base_instance,
  OES_texture_border_clamp.

## Build
`make [SDK=macosx|xros|xrsimulator]`, `make test`, `make symbols`
(~1 min, -j12; object dirs deleted after, build/ not committed).
`xcrun --sdk <sdk> clang -target arm64-apple-{macos13.0|xros26.0|xros26.0-simulator}`.
Overridable: LTW, ANGLE_INC, ANGLE_MAC, VK_INC. Dylib ~3.1 MB, unsigned,
install name `@rpath/libltw.dylib`, depends on libc++/libSystem only; dlopens
libEGL at load like upstream.

## Exports (`exports.txt`, 1488)
1279 `stub_gl*`, 113 real `gl*`, `glXGetProcAddress`, `eglGetProcAddress`,
`eglCreateContext`, `eglDestroyContext`, `eglMakeCurrent`,
`OSMesaGetProcAddress` (Darwin addition), ~90 internal helpers.

## LWJGL hook — no Java change needed
The fork's `GL$1` on MACOSX skips glX/wgl and asks the GL library for
`OSMesaGetProcAddress`, then falls back to dlsym. Run with
`-Dorg.lwjgl.opengl.libname=<path>/libltw.dylib`. T4's GLFW stand-in must take
its EGL functions from ltw's `eglGetProcAddress` (as Pojav's egl_loader.c
does): at least eglCreateContext/eglMakeCurrent/eglDestroyContext.

## Risks
1. **Base-vertex draws silently dropped on ES 3.0** (ltw handles them only on
   3.1/3.2). ANGLE exposes EXT_draw_elements_base_vertex → small fix
   (`glDrawElementsBaseVertexEXT`, looped for multi-draw). Blocks Sodium.
2. **LWJGL will see GL 3.0** (Quest sees 3.2): OpenGL31/32/33 caps false.
   Option: Darwin wrapper answering GL_MAJOR/MINOR 3.2. Decision pending.
3. Lookup-table gaps (same on Quest): glCreateProgram, glAttachShader,
   glLinkProgram, glDeleteProgram, glDeleteTextures, glDrawBuffer,
   glDrawElementsBaseVertex → callers get raw ANGLE or a no-op stub.
4. glShaderSource with >1 string corrupts the source (MC/Sodium pass one).
5. GL_LINEAR forced to GL_NEAREST. 6. glBufferStorage is a no-op.
7. Log line on every lookup of an overridden name.
8. xros libEGL.framework not exercised; confirm same ANGLE revision; via
   kl_egl's gateway set KL_GL_ES_VERSION=3.0.
9. kl_openxr switching contexts behind ltw leaves ltw on the wrong context.
10. Load-time constructor aborts if libEGL is missing.

## Follow-up 1 (2026-10-12): base-vertex on ES 3.0 + GL version report

**Result:** both PASS on macOS (`make test`, 3 passes, 0 failures); xros and xrsimulator
still link (VISIONOS / VISIONOSSIMULATOR, minos 26.0). Upstream clone still untouched.

### What changed (all in the Darwin layer, no patch file needed)
- `darwin/ltw_darwin_gl.c` (new) owns the exported `glDrawElementsBaseVertex`,
  `glMultiDrawElementsBaseVertex`, `glDrawRangeElementsBaseVertex` (new export),
  `glDrawElementsInstancedBaseVertex` (new export), `glGetString`, `glGetIntegerv`,
  `eglGetProcAddress`, `glXGetProcAddress`.
- Upstream's own definitions are renamed by force-included macro headers, one per file:
  `ltw_darwin_proc.h` (proc.c: eglGetProcAddress/glXGetProcAddress), new
  `ltw_darwin_rename_main.h` (main.c: glGetString/glGetIntegerv), new
  `ltw_darwin_rename_basevertex.h` (basevertex.c: the two base-vertex draws). They become
  `ltw_upstream_*`; the Darwin versions call them for everything they don't handle. Linked
  with `-unexported_symbol _ltw_upstream_*`, so exports only grow by 2 (1490, `exports.txt`).
  Caveat: in main.c/basevertex.c the macro also renames the `es3_functions` member of the
  same name. The layout is the same, only the spelling differs in that one file.
- **Base-vertex routing** (per ltw context, cached per thread): ES 3.2 uses the host core
  entry points. Otherwise, if the raw host extension list has `GL_EXT_draw_elements_base_vertex`
  (else `GL_OES_…`), it uses `gl*BaseVertexEXT`/`OES`, and multi-draw is a loop. Otherwise
  it falls back to upstream (the ES 3.1 indirect path, or its drop). The Range draw falls back
  to the plain draw; Instanced with a count other than 1 drops and logs once.
  `LTW_DARWIN_BASEVERTEX_EXT=0` turns the route off, for the negative test.
- **Version report:** `LTW_REPORT_GL_VERSION`, "M.N", defaults to 3.2. "off", "0" or ""
  give the upstream "3.0 OpenLTW".
  - `GL_MAJOR_VERSION`/`GL_MINOR_VERSION` answer M/N.
  - `GL_VERSION` answers "M.N OpenLTW".
  - `GL_CONTEXT_PROFILE_MASK` answers CORE. I added this because LWJGL's
    `createCapabilities` queries 0x9126 whenever the version is ≥ 3.2. ES has no such enum,
    so ANGLE would queue GL_INVALID_ENUM, and LWJGL would decide the context isn't
    forward-compatible.
  - ltw's own ES detection (`find_esversion`) reads raw `es3_functions` strings. It still
    logs "Running on OpenGL ES 3.0", and es31/es32 stay false.
- The lookup now returns the Darwin functions for those six names, without the per-lookup
  "Overridden" spam. Before, `glDrawElementsBaseVertex` fell through to ANGLE's raw ES 3.2
  entry point (risk 3, partly fixed).

### New test checks (`test/ltw_test.c`; Makefile runs it 3×, log in `test/ltw_test.log`)
- Lookup: all six names → ltw's export, ≠ ANGLE's pointer. glX/egl lookups agree.
  `ltw_upstream_*` are not exported. All 31 names in LWJGL `check_GL31` + `check_GL32`
  resolve to non-NULL.
- createCapabilities replay, in the fork's bytecode order: MAJOR/MINOR, GL_VERSION,
  NUM_EXTENSIONS + every glGetStringi, PROFILE_MASK, then glGetError == 0. ANGLE itself
  still says 3.0.
- Base-vertex draw. The vertex buffer holds a decoy quad at vertices 0–3 (top-left) and real
  quads only at basevertex 4/8/12/16/20. They are drawn by DrawElementsBaseVertex,
  MultiDrawElementsBaseVertex (2 draws, second at a non-zero index offset), DrawRange…, and
  DrawElementsInstanced…(1). Pixel checks: every real quad is ff8020ff and the decoy is still
  clear colour. If basevertex were ignored, the decoy would be drawn. If the draw were
  dropped, nothing would be.
- Pass 2 (`LTW_DARWIN_BASEVERTEX_EXT=0`) asserts that all 5 quads are ABSENT while 3.2 is
  still reported. This proves the check tells the cases apart, and that ltw internally still
  treats the backend as ES 3.0. Pass 3 (`LTW_REPORT_GL_VERSION=off`) asserts "3.0 OpenLTW"
  and 3/0. A manual run with `=3.3` gives 3.3 and PROFILE_MASK=1.

### What MC 1.21.5 calls (javap over the client jar's 16 classes that touch org/lwjgl/opengl)
Mojang names come from `client_mappings_1.21.5.txt`. The classes are GlStateManager,
GlDevice, GlCommandEncoder, GlProgram, GlDebug, GlDebugLabel, DirectStateAccess,
VertexArrayCache and TimerQuery.
- MC reads no GL version itself. The 3.2-core requirement comes from its GLFW window hints,
  which T4's GLFW stand-in decides. On the GL side, only LWJGL's OpenGLxx flags and these
  capability fields matter: `GL_KHR_debug`, `GL_EXT_debug_label`, `GL_ARB_debug_output`,
  `GL_ARB_direct_state_access`, `GL_ARB_vertex_attrib_binding`, `GL_ARB_timer_query`.
- **GL 3.1–3.3 functions MC calls directly:** only GL32 `glFenceSync`, `glClientWaitSync`,
  `glDeleteSync` (ES 3.0 core, so they pass through to ANGLE). It calls no GL31 or GL33
  functions. (`GL32C.glBeginQuery/EndQuery/GenQueries/DeleteQueries/GetQueryObjecti/
  BindBuffer/BufferData` are GL15 methods referenced through GL32C.)
- GL30 functions it uses: glBindFramebuffer, glGenFramebuffers, glDeleteFramebuffers,
  glFramebufferTexture2D, glBlitFramebuffer, glBindVertexArray, glGenVertexArrays,
  glMapBufferRange, glVertexAttribIPointer. The rest are GL11–GL20 core, plus
  GL11.glDrawBuffer, glPolygonMode, glLogicOp, glClearDepth and glGetTexLevelParameteri.
- For LWJGL to set `OpenGL32`, every function in check_GL32 must resolve. They all do now,
  whether from the Darwin layer, an ANGLE pointer or an ltw stub. Several still go to ANGLE
  ES 3.1/3.2 entry points or stubs that would error or no-op if called: glTexBuffer,
  glTexImage2DMultisample, glFramebufferTexture, glProvokingVertex, glPrimitiveRestartIndex.
  Vanilla MC never calls them.

### Remaining risks
1. ANGLE's ES 3.0 exposes **GL_KHR_debug, GL_EXT_debug_label, GL_EXT_disjoint_timer_query**.
   ltw forwards that extension list, so LWJGL can set GL_KHR_debug. If GlDevice's KHR flag is
   also on, GlDebugLabel calls `glObjectLabel`, and with debug verbosity > 0 GlDebug calls
   `glDebugMessageCallback`. Both are unsuffixed names that ANGLE resolves to its ES 3.2
   entry points. On a 3.0 context those may raise GL_INVALID_OPERATION, where ANGLE expects
   the `…KHR` names. Not tested. If it shows up, route the unsuffixed names to the KHR ones
   in `ltw_darwin_gl.c`, or filter the extension.
2. The reported 3.2 is a claim, not an implementation. Sodium and other mods that branch on
   OpenGL32/33 may call geometry-shader, multisample-texture or sampler paths. ES 3.0 has
   samplers. It lacks geometry shaders and multisample textures.
3. glDrawElementsInstancedBaseVertex with instancecount ≠ 1 is dropped when neither EXT/OES
   nor ES 3.2 is available. On ANGLE, EXT is present, so this doesn't happen.
4. Older risks 3 to 10 still stand. Risk 3 is reduced: glDrawElementsBaseVertex is now
   routed, but glCreateProgram and the others on that list still go to raw ANGLE.

## Follow-up (launch-12 stall round, 2026-10-13): section 8, staged buffer uploads
`darwin/ltw_darwin_buf.c`. glBufferSubData into a buffer of >= `LTW_DARWIN_STAGE_MIN` (1 MiB) goes through an
ltw staging ring (`LTW_DARWIN_STAGE_BLOCK` 4 MiB, unsynchronized append, orphaned when full) + glCopyBufferSubData,
instead of ANGLE Metal's whole-buffer copy-on-write (`putDataInNewBufferAndStartUsingNewBuffer`), which allocated and
copied a full 32 MiB+ Sodium arena per chunk-mesh upload and drove ANGLE into `WaitUntilFinished` syncs (the 26.2
chunk storms). `LTW_DARWIN_SUBDATA=off` = ANGLE's path; `LTW_DARWIN_BUFSTATS=1` = `[ltw-buf]` per-second counters;
`LTW_DARWIN_FRAMELOG=<file>` = host per-frame log. `make test` gains `test/ltw_buf_test.c` (3 passes). Exports
1501 -> 1502 (`_glBufferSubData`). Evidence and numbers: `work/perf/LAUNCH12-STALLS-REPORT.md`.
