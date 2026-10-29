// The one-eye reference renderer. See kl_glfb.c for why this is host-only and
// cannot ship: visionOS is Metal-only, so the device path is ANGLE or native
// Metal. This exists to produce the known-good frame those will be diffed against.
//
// Opt-in with KL_GLFB=1; without it the null driver in kl_egl.c is unchanged.
#ifndef KL_GLFB_H
#define KL_GLFB_H
#include <stdint.h>
#include <stdio.h>

int  kl_glfb_enabled(void);
int  kl_glfb_init(void);
void kl_glfb_set_size(int w, int h);

// The flat door's zero-readback storage. Requested BEFORE ANGLE comes up
// (view_run does, for a flat Steam Link door), the root surface becomes a
// pbuffer over an IOSurface kl_glfb allocates: the guest's default framebuffer
// IS shareable storage, and the compositor samples it instead of anything
// calling glReadPixels. The accessor answers NULL until init — and forever, if
// the request was never made or the IOSurface route was refused (the caller
// keeps the readback sink in that case). Returns the IOSurfaceRef.
void  kl_glfb_request_flat_surface(void);
void *kl_glfb_flat_surface(int *w, int *h);

// The host's GL entry point for `name`, or NULL to mean "keep your own answer" —
// which is what the capability queries get, since the guest must go on believing
// it is driving GLES 3.2.
void *kl_glfb_sym(const char *name);

// Bind the guest's context to the calling thread. Call from eglMakeCurrent: a
// GL context is per-thread, and the guest's render thread is not the one that
// created it. `guest_ctx` is the guest's own EGLContext handle — each distinct
// one is backed by its own real ANGLE context in a single share group, which is
// what lets UE4 drive GL from the RHIThread while the game thread holds another.
void kl_glfb_make_current(void *guest_ctx);

// Seed ANGLE's per-thread TLS on a guest thread. Call once from the guest
// thread trampoline, before the thread can issue GL: the DEBUG ANGLE the host
// links asserts on that TLS at the top of every GLES entry point and
// dereferences NULL when a thread reaches GL without ever touching EGL. See the
// block above the definition in kl_glfb.c. No-op until GL is up, and off with
// KL_GLFB_THREAD_TLS=0.
void kl_glfb_thread_tls_touch(void);

// The release half: call from eglMakeCurrent when the guest makes no context
// current. Frees this thread's guest context for the next thread to take.
void kl_glfb_release_current(void);

// Read the eye framebuffer back and write <dir>/frame_NNN.png. Returns the number
// of frames presented so far. When a frame sink is registered the buffer goes
// there instead (dir may then be NULL) and no PNG is written.
unsigned kl_glfb_present(const char *dir);

// r26 -- arm the capture and the frame trace from the guest side.
//
// KL_GLFB_OUT_ARM_CSM arms on the first depth-only framebuffer, which on a
// guest that runs shadow cascades from the first world frame means "the front
// end", minutes before whatever is actually being investigated. This is the
// other door: the target's own guest interposer calls it when it knows the
// interesting moment has arrived (SHAR calls it from
// InteriorManager::Enter -- the player pressing the button on a doorway), and
// KL_GLFB_OUT_ARM_INTERIOR=1 makes that arm the gate. Idempotent, safe from any
// thread, and inert unless the knob is set. Either arm is sufficient when both
// KL_GLFB_OUT_ARM_CSM and KL_GLFB_OUT_ARM_INTERIOR are on.
void kl_glfb_arm_capture(void);

// ---- the SHAR radar capture, round 37 --------------------------------------
//
// Driven from the BeginRadarCapture / EndRadarCapture interposers in kl_sdl.c,
// because the bug they close is the guest's and the guest is where the call
// sites are; the GL lives here. See the "round 37: EndRadarCapture" block in
// kl_glfb.c for what BeginRadarCapture gets wrong and why the smallest fix is
// to remember the binding it was entered with.
int32_t kl_glfb_shar_radar_binding(void);
void    kl_glfb_shar_radar_restore(int32_t want);
void    kl_glfb_shar_radar_endpass(void);

// KL_GLFB_FRAMETRACE=N, cached. Non-zero means the ordered per-frame
// clear/draw/bind trace is wanted, which is what makes the swap worth calling
// kl_glfb_present on even with no capture directory and no frontend attached.
int kl_glfb_frametrace(void);

// The frontend seam, frame out. Registering a sink turns the per-swap capture
// into a handoff: instead of PNG-encoding to KL_GLFB_OUT, the readback buffer
// (bottom-up RGBA rows, exactly as glReadPixels produced it) is handed to `fn`
// on the GL thread, and a capture is requested on EVERY eglSwapBuffers. The
// sink must be fast — it runs inside the guest's frame — and must not call back
// into GL or SDL; memcpy and go. With no sink registered the KL_GLFB_OUT PNG
// behaviour is byte-identical to before.
typedef void (*kl_glfb_frame_sink)(const uint8_t *rgba, int w, int h, void *ctx);
void kl_glfb_set_frame_sink(kl_glfb_frame_sink fn, void *ctx);
int  kl_glfb_has_frame_sink(void);

// Lit-pixel count (luma > 12) of the most recent readback — the same diagnostic
// the PNG path prints, kept available so a sink frontend can tell a blank frame
// from a dead pipeline.
unsigned long kl_glfb_last_frame_lit(void);

// ---------------------------------------------------------------------------
// The frame-out seam, GPU edition — what a hardware compositor needs instead of
// the sink above. The sink hands over *pixels*, which costs a glReadPixels of
// the whole eye (a full pipeline stall plus ~40 MB at 2198x2304 RGBA16F) and a
// tone map and two memcpys before anything is displayed. A compositor that
// samples the eye's MTLTexture directly needs none of that; what it needs is
// *ordering*, because the guest's rendering is in ANGLE's Metal command queue
// and the composite is in ours, and Metal does not order across queues.
//
// So: register an id<MTLSharedEvent>, and each swap encodes a signal of it into
// ANGLE's command stream after the guest's frame (EGL_ANGLE_metal_shared_event
// _sync) and flushes. The compositor reads the value below and does
// encodeWaitForEvent: before its pass. Nothing is read back, nothing is copied.
//
// Registering a fence takes priority over the CPU sink: they are two answers to
// the same question and the swap does one or the other, never both.
void kl_glfb_set_gpu_fence(void *mtl_shared_event);
int  kl_glfb_has_gpu_fence(void);

// The value the most recent swap's signal will reach, or 0 before the first.
// Read from the compositor thread; monotonically increasing, so an unchanged
// value means "no new guest frame since you last looked".
uint64_t kl_glfb_gpu_fence_value(void);

// Print the internalformats the guest allocated immutable texture storage with.
// KL_GLFB_SKIP bisected the AGX abort to glTexStorage* + glTexSubImage*, which
// makes this the population that matters. Safe to call when nothing was allocated.
void kl_glfb_report_formats(void);

// The GL object census (KL_GL_CENSUS=<swaps>): per class, how many objects the
// guest has created and how many it has deleted, plus the immutable texture and
// renderbuffer storage still live. Every gen/delete goes straight to ANGLE, so
// without this nothing at the seam can say whether the guest's GL objects are
// being reclaimed — which is the first question when memory climbs across scene
// loads. A no-op unless KL_GL_CENSUS is set; safe to call at any time.
void kl_glfb_gl_census(FILE *f);

// Where the guest's draws actually landed: one row per (framebuffer, thread),
// with the colour attachment the framebuffer last carried. Always collected,
// and the first thing to read when a picture is missing — "the eye texture is
// black" and "the guest never drew into the eye texture" are the same symptom
// and completely different work.
void kl_glfb_draw_census(FILE *f);

// The program-link ledger: how many linked, the slowest one, and — the reason
// this exists — the link that was still running when the report was written. A
// wedged render thread inside ANGLE's link event leaves no other trace, and r10
// spent 33 s in one without the log ever naming the program.
void kl_glfb_link_census(FILE *f);
// r22: the cached-binary link path, which had no instrument at all.
void kl_glfb_program_binary_census(FILE *f);

// The generation of klfb_rewrite_glsl's OUTPUT, and it belongs to the shader
// cache exactly as much as the driver's build number does.
//
// A guest's ProgramBinaryCache stores the compiled form of the GLSL WE handed
// ANGLE, not the GLSL the guest wrote — so a change to any rewrite rule leaves
// every cached blob a compiled copy of the OLD text, and the guest reloads it
// happily and silently. r24 had to bake a one-shot KL_SHADER_CACHE_DROP token
// by hand because a rewrite change had no way to invalidate a cache it does not
// own; this is that, made automatic and general.
//
// BUMP THIS whenever klfb_rewrite_glsl emits different text. It is folded into
// shader_cache_guard's fingerprint beside kern.osversion and hw.model, so the
// drop then happens by itself, on every target, exactly once per install.
//
//   1  everything up to and including r23
//   2  r25 — scene depth: DepthbufferFetchES2 samples the depth attachment
//   3  r27 — volumetric fog: a vertex-stage sampler3D textureLod becomes
//            vec4(0,0,0,1), UE4's own value for "no volumetric fog"
//   4  r28 — the fog COMPOSITE: a fragment shader that reads in_TEXCOORD7
//            shadows it with vec4(0,0,0,1), removing the composite itself.
//            The mechanism test the volumetric guess skipped.
//   5  r29 — NO text change: the bump is deliberate and is here to drop the
//            PROGRAM BINARY cache. r29's worn log named the fill's program and
//            could not dump its GLSL, because a program linked from a cached
//            binary has no attached shaders and this process never saw its
//            source. One long compile buys the named program's own text.
#define KL_GLFB_REWRITE_GEN 5

// The sampler-LOD ledger: mipmapped minification filters, level caps, LOD
// clamps and biases, anisotropy, glGenerateMipmap, and how many levels the
// guest's texture allocations asked for. Written to answer the one question
// r20's mip-level reading of the tetris blue fill could not ask from a log —
// whether the content being sampled has a mip chain at all. Free, always on.
void kl_glfb_lod_census(FILE *f);

// The MIP-CHAIN ledger (r30): for every texture the guest allocated with more
// than one level, how many of those levels ever received an upload and how many
// bytes each got, against the bytes its dimensions imply. NOMIP=1 removed the
// blue fill, so the sample came from a level above 0; this says whether that
// level had real content in it or was allocated storage nobody ever wrote.
// Free — the destination texture name comes from the bind tracker rather than a
// glGetIntegerv round trip — and always on.
void kl_glfb_mipchain_census(FILE *f);

// The eye-stage association: which texture names are registered as which stage,
// which framebuffers the sixteen-slot map holds, and which eye framebuffers only
// the wider colour table knows about. r11 drew 189,574 times into eye textures
// and filed none of them, and this is the state that says why.
void kl_glfb_eye_map_census(FILE *f);

// The censuses above, printed periodically DURING the run.
//
// On device the exit report is unreachable: KL_EXIT_ON_BACKGROUND=0 is baked
// for these targets (the app must not exit when its window closes) and a force
// quit is SIGKILL, so kl_egl_report never runs. Two rounds of the Tetris
// bring-up were diagnosed from a HOST exit report describing a bug that only
// happens on device. Call this from the compositor's own loop.
//
// `why` NULL means "the timer, if it is due"; a string means "flush now and
// label it", for the layer-paused transition — the one event a closed window is
// guaranteed to deliver. KL_GLFB_CENSUS_SECS sets the period (default 15 s).
void kl_glfb_live_census(const char *why);

// The eye-texture seam, texture identity out. kl_ovrp's SetupEyeTexture2 calls
// this with the GL texture name Unity handed down; the capture finds "the
// framebuffer with the picture" by looking for the FBO whose color attachment
// is one of these (fb0 is black by construction — the VR frame goes to eye
// textures, not the backbuffer).
void kl_glfb_note_eye_texture(int eye, int stage, uint32_t tex);

// ...and which of an eye's images the guest most recently PRESENTED. Registering
// a swapchain leaves "the eye texture" naming whichever image was registered
// last — one of three, so the capture reads the picture the guest drew one frame
// in three and black the rest. The OpenXR path knows which image it presented
// (xrReleaseSwapchainImage names it) and says so here.
void kl_glfb_set_live_eye_texture(int eye, uint32_t tex);
// ...and the full description, which is what an OpenXR guest can state and the
// capture cannot recover: the swapchain is allocated through the real ANGLE
// entry points (so kl_glfb never sees its size) and both eyes are LAYERS of one
// array texture (so there is no per-eye name at all). `layer` is -1 for a plain
// 2D texture. KL_XR_CAPTURE_EYE picks which eye the capture writes.
void kl_glfb_set_live_eye_image(int eye, uint32_t tex, int32_t w, int32_t h, int layer);

// ...and the teardown half, from ovrp_DestroyEyeTexture. Drops both references
// that keep an eye texture's storage alive — the EGLImage and the GL texture —
// so the provider's own release on reallocation can actually reclaim it. A
// no-op for a slot that was never bound; out-of-range says so and does nothing.
void kl_glfb_release_eye_texture(int eye, int stage);

// Which swapchain stage the guest most recently RENDERED into, or -1 if that
// has not been observed.
//
// This exists because the stage is the guest's choice and we had been guessing
// it. With one stage the guess could not be wrong; with more than one it can,
// and a wrong answer is not a missing picture but a *stale* one paired with a
// *fresh* pose — reprojection then corrects by the wrong delta, which is
// judder. So the stage is measured instead: kl_ovrp registers each (eye,stage)
// texture name above, and the framebuffer thunks watch which of those names is
// the draw target. See kl_ovrp.c's EndFrame, which files the frame's pose
// record under whatever this reports.
int kl_glfb_last_render_stage(void);

// ...and the same observation with a window around it, which is what makes it
// checkable rather than merely available.
//
// `kl_glfb_last_render_stage` is sticky, so it answers even for a frame that
// drew into no eye texture at all — and that answer is the PREVIOUS frame's
// stage, which is exactly the off-by-one that pairs a fresh pose with a stale
// picture and shows up as doubling on head rotation. A sticky global cannot
// report that it did not know; a window can.
//
// kl_ovrp opens a window at ovrp_BeginFrame and reads it at ovrp_EndFrame:
//
//   *mask   bit per stage bound as the draw target inside the window. Exactly
//           one bit is the healthy case; zero means nothing was observed and
//           the returned stage belongs to another frame; more than one means
//           the frame committed to several stages and "one stage per frame" is
//           not true for this guest.
//   *binds  how many eye binds happened, for telling "did not draw" from "drew
//           once" from "bounced".
//   *tid    the thread that did the last of them. If it is not the thread
//           calling EndFrame, the window bounds nothing and the association
//           needs ordering rather than better observation.
//
// The return value is the last stage observed, sticky, as before.
void     kl_glfb_begin_render_window(void);
int      kl_glfb_render_stages(uint32_t *mask, uint32_t *binds, uint64_t *tid);

// How many times `stage` has been committed to as the draw target over the
// whole run. This is pure observation — it does not pass through the pose
// bookkeeping — so a stage that stops being drawn into is visible here even
// when every other counter still looks healthy.
uint64_t kl_glfb_stage_draw_count(int stage);

// ...and the question none of the counters above can answer: is there a PICTURE
// in the stage, or is it black?
//
// Every observation on this seam is about addressing — which stage was bound,
// how often, by whom — and a guest that draws black into the right texture
// reads identically to one that draws a scene. Under KL_GLFB_STAGE_PROBE=N this
// reads two 32x32 patches out of every registered (eye, stage) every Nth call
// and reports min/mean/max over RGB, so a device log can say which of the two
// it is. Call it from the guest's render thread once the frame's draws are in
// (kl_ovrp's EndFrame); it is a no-op without the knob, and a no-op on a thread
// with no current context.
void kl_glfb_probe_stages(int frame);

// ---------------------------------------------------------------------------
// DoubleWide.
//
// A guest that asked for ovrpLayout_DoubleWide renders BOTH eyes into the eye-0
// texture — one viewport per eye, side by side — and never touches eye 1's
// swapchain at all. kl_ovrp turns this on where it bridges the request (it is
// the only thing that knows the guest asked); KL_GLFB_DOUBLEWIDE=0/1 overrides.
//
// What it changes: `kl_glfb_eye_mtl_texture(1, stage)` answers with EYE 0's
// texture and EYE 0's slice. Both eyes then sample the one picture the guest
// drew, and the compositor's per-eye render viewport — the two rects
// ovrp_EndFrame4 files, turned into one unwarp block per eye by
// kl_reproject_grid_build_eye — crops each eye's half out of it. That IS the
// per-eye source rect the old note here said was missing: it was built for
// BONELAB's symmetric projection and serves this unchanged, at full resolution
// and through the eye texture's rate map.
//
// `kl_glfb_split_doublewide` is the OLD answer and is now opt-in behind
// KL_GLFB_DOUBLEWIDE_SPLIT=1: it blits each half out to a whole eye texture at
// EndFrame. **It is wrong on device** — it works in logical screen coordinates
// on FOVEATED storage, un-squeezing a picture the unwarp grid is about to
// un-squeeze again, and reading unwritten texels for the right half. r13 saw
// all three of its symptoms in one headset session: a stretched, displaced left
// eye, a black right eye, and a flicker as un-split stages came round. Kept
// only for a present path with no per-eye viewport at all (the host viewer).
// `stage_mask` is kl_glfb_render_stages()'s — the stages the frame's draws
// actually landed in.
int  kl_glfb_doublewide(void);
void kl_glfb_set_doublewide(int on);
void kl_glfb_split_doublewide(uint32_t stage_mask);

// ---------------------------------------------------------------------------
// The Metal interop seam. Proven on host and device before any of this
// existed — `make mtltex` and device probe P13.
//
// On visionOS the guest's eye textures must live in MTLTextures the compositor
// can sample, so instead of allocating GL storage for the texture name Unity
// hands down, we back it with an MTLTexture the host allocated:
//
//     glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, h, w)   // host / no provider
//     glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image)   // with a provider
//
// With no provider registered nothing below changes behaviour, which is why the
// macOS path and `make check` are unaffected.

// How many (eye, slot) pairs the system carries, and therefore how many a
// PROVIDER must be able to hold.
//
// A slot is the guest's own swapchain stage on the OVRPlugin path
// (ovrp_GetEyeTextureStageCount's answer, indexed directly) and a compositor
// SLOT on the OpenXR one, where several projection layers' images are live at
// once and a swapchain's image index is not unique across them — see
// kl_glfb_mirror_eye_layer. The two never share a process, so one table serves
// both, and what the count has to cover is the OpenXR case: every (swapchain,
// image) pair the guest keeps in flight, which for a three-layer guest is nine.
//
// **A provider sized smaller than this declines the slots past its own end**,
// and a declined slot is a layer with no storage: the mirror fails, the frame
// composes nothing, and a compositor either presents black or holds. Measured
// on the host provider, which held four — most frames of a two-swapchain guest
// failed to compose, seen first as flicker and then, once the compositor held
// instead, as two seconds a frame.
#define KL_MTL_MAX_STAGES 16

// What the host supplies for one (eye, stage). `texture` is an id<MTLTexture>
// which the host must keep alive for as long as the guest holds the eye texture.
// `slice` addresses an array slice, which is how the two eyes arrive in a
// layered drawable; 0 for a plain 2D texture.
//
// `w`/`h` are the texture's ACTUAL dimensions, and the provider must fill them:
// EGL_ANGLE_metal_texture_client_buffer takes the size from the MTLTexture and
// ignores everything we say about it, so a provider that returns a texture of
// the wrong size gets a successful eglCreateImageKHR and a guest rendering into
// storage smaller than it believes it has. That is exactly what happened the
// first time this ran — Unity re-creates its eye textures at a *different* size
// partway through startup (measured: 1832x1920, then 2198x2304), the provider
// handed back its one cached texture, and the result was a partially-filled
// frame with nothing anywhere reporting a problem. So kl_glfb refuses a
// mismatch rather than trusting the caller.
typedef struct { void *texture; int slice; int w, h; } kl_mtl_eye_texture;

// Called when a guest asks for eye storage — ovrp_SetupEyeTexture2 for Unity,
// xrEndFrame's eye assertion for an OpenXR guest. Return non-zero having filled
// `out`, or 0 to let the guest get ordinary GL storage. w/h are the dimensions
// the texture must have — note they arrive already transposed relative to
// ovrp's arguments; see kl_ovrp.c's SetupEyeTexture2.
//
// **`internal_fmt` is not advisory.** ANGLE compares the GL internal format we
// claim against the MTLTexture's own pixel format and refuses the pair outright
// — `TextureImageSiblingMtl::ValidateClientBuffer`, "Incompatible format" — so
// a provider that always allocates RGBA16F simply cannot back a guest that
// asked for anything else. Unity asks for RGBA16F and Steam Link asks for
// SRGB8_ALPHA8, which is why this is a parameter rather than a constant.
typedef int (*kl_glfb_mtl_provider)(int eye, int stage, int w, int h,
                                    uint32_t internal_fmt,
                                    kl_mtl_eye_texture *out, void *ctx);
void kl_glfb_set_mtl_provider(kl_glfb_mtl_provider fn, void *ctx);
int  kl_glfb_has_mtl_provider(void);

// ANGLE's own MTLDevice (an id<MTLDevice>), or NULL. The host MUST allocate on
// this device: EGL_ANGLE_metal_texture_client_buffer requires the texture come
// from the display's device, and passing one from MTLCreateSystemDefaultDevice()
// fails with EGL_BAD_PARAMETER for a reason nothing else would explain.
void *kl_glfb_mtl_device(void);

// Ask the registered provider for (eye, stage) and give `gl_tex` storage backed
// by what it returns. Returns 1 if the texture is now MTLTexture-backed, 0 if
// the caller should allocate GL storage itself. `internal_fmt` is the GL
// internalformat the guest asked for.
int kl_glfb_bind_eye_mtl_texture(int eye, int stage, uint32_t gl_tex,
                                 int w, int h, uint32_t internal_fmt);

// The MTLTexture currently backing (eye, stage), or NULL — what the compositor
// pass samples, and what a test reads back to check the guest's frame arrived.
void *kl_glfb_eye_mtl_texture(int eye, int stage, int *out_slice);

// Record an MTLTexture that is an eye's storage but did NOT come through the
// GL path above — the Vulkan one (kl_vulkan.c). There the guest renders into a
// VkImage, MoltenVK backs that with an MTLTexture of its own, and there is no
// EGLImage and no GL name anywhere in the picture; what the compositor needs is
// the same thing either way, so it goes in the same table and every consumer of
// `kl_glfb_eye_mtl_texture` is unchanged.
//
// `slice` is the array layer, which is how BOTH eyes arrive on the Vulkan path:
// the Array layout puts them in one two-layer image (see kl_ovrp_multiview).
// Registering the same texture twice with different slices is the normal case
// here, not a mistake.
//
// This does not take a reference. The image belongs to kl_vulkan.c for the life
// of the swapchain generation, and releasing it is that file's business — which
// is why this is `note` and not `bind`: nothing here allocates, so nothing here
// frees.
void kl_glfb_note_eye_mtl_texture(int eye, int stage, void *texture, int slice,
                                  int w, int h);

// Copy one array LAYER of a guest-owned texture into a provider-backed 2D eye
// texture, and return the SLOT that now holds that picture (-1 on failure).
//
// This is the ARRAY swapchain's way onto the compositor, and it exists because
// the zero-copy route cannot serve one. `kl_glfb_bind_eye_mtl_texture` re-points
// the guest's own GL name at an MTLTexture, which needs the name to be a
// GL_TEXTURE_2D: ANGLE's Metal backend reduces every EGLImage sibling to a slice
// view (`TextureImageSiblingMtl::initImpl`), so an EGLImage is always 2D and a
// GL_TEXTURE_2D_ARRAY has nothing to bind to. A Unity OpenXR guest asks for a
// 2-slice swapchain and renders BOTH eyes into it — so on that path the choice
// is a copy or a black display, and it was a black display.
//
// The copy is one blit per eye per frame into storage the compositor already
// owns, so nothing about the compositor, the reprojection pass or the frame
// record changes: the destination is the same provider slice `SetupEyeTexture2`
// would have handed a Unity/OVRPlugin guest. `image` is the swapchain image the
// guest just presented, so that the destinations rotate exactly as the guest's
// own images do — a single destination would be blitted into while the
// compositor is sampling it.
//
// `src_layer` is -1 for a plain 2D source. Returns -1 with a named reason if
// there is no provider, no interop, or the blit failed; the caller then has an
// eye the compositor cannot sample, which is the state this replaces.
//
// **The return value is the key the picture is read back with** —
// `kl_glfb_eye_mtl_texture(eye, slot, ...)` — and it is NOT `image`. A
// swapchain's image index runs 0..2 whatever swapchain it belongs to, so two of
// a guest's projection layers name the same index in the same frame; keying the
// destinations on it is why only one projection layer could ever be composited.
// The slot is allocated per (`source`, `image`) instead, so every image a guest
// keeps in flight has a destination of its own and any number of layers coexist.
//
// `source` identifies WHICH swapchain these images come from. Any stable
// per-swapchain value will do; 0 means "unknown" and then `image` IS the slot,
// which is the OVRPlugin path's direct keying and what a caller with nothing to
// give still gets.
int kl_glfb_mirror_eye_layer(int eye, int image, uint32_t source,
                             uint32_t src_tex, int src_layer,
                             int w, int h, uint32_t internal_fmt);

// ...and the teardown half: release every slot allocated to `source`. Call it
// when the swapchain goes, or its slots stay claimed by something that no
// longer exists and the pool fills with the dead.
void kl_glfb_forget_eye_source(uint32_t source);

// Diagnostic-only: read `tex` back and log its lit-pixel count under `tag`,
// gated on KL_GLFB_PROBE_VIDEO. Lets a caller outside kl_glfb (the OpenXR
// layer loop) ask "is THIS projection layer's image lit?" for every layer,
// not just the one the compositor happens to mirror.
// Read `tex` back and write it as a PNG. Diagnostic; returns non-zero on
// success. Exists so two layers that overlap in the picture can be compared
// AGAINST EACH OTHER rather than against the field of view each claims — the
// only way to measure a seam whose sides are doubled rather than just softer.
int kl_glfb_dump_tex(const char *path, uint32_t tex, int layer, int w, int h);

void kl_glfb_probe_tex(const char *tag, uint32_t tex, int layer,
                       int hint_w, int hint_h);

// Allocate the compositor's eye-mirror textures LARGER than the guest's eye by
// num/den, upscaling the base into them. For a foveated guest this is what gives
// the high-resolution centre inset (kl_glfb_overlay_eye_inset) room to land at
// full detail instead of being shrunk into the base's own resolution. 1/1 (the
// default) is unchanged behaviour. Set once, before the eyes are first mirrored.
void kl_glfb_set_eye_mirror_scale(int num, int den);

// Cap the eye-mirror texture's larger side (pixels). Keeps the scale above from
// ballooning a base that is already near the display resolution. Default 3456.
void kl_glfb_set_eye_mirror_cap(int px);

// Composite a foveal inset over the base eye already mirrored into (eye, stage).
// `tex`/`layer` are the inset swapchain image (iw×ih); nx0..ny1 are where the
// inset's frustum falls inside the base eye, as fractions in [0,1]. For a guest
// that streams foveated (a wide low-detail base plus a narrow high-detail
// centre — Steam Link), this is what puts the sharp centre back on screen. Must
// be called AFTER kl_glfb_mirror_eye_layer for the same (eye, stage).
void kl_glfb_overlay_eye_inset(int eye, int stage, uint32_t tex, int layer,
                               int iw, int ih,
                               float nx0, float ny0, float nx1, float ny1);

// Which way up is the picture in (eye, stage)? 1 = row 0 is the TOP of the
// image, 0 = row 0 is the bottom.
//
// This is not a detail and it has no error surface: a flip is a correct-looking
// picture that is wrong, and it is a property of the API the GUEST rendered
// with, not of Metal. GL puts the origin at the bottom left, so every eye
// texture ANGLE filled has to be flipped when it is sampled or written out —
// which is how the readback in tests/t_mtl_provider.m was written and why it
// flips unconditionally. **Vulkan's origin is the TOP left, the same as
// Metal's**, so an eye a Vulkan guest rendered must NOT be flipped, and doing
// it anyway produces the loading screen upside down with everything else about
// the pipeline working — measured, on the first run of the Vulkan compositor
// seam.
int kl_glfb_eye_mtl_origin_top_left(int eye, int stage);

// KL_XR_ARRAY_DIRECT: answer the guest's glFramebufferTextureLayer(array_tex,
// layer) with glFramebufferTexture2D(tex2d) — an array swapchain's slice served
// as a 2D texture whose storage is the compositor's (see kl_glfb.c). tex2d 0
// removes the entry. Returns 0 only when the table is full (said in the log).
int kl_glfb_array_redirect(uint32_t array_tex, int layer, uint32_t tex2d);
// The GL name currently bound to (eye, stage)'s compositor storage, 0 if none.
uint32_t kl_glfb_eye_gl_texture(int eye, int stage);
// KL_GLFB_LAZY_UNBIND: forward a held draw-unbind now (callers that reach
// ANGLE around kl_glfb's thunks; no-op when nothing is held).
void kl_glfb_lazy_flush_now(void);

// ---------------------------------------------------------------------------
// The same seam for a layer that is NOT an eye.
//
// An OpenXR guest can present its whole frame as an XR_TYPE_COMPOSITION_LAYER_
// QUAD — a flat panel with a pose and a size in metres — and JKXR presents
// nothing else: 7381 of 7381 layers in a host run, one per frame. The
// compositor's overlay pass (kl_reproject.h) draws exactly that, and what it
// needs is an MTLTexture, which on the GL path only exists if the swapchain's
// images were given one. That is this pair, and it is the eye pair with the key
// widened: `layer` is the caller's own identifier for the layer (kl_openxr uses
// the swapchain's slot), and the record a compositor reads is
// kl_ovrp_overlay.layer_id.
//
// A SEPARATE provider from the eye one, rather than the same callback with a
// third meaning for `eye`: an eye is one of two slices of one shared array
// texture and a layer is a plain 2D texture of the guest's own size, so a
// frontend serving both from one allocator would have to tell them apart by a
// convention no signature states. Registering none is legal and means a quad
// layer has no MTLTexture, which every frontend that predates this did.
typedef int (*kl_glfb_mtl_layer_provider)(int layer, int stage, int w, int h,
                                          uint32_t internal_fmt,
                                          kl_mtl_eye_texture *out, void *ctx);
void kl_glfb_set_mtl_layer_provider(kl_glfb_mtl_layer_provider fn, void *ctx);
int  kl_glfb_has_mtl_layer_provider(void);

// Copy the guest's presented image into storage the provider owns, and return 1
// if (layer, stage) now holds that picture. One blit per submitted frame.
//
// **A copy, and NOT the eye path's re-point, and that is a measurement rather
// than a preference.** Giving the guest's own swapchain texture MTLTexture
// storage (`glEGLImageTargetTexture2DOES` over the name it is already
// rendering into) is what the eye path does, and on JKXR it BROKE the guest's
// rendering: with the re-point the intro movie came out as a small copy in one
// corner of the image plus a flipped one in another, and with the provider
// simply not registered — the same run, same frame — it is a correct
// full-screen picture. Every GL call succeeded either way. That guest attaches
// its swapchain images to framebuffers it builds once and keeps, and replacing
// the storage underneath a live attachment is not something the ES spec
// promises anything about.
//
// So the guest's storage is left alone and its finished image is copied. The
// destination is a GL name of OURS, which nothing else has ever attached, so
// the hazard cannot arise; everything downstream — the layer table, the
// compositors, the readback — sees exactly what the re-point produced when the
// re-point worked.
int kl_glfb_mirror_layer(int layer, int stage, uint32_t src_tex, int src_layer,
                         int w, int h, uint32_t internal_fmt);

// Record an MTLTexture that IS a layer's storage but did not come through the
// GL path above — the Vulkan one, where MoltenVK backs the guest's own image and
// there is nothing to re-point. The eye table's `note` twin, and for the same
// reason: one table, so every consumer of the lookup below is unchanged.
// Takes no reference; the image belongs to kl_vulkan.c.
void kl_glfb_note_layer_mtl_texture(int layer, int stage, void *texture,
                                    int w, int h);

// ...and what the compositor samples. NULL when nothing was bound for that
// (layer, stage) — which is the ordinary answer on a run with no provider, and
// the one the compositors name once per layer rather than drawing nothing
// silently. `out_w`/`out_h` may be NULL.
void *kl_glfb_layer_mtl_texture(int layer, int stage, int *out_w, int *out_h);

// ---------------------------------------------------------------------------
// The decoded-video image — the guest's eglCreateImageKHR over an
// AHardwareBuffer, which here is a VideoToolbox CVPixelBuffer. kl_egl.c serves
// the guest-facing entry points; the ANGLE work is here because this file owns
// the display, the config and the entry-point resolution.
//
// `pixels` is a CVPixelBufferRef (void* so callers need not include CoreVideo).
// NULL means the frame could not be made samplable and the caller must say so to
// the guest rather than hand it an image that will not sample. See kl_glfb.c for
// why this is an IOSurface pbuffer and not an EGLImage: ANGLE's Metal backend
// has neither external images nor Android buffers.
void *kl_glfb_image_from_pixels(void *pixels, int *out_w, int *out_h);
void  kl_glfb_image_destroy(void *image);

// Bind `image` to whatever the current context has bound to GL_TEXTURE_2D —
// which is where the guest's glBindTexture(GL_TEXTURE_EXTERNAL_OES, ...) landed
// after klfb_detarget. This is what glEGLImageTargetTexture2DOES becomes.
int   kl_glfb_image_bind(void *image);

// Is this handle one of ours? Asked of pointers the guest chose, so it is a
// registry lookup and not a magic-word peek through a foreign pointer.
int   kl_glfb_is_image(const void *h);

// ---------------------------------------------------------------------------
// Foveation — variable rasterization rate for the guest's own rendering.
//
// `rate_map` is an id<MTLRasterizationRateMap> the PLATFORM built, on the same
// MTLDevice as the eye textures, for a screen size of `w` x `h`. The runtime
// never constructs one, for the same reason it never constructs an MTLTexture:
// that keeps this file plain C and puts the Metal object where the display
// numbers are (KleptonCompositor.swift on device, t_mtl_provider.m on host).
// NULL turns foveation off again.
//
// Two registrations, because the guest's per-eye chain has two render targets
// and they must share ONE map (measured):
//
//   * the eye texture, by identity, as each one is bound;
//   * the guest's multisampled SCENE target, by size — ANGLE allocates that
//     renderbuffer itself in response to glRenderbufferStorageMultisample, so
//     there is no object anyone outside ANGLE can name. The rule is qualified
//     by sample count so it hits only that target: the guest also allocates
//     single-sampled post-processing intermediates at the eye size, and
//     foveating one of THOSE is a wrong picture, because the guest samples it
//     with screen-space coordinates that know nothing about the warp.
//
// Sharing the map is what keeps the resolve between them a physical-to-physical
// copy rather than a second warp — see the passthrough mode in
// angle-patches/klepton.patch, and `make vrr` Q6, which measures it.
//
// **The eye texture is left in a WARPED layout by this**, so whoever samples it
// has to unwarp: the compositors, and every readback path
// (`KL_GLFB_OUT`, t_mtl_provider). Until those do, leaving this unset is the
// only correct configuration.
// `zones_x`/`zones_y` are the rate layer's sample counts — the number of equal
// screen-space regions the map divides each axis into. They travel with the map
// because a built MTLRasterizationRateMap does not report them, and the
// compositor's unwarp needs them: a rate map is piecewise linear with its
// breakpoints at those boundaries, so an unwarp grid of exactly this many cells
// is EXACT, where any other count only samples the curve. See kl_reproject.h.
void kl_glfb_set_eye_rate_map(int w, int h, int zones_x, int zones_y, void *rate_map);

// The zone counts the map in force was built with; both 0 when there is none.
void kl_glfb_eye_rate_zones(int *zones_x, int *zones_y);

// The map in force, or NULL. What a compositor's unwarp needs to build its
// coordinate conversion from, and how a readback path knows the eye texture is
// not an ordinary image.
void *kl_glfb_eye_rate_map(void);

// 1 when the eyes reach the compositor by the array-swapchain COPY and that copy
// carries the guest's foveated picture (KL_VRR_MIRROR=1 with a map in force), so
// the compositor's per-layer composite is unwarping a MIRROR, not a re-pointed
// eye. For the log line only; the unwarp itself does not depend on it.
int kl_glfb_eye_mirror_foveated(void);

// The foveation POLICY — how many zones and how hard the periphery falls off —
// held here rather than in either builder, because there are two of them
// (t_mtl_provider.m on the host, KleptonCompositor.swift on device) and a
// device that foveated differently from the host would make every host
// measurement a lie about the thing being shipped. The Metal object is still
// built where the display numbers are; only the numbers that describe it are
// shared.
//
// Returns 1 when foveation is asked for (`KL_VRR`), 0 otherwise. `*zones` gets
// the per-axis zone count (`KL_VRR_ZONES`, default 16) and `*edge` the rate at
// the border (`KL_VRR_EDGE`, default 0.35); both are clamped to what a rate map
// will accept. Either pointer may be NULL.
int kl_glfb_foveation_wanted(int *zones, float *edge);

// The rate layer's per-zone quality along one axis: 1.0 at the centre falling
// linearly to `edge` at the border, `n` entries. Both axes get the same curve,
// so this is called once per axis with the same arguments.
void kl_glfb_foveation_quality(float *q, int n, float edge);

// The largest zone count kl_glfb_foveation_wanted will report, so a caller can
// size a stack array for the curve above without allocating.
#define KL_FOVEATION_MAX_ZONES 64

#endif
