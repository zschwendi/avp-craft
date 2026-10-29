// libvulkan.so — the synthetic Vulkan loader, over MoltenVK.
//
// BONELAB boots completely and cannot render because its graphics API is Vulkan
//. Unity `dlopen`s `libvulkan.so` as the first thing it
// tries, and it is the only API this build probes: when the open fails, Unity
// warns that the device does not meet the application's hardware requirements
// and falls back to a GLES 2 context whose shaders do not exist in the blob.
// So this file is what turns the first domino.
//
// It is served, not loaded, for exactly the reason kl_ovrp and kl_openxr are:
// the real `libvulkan.so` is Android's loader and would go looking for an ICD
// through Android's driver plumbing. MoltenVK IS the ICD here, so this stands
// in for the loader and nothing else.
//
// **There is no ABI translation anywhere in this file, and that is a measured
// property rather than an assumption.** Vulkan's structs are plain C with
// explicit types and the guest is aarch64 LP64 little-endian, exactly like the
// host — so a `VkImageCreateInfo` the guest fills in is byte-identical to one
// this process would build. That is what makes a 182-entry-point API tractable
// as a forwarding table, and it is emphatically NOT true of the JNI, libc or
// OVRPlugin seams next door. `make vkabi` is the gate that keeps it true.
//
// Three things are genuinely ours rather than forwarded, and they are all the
// same thing wearing different hats — Android's window system does not exist:
//
//   1. `VK_KHR_android_surface` / `vkCreateAndroidSurfaceKHR`. MoltenVK has no
//      such extension and never will. The surface is synthesized.
//   2. the `VK_KHR_swapchain` family. Ours end to end, backed by ordinary
//      VkImages we allocate — no CAMetalLayer, no presentation engine, nothing
//      that has to be on a screen. This is what lets a frame be produced with
//      the compositor wiring still an open question.
//   3. `vkQueuePresentKHR`, which is where the frame is read back and dumped.
//
// Everything else is MoltenVK's, reached through its own `vkGetInstanceProcAddr`.
#ifndef KL_VULKAN_H
#define KL_VULKAN_H

#include <stdio.h>   // kl_vulkan_memory_report writes to a FILE *

// The synthetic-library quartet, same shape as kl_egl / kl_ovrp / kl_mediandk.
// See kl_dl.c's klb_dlopen and kl_can_dlopen for why `claims` exists separately
// from `dlopen`: an existence check the guest makes on its own behalf has to be
// answerable without loading anything.
void *kl_vulkan_dlopen(const char *soname);   // NULL if not one of ours
// libadrenotools' adrenotools_open_libvulkan, answered with the handle above
// (the Alyx jailbreak host asks through it). See kl_vulkan.c.
void *kl_vulkan_adrenotools_open(int dlopen_mode, int feature_flags,
                                 const char *tmp_lib_dir, const char *hook_lib_dir,
                                 const char *custom_driver_dir,
                                 const char *custom_driver_name,
                                 const char *file_redirect_dir,
                                 void **user_mapping_handle);
int   kl_vulkan_claims(const char *soname);   // the same test, without opening
int   kl_vulkan_is_handle(const void *h);
void *kl_vulkan_sym(const char *name);        // dlsym semantics: NULL when absent

// Tier lookup for kl_shim_lookup, for a guest that binds vk* at load time
// through a DT_NEEDED rather than through dlopen/dlsym. libunity takes the
// dlopen road; libSLZQuestNative names six vk* symbols directly.
void *kl_vulkan_lookup(const char *name);

// Was MoltenVK actually reachable? Separated from `claims` so the failure can be
// reported once, by name, at the point the guest asks — rather than as a NULL
// symbol somewhere inside Unity's device selection.
int   kl_vulkan_available(void);

// A glibc/Linux guest (the native Alyx door): list VK_KHR_xcb_surface,
// VK_KHR_xlib_surface and VK_KHR_wayland_surface in the global extension
// enumeration and take them off vkCreateInstance's request. Off by default.
void  kl_vulkan_set_linux_wsi(int on);

// Feature audit of a VkDeviceCreateInfo (phys = VkPhysicalDevice, ci = const
// VkDeviceCreateInfo *) against MoltenVK's vkGetPhysicalDeviceFeatures2: logs
// the requested set once per `who`, CLEARS in place every flag MoltenVK does
// not report (each named), returns how many. klvk_CreateDevice runs it on
// every create and puts the cleared flags back after MoltenVK answers; a door
// that must change what the guest's host SEES (the Alyx host samples its EDS3
// bits before the create reaches us) calls this first, and those stay cleared.
int   kl_vulkan_feature_audit(void *phys, const void *ci, const char *who);

// The VkDevice an OpenXR session was created with (kl_openxr's xrCreateSession):
// swapchain images are made on it. Unbound, the first device is used.
void  kl_vulkan_xr_bind_device(void *vk_device);

// ---------------------------------------------------------------------------
// The eye-texture seam, for kl_ovrp.c
// ---------------------------------------------------------------------------
//
// BONELAB never creates a WSI swapchain: Unity's Oculus XR path takes its eye
// textures from `ovrp_GetLayerTexture2` and submits them to the Oculus
// compositor. kl_ovrp.c has always answered that with a GL texture name, and on
// a Vulkan guest the name goes straight into `vkCreateImageView` as a VkImage
// handle — a segfault inside MoltenVK with a small integer for an address.
//
// So on the Vulkan path the eye textures have to be real VkImages, and these
// are how kl_ovrp.c asks for them without knowing anything about Vulkan.

// Did the guest actually bring up a Vulkan device through us? This is the test
// kl_ovrp.c branches on, and it is a measurement of what the guest DID rather
// than a guess from a renderer enum: a guest that resolved vk* symbols and then
// chose GLES would answer false here, correctly.
int kl_vulkan_guest_active(void);

// Create (or return) the eye image for a swapchain stage and eye. The returned
// value is the VkImage handle widened to 64 bits, which is exactly what
// ovrp_GetLayerTexture2's out-parameter carries.
unsigned long long kl_vulkan_eye_image(int stage, int eye, unsigned w, unsigned h,
                                       int srgb);

// ...and the same thing for the layout where the two eyes are ARRAY LAYERS of
// one image rather than two images — ovrpLayout_Array, which is what Unity
// calls Single Pass Instanced / Multiview and what a Quest title normally ships
// with. `layers` is 2 there and 1 for Stereo. Under Array both eyes get the
// SAME handle back and the eye is the layer index, so kl_ovrp.c must not treat
// two identical handles as a mistake.
unsigned long long kl_vulkan_eye_image_layers(int stage, int eye, unsigned w, unsigned h,
                                              int srgb, int layers);

// ...and the general form, which is what `ovrp_GetLayerTexture2` actually asks:
// storage for ONE LAYER's stage/eye.
//
// The two above are this with `layer_key = KLVK_EYE_LAYER`, and until 2026-08-15
// they were the only form — so every layer the guest set up was handed the eye
// layer's images. A Unity guest never noticed (its one other layer is a 1x1
// dummy nothing renders into); an Unreal guest draws its splash and its stereo
// UI quads into real ones, and they landed in the corner of the eye texture.
//
// Only the eye layer is published into kl_glfb's eye table, because that table
// is what every compositor samples. A size that differs from the cached one
// re-allocates rather than answering with storage of the wrong shape.
#define KLVK_EYE_LAYER (-1)
unsigned long long kl_vulkan_layer_image(int layer_key, int stage, int eye,
                                         unsigned w, unsigned h, int srgb, int layers);

// ...and the MTLTexture behind one of those, for a compositor that has to draw
// the layer. The eye layer is deliberately NOT reachable here: its textures go
// through kl_glfb's eye table, where every consumer already looks, and a second
// door onto the same storage is how two answers start to differ.
//
// A layer allocated for eye 0 alone — the common case, one texture with a
// per-eye ViewportRect selecting the part — answers eye 1 from it rather than
// with nothing. NULL when the layer has no storage for that stage.
void *kl_vulkan_layer_mtl_texture(int layer_key, int stage, int eye,
                                  int *w, int *h);

// Application SpaceWarp's motion-vector storage — `ovrp_GetLayerTextureSpaceWarp`.
//
// A title that ships with Application SpaceWarp enabled makes the Unity Oculus
// display provider allocate, per eye-swapchain stage, a MOTION VECTOR colour
// target and a depth target alongside the eye texture. Nothing on this host
// consumes them: the compositor reprojects from the eye layer and its own pose
// history. They exist because the provider treats a failure to create them as a
// failure to create the eye textures at all
// (OculusDisplayProvider::CreateMotionVectorTexture returns 1, which
// CreateEyeTextureStage propagates), and it then tears the whole display down.
//
// So this is storage the guest renders into and we never read, and the honest
// shape for it is the same as the eye images': cached per (stage, eye) so the
// handle a provider was given stays the handle it holds. `depth` picks the
// depth-stencil image rather than the colour one; the two live in separate
// slots and are asked for separately.
unsigned long long kl_vulkan_spacewarp_image(int stage, int eye,
                                             unsigned w, unsigned h,
                                             int layers, int depth);

// Write both eyes of a stage out as PNGs. Called at frame submission, which on
// this path is ovrp_EndFrame4 — the guest's own assertion that it has finished
// drawing them.
void kl_vulkan_capture_eyes(unsigned frame, int stage);

// "The guest has finished drawing this stage's eye textures." Called from the
// same place the capture is (ovrp_EndFrame4), and it is what a COMPOSITOR waits
// on: it makes the guest's queue idle and then advances a serial, so past that
// serial the picture in the eye MTLTexture is complete rather than merely
// submitted. `kl_vulkan_frame_serial` is the value — monotonic, 0 before the
// first frame, and exactly the role kl_glfb_gpu_fence_value plays on the GL
// path (where the wait is on the GPU instead). See the implementation for why
// this one is a CPU stall and what would remove it.
void kl_vulkan_frame_done(int stage);
unsigned long long kl_vulkan_frame_serial(void);
// A frontend whose eye pictures are already proven complete by a fence of its
// own (kl_openvr's async eye copy) says so here: kl_vulkan_frame_done then
// skips its queue drain — which on that path would stall the guest on its
// whole next frame of GPU work — and only advances the serial. 0 restores the
// drain. KL_VK_FRAME_SYNC=0 still turns the drain off for everyone.
void kl_vulkan_frame_sync_external(int on);

// The draw census's per-frame distribution over a WINDOW the caller closes —
// read and clear. Returns 1 when the numbers are real and 0 when the census is
// off (KL_VK_DRAW_CENSUS=0), in which case every output is zero and the caller
// must report it as UNAVAILABLE, not as "no draws". `frames` is the number of
// frame seams the window spanned; 0 means nothing closed inside it and the sum
// must not be divided by it.
//
// It exists for the perturbation test: "does a sub-degree change in the head
// pose change how many draws the guest records", which the run-long census
// cannot answer because both halves of the comparison live inside its mean.
int kl_vulkan_draw_window(unsigned long long *frames, unsigned long long *sum,
                          unsigned long long *min, unsigned long long *max);

// How many frames have been presented, and how many were written out. The
// end-of-run report reads these; a run with a healthy frame loop and zero
// captures is a different bug from one that never presented.
void  kl_vulkan_stats(unsigned *presented, unsigned *captured);

// One line naming every GPU byte Klepton holds, by kind, beside the whole
// process's Metal footprint. Printed once a second by the heartbeat and once
// inside the memory tripwire's dump.
//
// It exists because a vm_region walk cannot see GPU memory AT ALL: private
// Metal storage is charged to phys_footprint in full and mapped into the
// address space not at all, so the tripwire that fired on Paint the Town Red
// VR could account for 1.5 GiB of a 6153 MiB process and had nothing to say
// about the other four and a half. This is that half.
void  kl_vulkan_memory_report(FILE *f);

// Write the persisted VkPipelineCache now if anything was built since the last
// save (which the memory report throttles to KL_VK_PIPELINE_CACHE_SECS). The
// app calls it when the scene goes to the background. No-op without a cache.
void  kl_vulkan_pipeline_cache_flush(void);

// The GPU half of the memory budget, in bytes — what a memory HEAP is clamped
// to before the guest is allowed to read its size. Defined whether or not
// MoltenVK is vendored (it is arithmetic, not Vulkan), so a guard can assert it
// on a bare checkout. See the definition for the whole argument; the short one
// is that Unity reads the heap size straight into
// SystemInfo.graphicsMemorySize, and until round 11 that answered 15737 on a
// device this runtime had already told the guest was 6144.
//
// KL_VK_HEAP_MB overrides it; 0 or absent means "the CPU budget"
// (kl_mem_budget_bytes).
unsigned long long kl_vulkan_heap_budget_bytes(void);

// ---------------------------------------------------------------------------
// The FORMAT seam — what a guest may ask for, and what Metal actually has.
// ---------------------------------------------------------------------------
//
// Launch #11 died on the first punch with a Metal assertion, not a Vulkan
// error: `MTLTextureDescriptor has invalid pixelFormat (0)` inside
// MVKImagePlane::initSubresources, reached from the guest's own vkCreateImage.
// MoltenVK maps every VkFormat to an MTLPixelFormat and a handful of them have
// no Metal spelling at all on this device — the 24-bit R8G8B8 / B8G8R8 family
// most of all, which Adreno supports natively and every Quest build is
// therefore free to use. When the mapping comes back MTLPixelFormatInvalid,
// MoltenVK does not refuse: it builds a descriptor with pixelFormat 0 and
// Metal's validation layer aborts the process.
//
// Both halves live above the MoltenVK `__has_include` split for the same
// reason kl_vulkan_heap_budget_bytes does: they are tables, not Vulkan calls,
// and m_boot has to assert them on a checkout that has never run `make mvk`.
// Neither decides WHETHER a format is unsupported — that answer comes from
// MoltenVK itself at runtime (a format it cannot map reports zero features in
// all three masks). These say only what to reach for once it has.

// "R8G8B8_UNORM" for 23, and so on for every core VkFormat 0..184. Returns a
// static string; unknown values answer "VkFormat(N)" in a small ring of
// buffers, so a log line may hold a few at once.
const char *kl_vulkan_format_name(int fmt);

// The nearest Metal-expressible VkFormat for one Metal cannot express, or 0
// when there is no sensible replacement (including for every format that is
// FINE — a mappable format must never be rewritten, so this answers 0 for it).
//
// `layout_differs` is set to 1 when the replacement's texels are a different
// size or a different encoding, which is the whole difficulty: an RGB8 upload
// packs 3 bytes per texel and an RGBA8 image expects 4, so a
// vkCmdCopyBufferToImage recorded for the original would read past the end of
// the guest's staging buffer. It is set to 0 for the substitutions that are
// byte-safe in practice — the depth/stencil ones, which are attachments and
// are never the destination of a buffer copy.
int kl_vulkan_format_substitute(int fmt, int *layout_differs);

// VkImageUsageFlagBits, spelled here so a checkout with no vulkan.h can still
// assert the last-resort table. These values are frozen in the Vulkan 1.0 core
// and cannot change.
#define KL_VK_USAGE_TRANSFER_SRC                0x00000001u
#define KL_VK_USAGE_TRANSFER_DST                0x00000002u
#define KL_VK_USAGE_SAMPLED                     0x00000004u
#define KL_VK_USAGE_STORAGE                     0x00000008u
#define KL_VK_USAGE_COLOR_ATTACHMENT            0x00000010u
#define KL_VK_USAGE_DEPTH_STENCIL_ATTACHMENT    0x00000020u
#define KL_VK_USAGE_TRANSIENT_ATTACHMENT        0x00000040u
#define KL_VK_USAGE_INPUT_ATTACHMENT            0x00000080u

// What to reach for when a format has no FAMILY to widen inside — chiefly
// VK_FORMAT_UNDEFINED, which is what Unity's GraphicsFormat table produces
// when it has decided the format it wanted is unsupported. Chosen by USAGE,
// because usage is the only description of the image that still means
// something once the format does not: a depth-stencil attachment answers
// D32_SFLOAT_S8_UINT (byte-safe), anything sampled or drawn into answers
// R8G8B8A8_UNORM (texel size differs — nothing can be uploaded into it), and
// anything else answers 0 and is refused as before.
//
// Consulted only behind MoltenVK's own unmappable verdict, and only after
// kl_vulkan_format_substitute has answered 0.
int kl_vulkan_format_last_resort(int fmt, unsigned usage, int *layout_differs);

// ---------------------------------------------------------------------------
// The usage bit that costs a gigabyte — kl_vulkan_lazy_transient_usage()
// ---------------------------------------------------------------------------
//
// A transient attachment is written and consumed inside one render pass and
// never spilled, so on a tile GPU it costs ZERO bytes. Metal spells that
// MTLStorageModeMemoryless and MoltenVK exposes it as a LAZILY_ALLOCATED
// memory type. Launch #16 had one on this device (`memory type 2 ->
// DEVICE_LOCAL LAZILY_ALLOCATED`) and still put 1757 MiB of eye-size 2x MSAA
// depth and colour attachments on ordinary DEVICE_LOCAL memory, because
// MoltenVK never offered the type: it strips the lazily-allocated types unless
// the usage is EXACTLY transient-and-not-input. Read out of
// MVKImage::getMemoryRequirements in the vendored xrOS binary:
//
//   3db70: and  w9, w9, #0xc0      // usage & (TRANSIENT|INPUT_ATTACHMENT)
//   3db74: cmp  w9, #0x40          // ...must be TRANSIENT with INPUT clear
//   3db78: b.eq +0x10              // -> keep the lazily-allocated types
//   3db7c: ldr  w8, [x8, #0x1708]  // _lazilyAllocatedMemoryTypes
//   3db80: bic  w8, w10, w8        // -> otherwise take them away
//
// Unity's Vulkan backend sets VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT on every URP
// attachment it creates, whether or not any subpass ever declares one. So this
// returns the usage to hand MoltenVK: with the input bit dropped where doing so
// can only change which memory type is offered, and untouched everywhere else.
//
// It is dropped ONLY for an image that is transient AND has no usage that reads
// the texture outside the render pass. SAMPLED, STORAGE and either transfer bit
// all mean somebody expects the contents to survive, and memoryless storage
// would hand them a blank texture; those keep the bit and keep their memory.
unsigned kl_vulkan_lazy_transient_usage(unsigned usage);

// ---------------------------------------------------------------------------
// The OpenXR seam, for kl_openxr.c — XR_KHR_vulkan_enable
// ---------------------------------------------------------------------------
//
// Open Brush is the first guest at the junction of this project's two graphics
// halves: it speaks OPENXR (kl_openxr.c, which until now had only ever carried
// GLES guests) and its renderer is VULKAN (this file, which until now had only
// ever been reached through OVRPlugin). Neither half needed the other, so
// neither knew about it.
//
// The split below is the same one kl_openxr already keeps with kl_glfb: the XR
// file states WHAT it needs in OpenXR's vocabulary and this file answers it in
// Vulkan's, so kl_openxr.c includes no Vulkan header and stays linkable in a
// build with no MoltenVK vendored. Every Vulkan handle crossing this boundary is
// a `void *` because a dispatchable Vulkan handle IS a pointer on LP64 — the
// same measured property that makes the forwarding table upstairs possible.
//
// **Under XR_KHR_vulkan_enable the APP creates the VkInstance and VkDevice**,
// not the runtime (that inverts in `_enable2`), and it creates them through the
// synthetic loader in this file. So by the time the graphics binding arrives, a
// guest's device is already one klvk_CreateDevice interposed on — which is what
// makes the two extension lists below honestly empty.

// Is there a Vulkan device the OpenXR half could bind a session to? This gates
// whether XR_KHR_vulkan_enable is ADVERTISED at all: naming an extension we
// cannot back is the one thing the extension table upstairs refuses to do, and
// a checkout with no MoltenVK must fail the guest's xrCreateInstance rather
// than fail later, deeper, on a NULL entry point.
int kl_vulkan_xr_supported(void);

// The VkInstance / VkDevice extensions an app MUST enable for its Vulkan
// objects to be usable by this runtime, space-delimited as the spec wants them.
//
// Both are EMPTY, and that is a true answer rather than an unimplemented one:
// the only extension this runtime needs on the device is VK_EXT_metal_objects
// (how an eye VkImage's MTLTexture reaches the compositor), and klvk_CreateDevice
// already adds it to every device the guest creates. Requiring it of the app as
// well would be asking for something we have already taken.
const char *kl_vulkan_xr_instance_extensions(void);
const char *kl_vulkan_xr_device_extensions(void);

// The VkPhysicalDevice this runtime requires the app to render with, chosen
// from the app's OWN VkInstance (which is the handle the spec passes, and not
// necessarily one this file has seen). NULL when it cannot be answered.
void *kl_vulkan_xr_physical_device(void *vk_instance);

// The Vulkan API version range a session may be created against, as major/minor
// pairs. Answered from the physical device rather than asserted — MoltenVK's
// instance-level and device-level versions differ, and the one that governs an
// app's device is the device's.
void kl_vulkan_xr_api_range(unsigned *min_major, unsigned *min_minor,
                            unsigned *max_major, unsigned *max_minor);

// One OpenXR swapchain image, as a VkImage handle widened to 64 bits — which is
// exactly what XrSwapchainImageVulkanKHR carries. `vk_format` is a VkFormat, not
// a GL internal format: the two lists are different vocabularies for the same
// question and kl_openxr keeps one per graphics API. 0 on failure.
//
// Unlike kl_vulkan_eye_image this is not keyed on (stage, eye), because at
// xrCreateSwapchain time nothing knows which swapchain is an eye — see the
// implementation.
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned layers,
                                      unsigned mips, long long vk_format, int depth);

// ...and the MTLTexture behind one, for the compositor seam. NULL when it cannot
// be exported.
void *kl_vulkan_xr_image_mtl(unsigned long long image);

#endif
