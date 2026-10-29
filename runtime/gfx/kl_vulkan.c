// libvulkan.so over MoltenVK — see kl_vulkan.h for what this is and why.
//
// The real Vulkan headers are used rather than transcribed, which is the
// opposite of kl_openxr's choice next door, and the reason is that the headers
// are already a build dependency here: MoltenVK ships them in the same tarball
// as the dylib, and there is no Vulkan path at all without that dylib. So
// transcribing would add a whole class of silent layout bug (a 182-entry-point
// API, structs up to ~100 fields) to buy exactly nothing.
//
// The __has_include guard keeps `make check` honest on a bare checkout: with no
// MoltenVK vendored this file still compiles, claims nothing, and says so BY
// NAME the moment a guest asks for Vulkan. That is the same shape as ANGLE not
// being built — a missing optional dependency should read as a missing
// dependency, not as a guest that mysteriously fails to render.
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kl_vulkan.h"
#include "kl_env.h"
#include "kl_perf.h"
#include "kl_fove.h"
#include "kl_shc.h"
#include "kl_openvr.h"

// Matched on with a prefix test rather than equality: the guest asks by plain
// soname, but a path can arrive too (kl_dl.c hands through whatever the guest
// wrote, and BONELAB's plugin loader builds absolute paths).
static int name_is_vulkan(const char *p) {
    if (!p) return 0;
    const char *slash = strrchr(p, '/');
    const char *base = slash ? slash + 1 : p;
    return !strcmp(base, "libvulkan.so") || !strcmp(base, "libvulkan.so.1") ||
           !strcmp(base, "vulkan");
}

// The GPU half of the memory budget. Defined ABOVE the MoltenVK split because
// it is arithmetic over two environment reads and has nothing to do with
// whether Vulkan is present — and because m_boot's guard has to be able to
// assert it on a checkout that has never run `make mvk`.
//
// The full argument is at the clamp that spends it (klvk_clamp_heaps). The
// short version: runtime/libc/kl_libc.c already tells the guest it is on a
// 6144 MiB machine and fires Application.lowMemory against that number, while
// MoltenVK reports its DEVICE_LOCAL heap as the whole 16 GB device — so Unity
// was reading SystemInfo.systemMemorySize = 6144 and
// SystemInfo.graphicsMemorySize = 15737 in the same breath, on a unified-memory
// device where those are the same pages.
//
// KL_VK_HEAP_MB overrides; 0 (the default) means "use the CPU budget", and a
// negative or absent value is the same as 0.
uint64_t kl_mem_budget_bytes(void);

unsigned long long kl_vulkan_heap_budget_bytes(void) {
    long mb = kl_env_int("KL_VK_HEAP_MB", 0);
    if (mb > 0) return (unsigned long long)mb << 20;
    unsigned long long b = kl_mem_budget_bytes();
    return b ? b : ((unsigned long long)6144 << 20);
}

// ---------------------------------------------------------------------------
// The FORMAT seam — shared half. See kl_vulkan.h for what this is and why it
// sits above the MoltenVK split.
// ---------------------------------------------------------------------------
//
// Launch #11 reached the BikerBar level, rendered it, and died on the first
// punch inside MoltenVK:
//
//   -[MTLTextureDescriptorInternal validateWithDevice:]:1421: failed assertion
//   `MTLTextureDescriptor has invalid pixelFormat (0).'
//     MVKImagePlane::initSubresources <- MVKImage::MVKImage
//       <- MVKDevice::createImage <- vkCreateImage <- libunity.so+0xc213bc
//
// The call site was read rather than guessed (round 9's lesson). libunity.so
// 0xc21050 builds a VkImageCreateInfo at sp+0xa0 with the format in w22, and
// hangs a VkImageFormatListCreateInfo (sType 0x3b9d0838 = 1000147000) off
// pNext holding the sRGB/linear pair — the switch at 0xc211a0 maps 15..57 onto
// {9,16,23,30,37,44,51}, i.e. every 8-bit sRGB format onto its linear
// counterpart. Two of those counterparts are R8G8B8_UNORM (23) and
// B8G8R8_UNORM (30): Unity's own table treats 24-bit RGB as a live texture
// format on this path, because on Adreno it is one. Metal has no 24-bit colour
// format at all, so MoltenVK maps it to MTLPixelFormatInvalid — and then
// builds the descriptor anyway.
//
// Nothing here decides that a format is unsupported. MoltenVK's own
// vkGetPhysicalDeviceFormatProperties does, at runtime, by answering zero in
// all three feature masks — which is exactly and only what an unmapped format
// produces. These two functions say what to reach for once it has, and they
// are pure tables so m_boot can assert them with no MoltenVK vendored.

static const char *const k_vk_format_name[] = {
    "UNDEFINED", "R4G4_UNORM_PACK8", "R4G4B4A4_UNORM_PACK16",
    "B4G4R4A4_UNORM_PACK16", "R5G6B5_UNORM_PACK16", "B5G6R5_UNORM_PACK16",
    "R5G5B5A1_UNORM_PACK16", "B5G5R5A1_UNORM_PACK16",
    "A1R5G5B5_UNORM_PACK16", "R8_UNORM", "R8_SNORM", "R8_USCALED",
    "R8_SSCALED", "R8_UINT", "R8_SINT", "R8_SRGB", "R8G8_UNORM",
    "R8G8_SNORM", "R8G8_USCALED", "R8G8_SSCALED", "R8G8_UINT", "R8G8_SINT",
    "R8G8_SRGB", "R8G8B8_UNORM", "R8G8B8_SNORM", "R8G8B8_USCALED",
    "R8G8B8_SSCALED", "R8G8B8_UINT", "R8G8B8_SINT", "R8G8B8_SRGB",
    "B8G8R8_UNORM", "B8G8R8_SNORM", "B8G8R8_USCALED", "B8G8R8_SSCALED",
    "B8G8R8_UINT", "B8G8R8_SINT", "B8G8R8_SRGB", "R8G8B8A8_UNORM",
    "R8G8B8A8_SNORM", "R8G8B8A8_USCALED", "R8G8B8A8_SSCALED",
    "R8G8B8A8_UINT", "R8G8B8A8_SINT", "R8G8B8A8_SRGB", "B8G8R8A8_UNORM",
    "B8G8R8A8_SNORM", "B8G8R8A8_USCALED", "B8G8R8A8_SSCALED",
    "B8G8R8A8_UINT", "B8G8R8A8_SINT", "B8G8R8A8_SRGB",
    "A8B8G8R8_UNORM_PACK32", "A8B8G8R8_SNORM_PACK32",
    "A8B8G8R8_USCALED_PACK32", "A8B8G8R8_SSCALED_PACK32",
    "A8B8G8R8_UINT_PACK32", "A8B8G8R8_SINT_PACK32", "A8B8G8R8_SRGB_PACK32",
    "A2R10G10B10_UNORM_PACK32", "A2R10G10B10_SNORM_PACK32",
    "A2R10G10B10_USCALED_PACK32", "A2R10G10B10_SSCALED_PACK32",
    "A2R10G10B10_UINT_PACK32", "A2R10G10B10_SINT_PACK32",
    "A2B10G10R10_UNORM_PACK32", "A2B10G10R10_SNORM_PACK32",
    "A2B10G10R10_USCALED_PACK32", "A2B10G10R10_SSCALED_PACK32",
    "A2B10G10R10_UINT_PACK32", "A2B10G10R10_SINT_PACK32", "R16_UNORM",
    "R16_SNORM", "R16_USCALED", "R16_SSCALED", "R16_UINT", "R16_SINT",
    "R16_SFLOAT", "R16G16_UNORM", "R16G16_SNORM", "R16G16_USCALED",
    "R16G16_SSCALED", "R16G16_UINT", "R16G16_SINT", "R16G16_SFLOAT",
    "R16G16B16_UNORM", "R16G16B16_SNORM", "R16G16B16_USCALED",
    "R16G16B16_SSCALED", "R16G16B16_UINT", "R16G16B16_SINT",
    "R16G16B16_SFLOAT", "R16G16B16A16_UNORM", "R16G16B16A16_SNORM",
    "R16G16B16A16_USCALED", "R16G16B16A16_SSCALED", "R16G16B16A16_UINT",
    "R16G16B16A16_SINT", "R16G16B16A16_SFLOAT", "R32_UINT", "R32_SINT",
    "R32_SFLOAT", "R32G32_UINT", "R32G32_SINT", "R32G32_SFLOAT",
    "R32G32B32_UINT", "R32G32B32_SINT", "R32G32B32_SFLOAT",
    "R32G32B32A32_UINT", "R32G32B32A32_SINT", "R32G32B32A32_SFLOAT",
    "R64_UINT", "R64_SINT", "R64_SFLOAT", "R64G64_UINT", "R64G64_SINT",
    "R64G64_SFLOAT", "R64G64B64_UINT", "R64G64B64_SINT", "R64G64B64_SFLOAT",
    "R64G64B64A64_UINT", "R64G64B64A64_SINT", "R64G64B64A64_SFLOAT",
    "B10G11R11_UFLOAT_PACK32", "E5B9G9R9_UFLOAT_PACK32", "D16_UNORM",
    "X8_D24_UNORM_PACK32", "D32_SFLOAT", "S8_UINT", "D16_UNORM_S8_UINT",
    "D24_UNORM_S8_UINT", "D32_SFLOAT_S8_UINT", "BC1_RGB_UNORM_BLOCK",
    "BC1_RGB_SRGB_BLOCK", "BC1_RGBA_UNORM_BLOCK", "BC1_RGBA_SRGB_BLOCK",
    "BC2_UNORM_BLOCK", "BC2_SRGB_BLOCK", "BC3_UNORM_BLOCK",
    "BC3_SRGB_BLOCK", "BC4_UNORM_BLOCK", "BC4_SNORM_BLOCK",
    "BC5_UNORM_BLOCK", "BC5_SNORM_BLOCK", "BC6H_UFLOAT_BLOCK",
    "BC6H_SFLOAT_BLOCK", "BC7_UNORM_BLOCK", "BC7_SRGB_BLOCK",
    "ETC2_R8G8B8_UNORM_BLOCK", "ETC2_R8G8B8_SRGB_BLOCK",
    "ETC2_R8G8B8A1_UNORM_BLOCK", "ETC2_R8G8B8A1_SRGB_BLOCK",
    "ETC2_R8G8B8A8_UNORM_BLOCK", "ETC2_R8G8B8A8_SRGB_BLOCK",
    "EAC_R11_UNORM_BLOCK", "EAC_R11_SNORM_BLOCK", "EAC_R11G11_UNORM_BLOCK",
    "EAC_R11G11_SNORM_BLOCK", "ASTC_4x4_UNORM_BLOCK", "ASTC_4x4_SRGB_BLOCK",
    "ASTC_5x4_UNORM_BLOCK", "ASTC_5x4_SRGB_BLOCK", "ASTC_5x5_UNORM_BLOCK",
    "ASTC_5x5_SRGB_BLOCK", "ASTC_6x5_UNORM_BLOCK", "ASTC_6x5_SRGB_BLOCK",
    "ASTC_6x6_UNORM_BLOCK", "ASTC_6x6_SRGB_BLOCK", "ASTC_8x5_UNORM_BLOCK",
    "ASTC_8x5_SRGB_BLOCK", "ASTC_8x6_UNORM_BLOCK", "ASTC_8x6_SRGB_BLOCK",
    "ASTC_8x8_UNORM_BLOCK", "ASTC_8x8_SRGB_BLOCK", "ASTC_10x5_UNORM_BLOCK",
    "ASTC_10x5_SRGB_BLOCK", "ASTC_10x6_UNORM_BLOCK", "ASTC_10x6_SRGB_BLOCK",
    "ASTC_10x8_UNORM_BLOCK", "ASTC_10x8_SRGB_BLOCK",
    "ASTC_10x10_UNORM_BLOCK", "ASTC_10x10_SRGB_BLOCK",
    "ASTC_12x10_UNORM_BLOCK", "ASTC_12x10_SRGB_BLOCK",
    "ASTC_12x12_UNORM_BLOCK", "ASTC_12x12_SRGB_BLOCK",
};

const char *kl_vulkan_format_name(int fmt) {
    if (fmt >= 0 && fmt < (int)(sizeof k_vk_format_name / sizeof *k_vk_format_name))
        return k_vk_format_name[fmt];
    // Extension formats (the 1000xxxxxx range) and anything else: a small ring
    // so one fprintf can name two of them.
    static char ring[4][32];
    static int next;
    char *b = ring[next++ & 3];
    snprintf(b, sizeof ring[0], "VkFormat(%d)", fmt);
    return b;
}

// True for the compressed blocks that carry an sRGB transfer function. Read off
// the name rather than kept as a second list: a substitution that quietly drops
// sRGB washes every albedo texture out by a gamma, which is the kind of defect
// that gets blamed on lighting for three rounds.
static int vk_format_is_srgb(int fmt) {
    const char *n = kl_vulkan_format_name(fmt);
    return strstr(n, "_SRGB") != NULL;
}

int kl_vulkan_format_substitute(int fmt, int *layout_differs) {
    int sub = 0;
    int differs = 1;   // the default: a different texel size or encoding

    // The three-component families. Metal has no 24-, 48- or 96-bit colour
    // format, so each widens to its four-component sibling — which is
    // deliberately the SAME family and the same transfer function, so
    // R8G8B8_SRGB lands on R8G8B8A8_SRGB and not on a linear format.
    if      (fmt >=  23 && fmt <=  29) sub = fmt + 14;  // R8G8B8_*    -> R8G8B8A8_*
    else if (fmt >=  30 && fmt <=  36) sub = fmt + 14;  // B8G8R8_*    -> B8G8R8A8_*
    else if (fmt >=  84 && fmt <=  90) sub = fmt +  7;  // R16G16B16_* -> R16G16B16A16_*
    else if (fmt >= 104 && fmt <= 106) sub = fmt +  3;  // R32G32B32_* -> R32G32B32A32_*
    else if (fmt >= 116 && fmt <= 118) sub = fmt +  3;  // R64G64B64_* -> R64G64B64A64_*
    // The small packed formats, none of which survive everywhere. RGBA8 is the
    // one format every Metal device has.
    else if (fmt >=   1 && fmt <=   8) sub = 37;        // R4G4 .. A1R5G5B5
    // Depth and stencil. These are the byte-SAFE substitutions: a depth
    // attachment is never the destination of a vkCmdCopyBufferToImage, so
    // widening it costs memory and nothing else. D24_UNORM_S8_UINT is the one
    // every Android title asks for and no Apple GPU has.
    else if (fmt == 124)               { sub = 126; differs = 0; }  // D16 -> D32_SFLOAT
    else if (fmt == 125)               { sub = 126; differs = 0; }  // X8_D24 -> D32_SFLOAT
    else if (fmt == 127)               { sub = 130; differs = 0; }  // S8_UINT -> D32_SFLOAT_S8
    else if (fmt == 128 || fmt == 129) { sub = 130; differs = 0; }  // *_S8 -> D32_SFLOAT_S8
    // Block-compressed. If a device cannot decode the block there is no
    // compressed format to fall back to, so this lands on uncompressed RGBA8 —
    // which keeps the image VALID (extents are in texels either way) but means
    // the guest's block data cannot be uploaded into it. That is exactly what
    // layout_differs is for.
    else if (fmt >= 131 && fmt <= 184) sub = vk_format_is_srgb(fmt) ? 43 : 37;

    if (layout_differs) *layout_differs = sub ? differs : 0;
    return sub;
}

// The last resort, and the reason launch #12 died rather than merely looked
// wrong. `kl_vulkan_format_substitute` answers within a FAMILY, and a family is
// something a format has to be a member of — VK_FORMAT_UNDEFINED is not.
//
// Launch #12's punch produced a full-eye 2-layer transient attachment whose
// format was 0. That is Unity's GraphicsFormat table answering "none", not a
// format Metal happens to lack, and there is no sibling to widen to. Round 12
// refused it with VK_ERROR_FORMAT_NOT_SUPPORTED — legal, and the guest still
// dereferenced the NULL image four instructions later (libunity.so+0xbf19ec,
// x21 = x22 = 0). A legal error the caller does not check is a crash with
// better paperwork.
//
// So: when there is no family answer, pick by what the image is FOR. Usage is
// the only description of an attachment that survives the format being
// meaningless, and it is enough — a depth-stencil attachment gets the one
// depth format every Apple GPU has, and anything sampled or drawn into gets
// RGBA8. An image that is neither still answers 0 and is still refused, because
// a plain transfer image with no format is not something a substitute can save.
int kl_vulkan_format_last_resort(int fmt, unsigned usage, int *layout_differs) {
    int sub = 0, differs = 1;

    // Only ever consulted where the family table had nothing. If it has an
    // answer, that answer is better than this one by construction.
    if (kl_vulkan_format_substitute(fmt, NULL)) {
        if (layout_differs) *layout_differs = 0;
        return 0;
    }
    if (usage & KL_VK_USAGE_DEPTH_STENCIL_ATTACHMENT) {
        sub = 130;      // D32_SFLOAT_S8_UINT — depth AND stencil, always present
        differs = 0;    // ...and an attachment is never a buffer copy's target
    } else if (usage & (KL_VK_USAGE_COLOR_ATTACHMENT | KL_VK_USAGE_INPUT_ATTACHMENT
                        | KL_VK_USAGE_STORAGE | KL_VK_USAGE_SAMPLED)) {
        sub = 37;       // R8G8B8A8_UNORM
        differs = 1;    // ...and nothing can be uploaded into it meaningfully
    }
    if (sub == fmt) sub = 0;
    if (layout_differs) *layout_differs = sub ? differs : 0;
    return sub;
}

// The input-attachment bit MoltenVK reads as "this one cannot be memoryless".
// The whole argument, with the disassembly it was read out of, is in the
// header; the rule is one line and the exclusions are the entire safety case.
unsigned kl_vulkan_lazy_transient_usage(unsigned usage) {
    // Not an attachment the guest promised to consume in-pass: nothing to do.
    if (!(usage & KL_VK_USAGE_TRANSIENT_ATTACHMENT)) return usage;
    if (!(usage & KL_VK_USAGE_INPUT_ATTACHMENT))     return usage;
    // ...and somebody reads it outside the pass, so its contents have to exist.
    // A transient image is already forbidden from being a transfer source or
    // destination, so those two bits are a contradiction rather than a case —
    // but a contradiction is exactly when not to start deleting usage bits.
    if (usage & (KL_VK_USAGE_SAMPLED | KL_VK_USAGE_STORAGE
                 | KL_VK_USAGE_TRANSFER_SRC | KL_VK_USAGE_TRANSFER_DST))
        return usage;
    return usage & ~KL_VK_USAGE_INPUT_ATTACHMENT;
}

#if !__has_include(<vulkan/vulkan.h>)

// ---------------------------------------------------------------------------
// No MoltenVK vendored. Refuse by name, once, and let the guest take whatever
// path it takes when Vulkan is absent — which for BONELAB is the hardware
// requirements warning, i.e. exactly the pre-existing behaviour.
// ---------------------------------------------------------------------------
static void complain_once(void) {
    static int said;
    if (said++) return;
    fprintf(stderr,
            "  [vk] libvulkan.so requested, but MoltenVK is not vendored.\n"
            "  [vk] Run 'make mvk' (see BUILDING.md); this build has no Vulkan.\n");
}
int   kl_vulkan_claims(const char *n) { if (name_is_vulkan(n)) complain_once(); return 0; }
void *kl_vulkan_dlopen(const char *n) { if (name_is_vulkan(n)) complain_once(); return NULL; }
int   kl_vulkan_is_handle(const void *h) { (void)h; return 0; }
void *kl_vulkan_sym(const char *n) { (void)n; return NULL; }
void *kl_vulkan_lookup(const char *n) { (void)n; return NULL; }
int   kl_vulkan_available(void) { return 0; }
void  kl_vulkan_set_linux_wsi(int on) { (void)on; }
void  kl_vulkan_stats(unsigned *p, unsigned *c) { if (p) *p = 0; if (c) *c = 0; }
int   kl_vulkan_guest_active(void) { return 0; }
unsigned long long kl_vulkan_eye_image(int s, int e, unsigned w, unsigned h, int srgb) {
    (void)s; (void)e; (void)w; (void)h; (void)srgb; return 0;
}
// Was missing, and it is what kl_ovrp.c actually calls — so a checkout that has
// never run `make mvk` did not fail to *compile*, it failed to LINK, which is
// the one failure this whole `#if` exists to prevent. Every entry point in the
// header belongs here.
unsigned long long kl_vulkan_eye_image_layers(int s, int e, unsigned w, unsigned h,
                                             int srgb, int layers) {
    (void)s; (void)e; (void)w; (void)h; (void)srgb; (void)layers; return 0;
}
unsigned long long kl_vulkan_layer_image(int k, int s, int e, unsigned w, unsigned h,
                                         int srgb, int layers) {
    (void)k; (void)s; (void)e; (void)w; (void)h; (void)srgb; (void)layers; return 0;
}
void *kl_vulkan_layer_mtl_texture(int k, int s, int e, int *w, int *h) {
    (void)k; (void)s; (void)e; if (w) *w = 0; if (h) *h = 0; return NULL;
}
unsigned long long kl_vulkan_spacewarp_image(int s, int e, unsigned w, unsigned h,
                                             int layers, int depth) {
    (void)s; (void)e; (void)w; (void)h; (void)layers; (void)depth; return 0;
}
void  kl_vulkan_capture_eyes(unsigned f, int s) { (void)f; (void)s; }
void  kl_vulkan_frame_done(int s) { (void)s; }
void  kl_vulkan_frame_sync_external(int on) { (void)on; }
unsigned long long kl_vulkan_mtl_allocated_bytes(void) { return 0; }
int   kl_vulkan_draw_window(unsigned long long *frames, unsigned long long *sum,
                            unsigned long long *min, unsigned long long *max) {
    if (frames) *frames = 0; if (sum) *sum = 0;
    if (min)    *min    = 0; if (max) *max = 0;
    return 0;
}
void  kl_vulkan_memory_report(FILE *f) { (void)f; }
void  kl_vulkan_pipeline_cache_flush(void) {}
unsigned long long kl_vulkan_frame_serial(void) { return 0; }
// The OpenXR seam. Answering "not supported" here is what keeps
// XR_KHR_vulkan_enable OUT of the advertised extension list on a checkout with
// no MoltenVK, so a Vulkan OpenXR guest is refused at xrCreateInstance — where
// the guest's own log names the extension — instead of somewhere further in.
int   kl_vulkan_xr_supported(void) { return 0; }
const char *kl_vulkan_xr_instance_extensions(void) { return ""; }
const char *kl_vulkan_xr_device_extensions(void) { return ""; }
void *kl_vulkan_xr_physical_device(void *vi) { (void)vi; return NULL; }
void  kl_vulkan_xr_api_range(unsigned *mmaj, unsigned *mmin,
                             unsigned *xmaj, unsigned *xmin) {
    if (mmaj) *mmaj = 0; if (mmin) *mmin = 0;
    if (xmaj) *xmaj = 0; if (xmin) *xmin = 0;
}
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned l,
                                      unsigned m, long long f, int depth) {
    (void)w; (void)h; (void)l; (void)m; (void)f; (void)depth; return 0;
}
void *kl_vulkan_xr_image_mtl(unsigned long long img) { (void)img; return NULL; }
int   kl_vulkan_feature_audit(void *p, const void *ci, const char *who) {
    (void)p; (void)ci; (void)who; return 0;
}
void  kl_vulkan_xr_bind_device(void *d) { (void)d; }

#else  /* MoltenVK headers present */

#define VK_NO_PROTOTYPES
// The Android WSI half of the header, which is what `vkCreateAndroidSurfaceKHR`
// and its create-info live in. Safe to switch on with no Android SDK anywhere:
// vulkan_android.h forward-declares `struct ANativeWindow` and `struct
// AHardwareBuffer` itself and includes nothing. Transcribing the struct instead
// would have been four fields of avoidable risk.
#define VK_USE_PLATFORM_ANDROID_KHR 1
// ...and the Metal half, for `VK_EXT_metal_objects` — how the MTLTexture behind
// an eye VkImage is reached and handed to the compositor. Equally safe in plain
// C: vulkan_metal.h typedefs every Metal handle to `void *` unless __OBJC__ is
// defined, which it is not here.
#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>
#include "kl_glfb.h"
#include "kl_vulkan_features.h"
#include <MoltenVK/mvk_private_api.h>   // MVKConfiguration, read back for the log

#include <zlib.h>

#define KLVK_MAX_IMAGES 8

static int vk_trace(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_TRACE", 0);
    return on;
}
#define VKT(...) do { if (vk_trace()) fprintf(stderr, "  [vk] " __VA_ARGS__); } while (0)
#define VKI(...) fprintf(stderr, "  [vk] " __VA_ARGS__)

// ---------------------------------------------------------------------------
// Loading MoltenVK
// ---------------------------------------------------------------------------
//
// Two shapes, for the same reason ANGLE has two (kl_glfb.c's angle_dlopen): a
// bare dylib is what the host stages, and an app may only load code from inside
// its own bundle on visionOS, where it is a .framework. Try both rather than
// making the caller know which.
#define MVK_VENDORED_DIR "vendor-moltenvk/out/macos"

static void *mvk_try(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) VKT("dlopen(%s): %s\n", path, dlerror());
    return h;
}

static void *mvk_open(void) {
    const char *explicit = kl_env_str("KL_MVK_DYLIB", NULL);
    if (explicit) return mvk_try(explicit);

    const char *dir = kl_env_str("KL_MVK_DIR", MVK_VENDORED_DIR);
    char path[1024];
    snprintf(path, sizeof path, "%s/libMoltenVK.dylib", dir);
    void *h = mvk_try(path);
    if (h) return h;
    snprintf(path, sizeof path, "%s/MoltenVK.framework/MoltenVK", dir);
    if ((h = mvk_try(path))) return h;
    // The bundle case: @rpath resolves to <exec dir>/Frameworks, which is where
    // an embedded framework lands and the ONLY place AMFI will accept code from.
    if ((h = mvk_try("@rpath/MoltenVK.framework/MoltenVK"))) return h;
    return mvk_try("MoltenVK.framework/MoltenVK");
}

static void *g_mvk;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr   real_gdpa;
static int g_init_done, g_avail;

static void *mvk_sym(const char *n) { return g_mvk ? dlsym(g_mvk, n) : NULL; }

static void vk_init(void) {
    if (g_init_done) return;
    g_init_done = 1;

    // MoltenVK logs its whole device census at info level, which buries
    // everything this port prints. Same choice as tests/t_mvk.c, same escape.
    if (!kl_env_on("KL_MVK_VERBOSE", 0)) setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);

    g_mvk = mvk_open();
    if (!g_mvk) {
        VKI("MoltenVK could not be loaded — run 'make mvk' (BUILDING.md).\n");
        return;
    }
    real_gipa = (PFN_vkGetInstanceProcAddr)mvk_sym("vkGetInstanceProcAddr");
    real_gdpa = (PFN_vkGetDeviceProcAddr)mvk_sym("vkGetDeviceProcAddr");
    if (!real_gipa) { VKI("MoltenVK has no vkGetInstanceProcAddr\n"); return; }
    g_avail = 1;

    PFN_vkEnumerateInstanceVersion eiv =
        (PFN_vkEnumerateInstanceVersion)real_gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t ver = VK_API_VERSION_1_0;
    if (eiv) eiv(&ver);
    VKI("MoltenVK loaded — instance API %u.%u.%u\n", VK_VERSION_MAJOR(ver),
        VK_VERSION_MINOR(ver), VK_VERSION_PATCH(ver));
}

int kl_vulkan_available(void) { vk_init(); return g_avail; }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t magic;
    uint32_t w, h;
} klvk_surface;
#define KLVK_SURF_MAGIC 0x4b565355u   /* 'KVSU' */

// Everything a capture needs, resolved once per device. Held rather than
// re-resolved because vkQueuePresentKHR is on the frame path.
typedef struct {
    VkDevice          dev;
    VkPhysicalDevice  phys;
    uint32_t          queue_family;
    VkQueue           queue;          // for the acquire signal + the capture submit
    PFN_vkGetDeviceQueue              GetDeviceQueue;
    PFN_vkCreateImage                 CreateImage;
    PFN_vkDestroyImage                DestroyImage;
    PFN_vkGetImageMemoryRequirements  GetImageMemoryRequirements;
    PFN_vkAllocateMemory              AllocateMemory;
    PFN_vkFreeMemory                  FreeMemory;
    PFN_vkBindImageMemory             BindImageMemory;
    PFN_vkCreateBuffer                CreateBuffer;
    PFN_vkDestroyBuffer               DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory            BindBufferMemory;
    PFN_vkMapMemory                   MapMemory;
    PFN_vkUnmapMemory                 UnmapMemory;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
    PFN_vkCreateCommandPool           CreateCommandPool;
    PFN_vkDestroyCommandPool          DestroyCommandPool;
    PFN_vkAllocateCommandBuffers      AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer          BeginCommandBuffer;
    PFN_vkEndCommandBuffer            EndCommandBuffer;
    PFN_vkResetCommandBuffer          ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier          CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer        CmdCopyImageToBuffer;
    PFN_vkCmdClearColorImage          CmdClearColorImage;
    PFN_vkQueueSubmit                 QueueSubmit;
    PFN_vkCreateFence                 CreateFence;
    PFN_vkDestroyFence                DestroyFence;
    PFN_vkWaitForFences               WaitForFences;
    PFN_vkResetFences                 ResetFences;
    PFN_vkDeviceWaitIdle              DeviceWaitIdle;
    PFN_vkQueueWaitIdle               QueueWaitIdle;
    // Readback scratch, created on first use and shared by every capture path
    // (swapchain present and the OVRPlugin eye layer). Sized to the largest
    // image captured so far and grown as needed.
    VkBuffer        cap_buf;
    VkDeviceMemory  cap_mem;
    VkDeviceSize    cap_size;
    VkCommandPool   cap_pool;
    VkCommandBuffer cap_cmd;
    VkFence         cap_fence;
} klvk_device;

typedef struct {
    uint32_t       magic;
    klvk_device   *d;
    VkFormat       format;
    VkExtent2D     extent;
    uint32_t       n;
    VkImage        images[KLVK_MAX_IMAGES];
    VkDeviceMemory mem[KLVK_MAX_IMAGES];
    uint64_t       bytes[KLVK_MAX_IMAGES];   // what each one charged; see the ledger
    uint32_t       next;
} klvk_swapchain;
#define KLVK_SWAP_MAGIC 0x4b565343u   /* 'KVSC' */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static klvk_device *g_devs[8];
static int          g_ndev;
static VkInstance   g_instance;
static unsigned     g_presented, g_captured;
// Defined here rather than beside kl_vulkan_frame_serial because the image
// graveyard (below) stamps retirements with it and is written above that point.
static uint64_t     g_frame_serial;

// The image ledger lives further down, beside the graveyard that spends it, but
// the swapchain path above needs to charge to it.
static void ledger_add(int kind, uint64_t bytes);
static void ledger_sub(int kind, uint64_t bytes);
enum { KLVK_LEDGER_EYE, KLVK_LEDGER_SPACEWARP, KLVK_LEDGER_LAYER,
       KLVK_LEDGER_SWAP, KLVK_LEDGER_XR, KLVK_LEDGER_SCRATCH, KLVK_LEDGER_N };

void kl_vulkan_stats(unsigned *p, unsigned *c) {
    if (p) *p = g_presented;
    if (c) *c = g_captured;
}

static klvk_device *dev_find(VkDevice d) {
    for (int i = 0; i < g_ndev; i++) if (g_devs[i]->dev == d) return g_devs[i];
    return NULL;
}

// The device OpenXR swapchain images are made on: the session's (bound by
// kl_openxr's xrCreateSession), else the first device, which is every
// single-device guest. The Alyx host makes TWO — its own at nativeStart, then
// Valve's renderer device with a second session — and the eye copy is a
// same-device blit into the swapchain image, so the images belong to the
// session that will present them, not to whichever device came first.
static VkDevice g_xr_dev;
void kl_vulkan_xr_bind_device(void *vk_device) {
    g_xr_dev = (VkDevice)vk_device;
    pthread_mutex_lock(&g_lock);
    klvk_device *d = dev_find(g_xr_dev);
    int idx = -1;
    for (int i = 0; i < g_ndev; i++) if (g_devs[i] == d) idx = i;
    pthread_mutex_unlock(&g_lock);
    VKI("xr: the session's device %p is %s (%d device(s) live)\n", vk_device,
        d ? (idx == 0 ? "the first device" : "NOT the first device — swapchain images "
                        "follow the session") : "UNKNOWN to this layer", g_ndev);
}
static klvk_device *xr_dev(void) {
    klvk_device *d = g_xr_dev ? dev_find(g_xr_dev) : NULL;
    return d ? d : (g_ndev ? g_devs[0] : NULL);
}
// The queue census lives beside the occlusion-query one, far below; vkCreateDevice
// needs to register the handle it fetches. See it for what it does and does not
// claim.
static void qrec_note_fetch(VkQueue q, uint32_t family, uint32_t index);
// What the device advertised, captured at vkCreateDevice so the census can say
// whether a family is a graphics family without re-querying on the frame path.
static VkQueueFamilyProperties g_qfam[8];
static uint32_t g_nqfam;

static klvk_device *dev_of_queue(VkQueue q) {
    // Queues are opaque; the guest may use one we never fetched. With a single
    // device — which is every case here — the answer is unambiguous.
    (void)q;
    return g_ndev ? g_devs[0] : NULL;
}

// ---------------------------------------------------------------------------
// PNG (top-down; Vulkan images already are). kl_glfb.c has its own writer and
// it is NOT shared, deliberately: that one flips GL's bottom-up rows and is
// entangled with the exposure/gamma tonemap the GL capture applies. Sharing
// would mean either a flag that is wrong half the time or surgery on a
// 5000-line file on the critical rendering path.
// ---------------------------------------------------------------------------
static void be32w(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void png_chunk(FILE *f, const char *tag, const uint8_t *d, uint32_t n) {
    uint8_t h[4];
    be32w(h, n); fwrite(h, 1, 4, f); fwrite(tag, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)tag, 4);
    if (n) c = crc32(c, (const Bytef *)d, n);
    be32w(h, (uint32_t)c); fwrite(h, 1, 4, f);
}

static int png_write_rgba(const char *path, const uint8_t *px, int w, int h) {
    size_t stride = (size_t)w * 4, raw_n = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    uLongf cn = compressBound((uLong)raw_n);
    uint8_t *cb = malloc(cn);
    if (!raw || !cb) { free(raw); free(cb); return 0; }
    for (int y = 0; y < h; y++) {
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1, px + stride * (size_t)y, stride);
    }
    int ok = compress(cb, &cn, raw, (uLong)raw_n) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
        fwrite(sig, 1, 8, f);
        uint8_t ih[13];
        be32w(ih, (uint32_t)w); be32w(ih + 4, (uint32_t)h);
        ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
        png_chunk(f, "IHDR", ih, 13);
        png_chunk(f, "IDAT", cb, (uint32_t)cn);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    } else ok = 0;
    free(raw); free(cb);
    return ok;
}

// Is this format's byte order BGRA rather than RGBA? The swapchain format is
// whatever the guest picked from what we advertised, and a channel swap is
// invisible in a grey test frame and glaring in a real one.
static int fmt_is_bgra(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

// ---------------------------------------------------------------------------
// Extension list surgery
// ---------------------------------------------------------------------------
//
// Every requested extension is checked against what MoltenVK actually
// advertises and silently-unsupported ones are DROPPED BY NAME rather than
// passed through. Passing them through fails the whole create call with
// VK_ERROR_EXTENSION_NOT_PRESENT, which tells the guest "no Vulkan" and tells
// us nothing about which of a dozen names was the problem.
//
// This is the same judgement the rest of the port makes about group answers:
// answering call-by-call would let the guest see a set that disagrees with
// itself.
typedef struct { const char *names[64]; uint32_t n; } extlist;

static void ext_add(extlist *l, const char *n) {
    if (l->n >= 64) return;
    for (uint32_t i = 0; i < l->n; i++) if (!strcmp(l->names[i], n)) return;
    l->names[l->n++] = n;
}

static int ext_supported(const VkExtensionProperties *have, uint32_t nhave,
                         const char *want) {
    for (uint32_t i = 0; i < nhave; i++)
        if (!strcmp(have[i].extensionName, want)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

// A Linux guest's window-system set (kl_vulkan_set_linux_wsi). Source 2's
// renderer requires all of VK_KHR_surface + xcb + xlib + wayland to be listed
// before it creates an instance, whatever window it later uses; on this
// runtime its window is SDL's, whose surface is VK_KHR_android_surface (ours).
// So the three names are listed and, at vkCreateInstance, taken off the
// request like android_surface: no xcb/xlib/wayland surface entry point is
// served, and none is reached (the guest's own xcb_connect has no display).
static int g_linux_wsi;
static const char *const g_linux_wsi_names[] = {
    "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface",
};
#define NLINUX_WSI (sizeof g_linux_wsi_names / sizeof *g_linux_wsi_names)
void kl_vulkan_set_linux_wsi(int on) { g_linux_wsi = on; }
static int is_linux_wsi(const char *e) {
    for (size_t i = 0; i < NLINUX_WSI; i++) if (!strcmp(e, g_linux_wsi_names[i])) return 1;
    return 0;
}

static VkResult klvk_EnumerateInstanceExtensionProperties(
        const char *layer, uint32_t *count, VkExtensionProperties *props) {
    vk_init();
    if (!g_avail) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceExtensionProperties real =
        (PFN_vkEnumerateInstanceExtensionProperties)
            real_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t n = 0;
    real(layer, &n, NULL);
    VkExtensionProperties *tmp = calloc(n + 2 + NLINUX_WSI, sizeof *tmp);
    if (!tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    real(layer, &n, tmp);

    // VK_KHR_android_surface is advertised because we implement it. Unity tests
    // for it before enabling it, and a Vulkan build that cannot find a surface
    // extension does not proceed.
    if (!layer && !ext_supported(tmp, n, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        snprintf(tmp[n].extensionName, sizeof tmp[n].extensionName, "%s",
                 VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        tmp[n].specVersion = 6;
        n++;
    }
    for (size_t i = 0; g_linux_wsi && !layer && i < NLINUX_WSI; i++) {
        if (ext_supported(tmp, n, g_linux_wsi_names[i])) continue;
        snprintf(tmp[n].extensionName, sizeof tmp[n].extensionName, "%s", g_linux_wsi_names[i]);
        tmp[n].specVersion = 6;
        n++;
    }

    VkResult r = VK_SUCCESS;
    if (!props) *count = n;
    else {
        uint32_t give = *count < n ? *count : n;
        memcpy(props, tmp, give * sizeof *props);
        if (give < n) r = VK_INCOMPLETE;
        *count = give;
    }
    free(tmp);
    return r;
}

static VkResult klvk_CreateInstance(const VkInstanceCreateInfo *ci,
                                    const VkAllocationCallbacks *alloc,
                                    VkInstance *out) {
    vk_init();
    if (!g_avail) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateInstance real = (PFN_vkCreateInstance)real_gipa(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)
            real_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!real || !enum_ext) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t nhave = 0;
    enum_ext(NULL, &nhave, NULL);
    VkExtensionProperties *have = calloc(nhave ? nhave : 1, sizeof *have);
    if (have) enum_ext(NULL, &nhave, have);

    extlist keep = {{0}, 0};
    for (uint32_t i = 0; ci && i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (!strcmp(e, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
            VKT("instance ext %s -> ours (MoltenVK has no Android WSI)\n", e);
            continue;
        }
        if (g_linux_wsi && is_linux_wsi(e)) {
            VKI("instance ext %s -> listed, not enabled (a Linux guest's WSI set; its "
                "surface is SDL's, over VK_KHR_android_surface)\n", e);
            continue;
        }
        if (have && !ext_supported(have, nhave, e)) {
            VKI("instance ext %s NOT supported by MoltenVK — dropped\n", e);
            continue;
        }
        ext_add(&keep, e);
    }

    // MoltenVK is a portability driver: without this flag a conformant loader
    // enumerates ZERO physical devices and still returns VK_SUCCESS, which reads
    // exactly like "Metal is unavailable" and names nothing.
    VkInstanceCreateFlags flags = ci ? ci->flags : 0;
    if (!have || ext_supported(have, nhave, "VK_KHR_portability_enumeration")) {
        ext_add(&keep, "VK_KHR_portability_enumeration");
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkInstanceCreateInfo mine = ci ? *ci : (VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    mine.flags = flags;
    mine.enabledExtensionCount = keep.n;
    mine.ppEnabledExtensionNames = keep.names;
    // Validation layers do not exist here; asking for one fails the create.
    mine.enabledLayerCount = 0;
    mine.ppEnabledLayerNames = NULL;

    VkResult r = real(&mine, alloc, out);
    free(have);
    if (r == VK_SUCCESS) {
        g_instance = *out;
        VKI("vkCreateInstance ok — %u extension(s)%s\n", keep.n,
            ci && ci->enabledLayerCount ? ", layers dropped" : "");
    } else {
        VKI("vkCreateInstance FAILED %d\n", r);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
static void dev_resolve(klvk_device *d) {
#define R(f) d->f = (PFN_vk##f)real_gdpa(d->dev, "vk" #f)
    R(GetDeviceQueue); R(CreateImage); R(DestroyImage); R(GetImageMemoryRequirements);
    R(AllocateMemory); R(FreeMemory); R(BindImageMemory);
    R(CreateBuffer); R(DestroyBuffer); R(GetBufferMemoryRequirements);
    R(BindBufferMemory); R(MapMemory); R(UnmapMemory);
    R(InvalidateMappedMemoryRanges);
    R(CreateCommandPool); R(DestroyCommandPool); R(AllocateCommandBuffers);
    R(BeginCommandBuffer); R(EndCommandBuffer); R(ResetCommandBuffer);
    R(CmdPipelineBarrier); R(CmdCopyImageToBuffer); R(CmdClearColorImage); R(QueueSubmit);
    R(CreateFence); R(DestroyFence); R(WaitForFences); R(ResetFences);
    R(DeviceWaitIdle); R(QueueWaitIdle);
#undef R
}

// ---------------------------------------------------------------------------
// Feature audit: what the guest ENABLES against what MoltenVK REPORTS
// ---------------------------------------------------------------------------
//
// MoltenVK checks a device create's feature structs flag by flag and fails
// with VK_ERROR_FEATURE_NOT_PRESENT at the FIRST one it lacks ("the 11th flag
// in VkPhysicalDeviceExtendedDynamicState3FeaturesEXT") — one gap per boot,
// and never the name. This walks the whole request instead: every VkBool32
// feature struct on pNext (tools/gen_vk_features.py, 266 of them) plus
// pEnabledFeatures, queried back through vkGetPhysicalDeviceFeatures2 with a
// chain of the same sTypes, and every flag that is ON is compared.
//
// The full requested set is printed once per `who`; each flag the guest asked
// for and MoltenVK does not report is named and CLEARED in place (the
// request's own memory: the create info is the caller's, and a copy could not
// carry the pNext structs this table does not know). Clearing is the only
// answer that lets the create succeed at all; the line is the record that the
// guest asked for it — the guest may still act as if it had it, so read the
// line before blaming anything later in the frame. Unknown sTypes are listed
// by number and left alone. Returns the number of flags cleared.
static const klvk_featstruct *featstruct_of(VkStructureType st) {
    for (size_t i = 0; i < sizeof klvk_featstructs / sizeof *klvk_featstructs; i++)
        if (klvk_featstructs[i].stype == st) return &klvk_featstructs[i];
    return NULL;
}

// Where each cleared flag was, so a caller that must hand the request back
// unchanged (klvk_CreateDevice) can put them back after the create.
typedef struct { VkBool32 *at[64]; unsigned n; } klvk_cleared;

static unsigned feat_audit_one(const char *who, const char *sname,
                               const klvk_featflag *fl, unsigned nf,
                               uint8_t *want, const uint8_t *have, int verbose,
                               klvk_cleared *rec) {
    unsigned cleared = 0;
    for (unsigned i = 0; i < nf; i++) {
        VkBool32 *w = (VkBool32 *)(want + fl[i].off);
        if (!*w) continue;
        VkBool32 h = *(const VkBool32 *)(have + fl[i].off);
        if (verbose)
            VKI("feature audit (%s): %s.%s = requested%s\n", who, sname, fl[i].name,
                h ? "" : " — NOT reported by MoltenVK");
        if (!h) {
            VKI("feature audit (%s): STRIPPED %s.%s — requested, MoltenVK does not "
                "report it\n", who, sname, fl[i].name);
            *w = VK_FALSE;
            cleared++;
            if (rec && rec->n < 64) rec->at[rec->n++] = w;
        }
    }
    return cleared;
}

static int feature_audit(void *phys_v, const void *ci_v, const char *who,
                         klvk_cleared *rec) {
    VkPhysicalDevice phys = (VkPhysicalDevice)phys_v;
    const VkDeviceCreateInfo *ci = (const VkDeviceCreateInfo *)ci_v;
    if (!ci || !phys || !real_gipa) return 0;
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
    if (!gf2) gf2 = (PFN_vkGetPhysicalDeviceFeatures2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!gf2) {
        VKI("feature audit (%s): no vkGetPhysicalDeviceFeatures2 — not audited\n", who);
        return 0;
    }
    // Once per caller label: the full set is long, and a second create (the
    // runtime's own, or a retry) is the same request.
    static char said[4][32];
    int verbose = 1;
    // An empty request (no feature structs, no core features) is not "the set"
    // and does not use up the label.
    for (int i = 0; (ci->pNext || ci->pEnabledFeatures) && i < 4; i++) {
        if (!strncmp(said[i], who, sizeof said[i] - 1)) { verbose = 0; break; }
        if (!said[i][0]) { snprintf(said[i], sizeof said[i], "%s", who); break; }
    }

    // The query chain: one zeroed struct per known sType on the request.
    enum { MAXS = 48 };
    VkBaseOutStructure *req[MAXS];
    void *q[MAXS];
    const klvk_featstruct *fs[MAXS];
    unsigned n = 0, unknown = 0;
    VkPhysicalDeviceFeatures2 top;
    memset(&top, 0, sizeof top);
    top.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkBaseOutStructure *tail = (VkBaseOutStructure *)&top;
    VkPhysicalDeviceFeatures *core_req = (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)ci->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            core_req = &((VkPhysicalDeviceFeatures2 *)p)->features;
            continue;
        }
        const klvk_featstruct *f = featstruct_of(p->sType);
        if (!f) {
            if (verbose) VKI("feature audit (%s): pNext sType %d is not a known feature "
                             "struct — passed through unaudited\n", who, (int)p->sType);
            unknown++;
            continue;
        }
        if (n >= MAXS) break;
        void *b = calloc(1, f->size);
        if (!b) break;
        ((VkBaseOutStructure *)b)->sType = p->sType;
        tail->pNext = (VkBaseOutStructure *)b;
        tail = (VkBaseOutStructure *)b;
        req[n] = p; q[n] = b; fs[n] = f; n++;
    }
    gf2(phys, &top);

    unsigned cleared = 0, asked = 0;
    for (unsigned i = 0; i < n; i++)
        for (unsigned k = 0; k < fs[i]->nflags; k++)
            asked += *(VkBool32 *)((uint8_t *)req[i] + fs[i]->flags[k].off) != 0;
    if (core_req)
        for (size_t k = 0; k < sizeof klvkff_core / sizeof *klvkff_core; k++)
            asked += *(VkBool32 *)((uint8_t *)core_req + klvkff_core[k].off) != 0;
    if (verbose)
        VKI("feature audit (%s): %u feature struct(s) on pNext (%u unknown), %u flag(s) "
            "requested — the full set follows\n", who, n, unknown, asked);
    if (core_req)
        cleared += feat_audit_one(who, "VkPhysicalDeviceFeatures", klvkff_core,
                                  sizeof klvkff_core / sizeof *klvkff_core,
                                  (uint8_t *)core_req, (const uint8_t *)&top.features,
                                  verbose, rec);
    for (unsigned i = 0; i < n; i++) {
        cleared += feat_audit_one(who, fs[i]->name, fs[i]->flags, fs[i]->nflags,
                                  (uint8_t *)req[i], (const uint8_t *)q[i], verbose, rec);
        free(q[i]);
    }
    if (verbose || cleared)
        VKI("feature audit (%s): %u flag(s) stripped\n", who, cleared);
    return (int)cleared;
}

int kl_vulkan_feature_audit(void *phys, const void *ci, const char *who) {
    return feature_audit(phys, ci, who, NULL);
}

static void klvk_perf_install(klvk_device *d);
static void fove_image_gone(VkImage img);   // round F2 (kl_fove.m)
static void fove_rp_note1(const VkRenderPassCreateInfo *ci, VkRenderPass rp);    // round G3
static void fove_rp_note2(const VkRenderPassCreateInfo2 *ci, VkRenderPass rp);
static void fove_xr_image(VkImage img, uint32_t layers, uint32_t w, uint32_t h);
static void fove_xr_image_gone(VkImage img);

// ---- KL_PERF_SPLIT_SUBMIT=1: one Metal command buffer per guest VkCommandBuffer
// (round N14, diagnostic, off by default). Source 2 submits its whole frame as
// one vkQueueSubmit, which MoltenVK commits as ONE MTLCommandBuffer, so
// kl_perf's GPU clock can only say "the frame took N ms". Split, each guest
// command buffer is its own submit (waits on the first, signals and fence on
// the last, same queue, same order — Vulkan's ordering is unchanged) and its
// own MTLCommandBuffer, and kl_perf's ordinal line names each one's GPU time
// next to what it recorded: rendering passes, their area (Mpx, x views) and
// the single-sample resolve copies kl_vulkan added (N12-B).
static int perf_split_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_PERF_SPLIT_SUBMIT", 0);
    return on;
}
#define KLVK_CBS_N 8192
static struct { VkCommandBuffer cb; uint32_t passes, copies; uint64_t px; } g_cbs[KLVK_CBS_N];
static unsigned cbs_slot(VkCommandBuffer cb, int create) {
    uint64_t h = (uint64_t)(uintptr_t)cb;
    h ^= h >> 17; h *= 0x9e3779b97f4a7c15ull; h ^= h >> 29;
    unsigned at = (unsigned)(h % KLVK_CBS_N);
    for (unsigned i = 0; i < 32; i++) {
        unsigned k = (at + i) % KLVK_CBS_N;
        VkCommandBuffer have = __atomic_load_n(&g_cbs[k].cb, __ATOMIC_ACQUIRE);
        if (have == cb) return k;
        if (!have && create) {
            VkCommandBuffer want = VK_NULL_HANDLE;
            if (__atomic_compare_exchange_n(&g_cbs[k].cb, &want, cb, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE) || want == cb)
                return k;
        }
    }
    // Full run of probes: the guest allocates command buffers faster than any
    // fixed table holds them, so the oldest-looking slot is taken over (a
    // diagnostic table; a stale count costs one line's accuracy, not a frame).
    if (!create) return UINT32_MAX;
    unsigned k = (at + (unsigned)(h >> 7) % 32) % KLVK_CBS_N;
    __atomic_store_n(&g_cbs[k].cb, cb, __ATOMIC_RELEASE);
    g_cbs[k].passes = g_cbs[k].copies = 0; g_cbs[k].px = 0;
    return k;
}
static void cbs_reset(VkCommandBuffer cb) {
    if (!perf_split_on()) return;
    unsigned k = cbs_slot(cb, 1);
    if (k != UINT32_MAX) { g_cbs[k].passes = g_cbs[k].copies = 0; g_cbs[k].px = 0; }
}
static void cbs_pass(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t views) {
    if (!perf_split_on()) return;
    unsigned k = cbs_slot(cb, 1);
    if (k != UINT32_MAX) { g_cbs[k].passes++; g_cbs[k].px += (uint64_t)w * h * (views ? views : 1); }
}
static void cbs_copy(VkCommandBuffer cb) {
    if (!perf_split_on()) return;
    unsigned k = cbs_slot(cb, 1);
    if (k != UINT32_MAX) g_cbs[k].copies++;
}

static VkResult klvk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                  const VkAllocationCallbacks *alloc, VkDevice *out) {
    PFN_vkCreateDevice real = (PFN_vkCreateDevice)real_gipa(g_instance, "vkCreateDevice");
    PFN_vkEnumerateDeviceExtensionProperties enum_ext =
        (PFN_vkEnumerateDeviceExtensionProperties)
            real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    // Round 21. MoltenVK reports `_metalFeatures.placementHeaps` straight back
    // out as VK_EXT_image_2d_view_of_3d's features (MVKDevice.mm:741-742, set
    // from the config at :2466), so a plain vkGetPhysicalDeviceFeatures2 is a
    // READBACK of whether MVK_CONFIG_USE_MTLHEAP actually took — the same probe
    // tools/mvk_heapprobe.c runs on the Mac, except this one runs INSIDE the
    // process that matters and therefore also on the headset, where no probe
    // binary can go. Printed once, unconditionally, because "is the knob even
    // on" is the question this port has answered wrong twice.
    {
        PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)
            real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
        if (!gf2) gf2 = (PFN_vkGetPhysicalDeviceFeatures2)
            real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
        if (gf2) {
            VkPhysicalDeviceImage2DViewOf3DFeaturesEXT v3;
            memset(&v3, 0, sizeof v3);
            v3.sType =
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 f2;
            memset(&f2, 0, sizeof f2);
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &v3;
            gf2(phys, &f2);
            VKI("MoltenVK PLACEMENT HEAPS are %s on this physical device "
                "(image2DViewOf3D=%u sampler2DViewOf3D=%u, which MVKDevice.mm "
                ":741-742 reports straight from _metalFeatures.placementHeaps)\n",
                v3.image2DViewOf3D ? "ON" : "OFF",
                (unsigned)v3.image2DViewOf3D, (unsigned)v3.sampler2DViewOf3D);
        } else {
            VKI("MoltenVK placement-heap readback UNAVAILABLE: no "
                "vkGetPhysicalDeviceFeatures2 on this instance\n");
        }
    }

    uint32_t nhave = 0;
    VkExtensionProperties *have = NULL;
    if (enum_ext) {
        enum_ext(phys, NULL, &nhave, NULL);
        have = calloc(nhave ? nhave : 1, sizeof *have);
        if (have) enum_ext(phys, NULL, &nhave, have);
    }

    extlist keep = {{0}, 0};
    for (uint32_t i = 0; ci && i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (have && !ext_supported(have, nhave, e)) {
            VKI("device ext %s NOT supported by MoltenVK — dropped\n", e);
            continue;
        }
        ext_add(&keep, e);
    }
    // The spec REQUIRES this one to be enabled when the device advertises it,
    // and MoltenVK always does. Omitting it is undefined behaviour, not a
    // missing nicety.
    if (have && ext_supported(have, nhave, "VK_KHR_portability_subset"))
        ext_add(&keep, "VK_KHR_portability_subset");
    // ...and one the guest never asks for and we need: `vkExportMetalObjectsEXT`
    // is how the MTLTexture behind an eye VkImage reaches the compositor, and
    // calling it on a device that did not enable this is undefined. It adds no
    // behaviour of its own — nothing else in the device's operation changes —
    // and eye_mtl_texture falls back to MoltenVK's own deprecated entry point if
    // it is absent, so this is an upgrade rather than a requirement.
    if (have && ext_supported(have, nhave, "VK_EXT_metal_objects"))
        ext_add(&keep, "VK_EXT_metal_objects");

    {
        static int said;
        if (!said && ci && ci->enabledExtensionCount && ++said)
            for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
                VKI("device ext requested: %s%s\n", ci->ppEnabledExtensionNames[i],
                    have && !ext_supported(have, nhave, ci->ppEnabledExtensionNames[i])
                        ? " (NOT in MoltenVK)" : "");
    }
    // Cleared for MoltenVK's sake only: the request is the caller's memory,
    // and a caller may read it back after the create (the Alyx host checks its
    // FDM contract on the very struct it handed down) — so every cleared flag
    // is put back once MoltenVK has answered.
    klvk_cleared cleared = {{0}, 0};
    feature_audit(phys, ci, "vkCreateDevice", &cleared);

    VkDeviceCreateInfo mine = ci ? *ci : (VkDeviceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    mine.enabledExtensionCount = keep.n;
    mine.ppEnabledExtensionNames = keep.names;
    mine.enabledLayerCount = 0;
    mine.ppEnabledLayerNames = NULL;

    VkResult r = real(phys, &mine, alloc, out);
    for (unsigned i = 0; i < cleared.n; i++) *cleared.at[i] = VK_TRUE;
    free(have);
    if (r != VK_SUCCESS) { VKI("vkCreateDevice FAILED %d\n", r); return r; }

    klvk_device *d = calloc(1, sizeof *d);
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->dev = *out;
    d->phys = phys;
    d->queue_family = ci && ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
    dev_resolve(d);
    // NOT "a queue of our own" — that comment was wrong for as long as it stood
    // and round 12 caught it. vkGetDeviceQueue(dev, family, 0) returns the
    // GUEST's own queue handle; there is no second queue anywhere in this file.
    // It is the handle the acquire signal and the capture submit go on, AND the
    // one kl_vulkan_frame_done drains. Whether that is all of them is what the
    // queue census below answers.
    if (d->GetDeviceQueue) d->GetDeviceQueue(d->dev, d->queue_family, 0, &d->queue);
    qrec_note_fetch(d->queue, d->queue_family, 0);

    // What the guest ASKED for, and what the device advertises. This is the
    // enumeration the census's per-queue rows are checked against, and it is
    // one line at startup, so it prints unconditionally on host and on device.
    {
        PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)(real_gipa
              ? real_gipa(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties") : NULL);
        if (qfp) {
            uint32_t n = 0;
            qfp(phys, &n, NULL);
            if (n > (uint32_t)(sizeof g_qfam / sizeof *g_qfam))
                n = (uint32_t)(sizeof g_qfam / sizeof *g_qfam);
            g_nqfam = n;
            if (n) qfp(phys, &n, g_qfam);
            for (uint32_t i = 0; i < g_nqfam; i++)
                VKI("queue family %u: %u queue(s), flags 0x%x%s\n", i,
                    g_qfam[i].queueCount, (unsigned)g_qfam[i].queueFlags,
                    (g_qfam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? " (GRAPHICS)" : "");
        } else {
            VKI("queue family properties UNAVAILABLE — the queue census cannot say "
                "which families are graphics families\n");
        }
        if (ci) {
            for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
                VKI("the guest asked for %u queue(s) from family %u\n",
                    ci->pQueueCreateInfos[i].queueCount,
                    ci->pQueueCreateInfos[i].queueFamilyIndex);
            if (ci->queueCreateInfoCount > 1 ||
                (ci->queueCreateInfoCount == 1 && ci->pQueueCreateInfos[0].queueCount > 1))
                VKI("*** the guest created MORE THAN ONE QUEUE. "
                    "kl_vulkan_frame_done drains only family %u index 0 — check the "
                    "queue census before trusting the frame seam\n", d->queue_family);
        } else {
            VKI("vkCreateDevice was called with no create info — queue layout "
                "UNKNOWN\n");
        }
    }

    pthread_mutex_lock(&g_lock);
    if (g_ndev < (int)(sizeof g_devs / sizeof *g_devs)) g_devs[g_ndev++] = d;
    pthread_mutex_unlock(&g_lock);
    klvk_perf_install(d);

    VKI("vkCreateDevice ok — %u extension(s), queue family %u\n", keep.n, d->queue_family);
    return VK_SUCCESS;
}

// A destroyed device leaves this layer's table. Nothing removed it before: every
// guest so far made one device and kept it, and g_devs[0] was "the device"
// everywhere. The Alyx host makes one at nativeStart and may retire it when
// Valve's renderer brings its own; a dangling row there is a use-after-free in
// MoltenVK the first time an image is made on it.
static void klvk_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyDevice real = (PFN_vkDestroyDevice)real_gdpa(dev, "vkDestroyDevice");
    klvk_device *gone = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ndev; i++) {
        if (g_devs[i]->dev != dev) continue;
        gone = g_devs[i];
        for (int k = i; k + 1 < g_ndev; k++) g_devs[k] = g_devs[k + 1];
        g_devs[--g_ndev] = NULL;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    if (g_xr_dev == dev) g_xr_dev = VK_NULL_HANDLE;
    VKI("vkDestroyDevice %p — %s; %d device(s) left\n", (void *)dev,
        gone ? "removed from this layer's table" : "not one this layer created", g_ndev);
    // The row itself is kept (not freed): a frame-path caller may still hold
    // the pointer it read a moment ago, and a leaked row per destroyed device
    // is a few hundred bytes.
    (void)gone;
    if (real) real(dev, alloc);
}

// ---------------------------------------------------------------------------
// Surface — synthesized whole. There is no Android window here and nothing that
// has to reach a screen: this arc ends in a file on disk.
// ---------------------------------------------------------------------------
static VkResult klvk_CreateAndroidSurfaceKHR(VkInstance inst,
        const VkAndroidSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
        VkSurfaceKHR *out) {
    (void)inst; (void)ci; (void)alloc;
    klvk_surface *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->magic = KLVK_SURF_MAGIC;
    // The eye size the rest of the port already agreed on, so a Vulkan frame is
    // the same shape as the GLES one and the compositor seam does not have to
    // learn a second size later.
    s->w = (uint32_t)kl_env_int("KL_VK_WIDTH", 2064);
    s->h = (uint32_t)kl_env_int("KL_VK_HEIGHT", 2208);
    *out = (VkSurfaceKHR)s;
    VKI("vkCreateAndroidSurfaceKHR -> synthetic surface %ux%u\n", s->w, s->h);
    return VK_SUCCESS;
}

static klvk_surface *surf_of(VkSurfaceKHR h) {
    klvk_surface *s = (klvk_surface *)h;
    return (s && s->magic == KLVK_SURF_MAGIC) ? s : NULL;
}

static void klvk_DestroySurfaceKHR(VkInstance inst, VkSurfaceKHR surf,
                                   const VkAllocationCallbacks *alloc) {
    (void)inst; (void)alloc;
    klvk_surface *s = surf_of(surf);
    if (s) { s->magic = 0; free(s); }
}

static VkResult klvk_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice p, uint32_t fam,
        VkSurfaceKHR surf, VkBool32 *out) {
    (void)p; (void)fam; (void)surf;
    *out = VK_TRUE;
    return VK_SUCCESS;
}

static VkResult klvk_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, VkSurfaceCapabilitiesKHR *c) {
    (void)p;
    klvk_surface *s = surf_of(surf);
    uint32_t w = s ? s->w : 1024, h = s ? s->h : 1024;
    memset(c, 0, sizeof *c);
    c->minImageCount = 2;
    c->maxImageCount = KLVK_MAX_IMAGES;
    c->currentExtent.width = w;   c->currentExtent.height = h;
    c->minImageExtent = c->currentExtent;
    c->maxImageExtent = c->currentExtent;
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
                                 VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    // TRANSFER_SRC is not decoration: it is what lets the capture read the image
    // the guest presented. A guest that trims usage to what the surface reports
    // would otherwise hand us an image that cannot legally be copied from.
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_STORAGE_BIT;
    return VK_SUCCESS;
}

static const VkFormat k_surf_formats[] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R8G8B8A8_SRGB,  VK_FORMAT_B8G8R8A8_SRGB,
};

static VkResult klvk_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, uint32_t *count, VkSurfaceFormatKHR *out) {
    (void)p; (void)surf;
    uint32_t n = sizeof k_surf_formats / sizeof *k_surf_formats;
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t give = *count < n ? *count : n;
    for (uint32_t i = 0; i < give; i++) {
        out[i].format = k_surf_formats[i];
        out[i].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
    *count = give;
    return give < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult klvk_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, uint32_t *count, VkPresentModeKHR *out) {
    (void)p; (void)surf;
    static const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    uint32_t n = sizeof modes / sizeof *modes;
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t give = *count < n ? *count : n;
    memcpy(out, modes, give * sizeof *out);
    *count = give;
    return give < n ? VK_INCOMPLETE : VK_SUCCESS;
}

// Declared rather than included: klepton.h is the runtime's own umbrella header
// and this translation unit deliberately depends on almost nothing. Both of
// these are defined elsewhere in the same binary — kl_mem_budget_bytes in
// runtime/libc/kl_libc.c, klvk_mtl_allocated a few hundred lines below, beside
// the image ledger it serves.
static unsigned long long klvk_mtl_allocated(void);

// ---------------------------------------------------------------------------
// The GUEST's own Vulkan memory — measured, and budgeted.
//
// Round 10 measured OUR images (the ledger below) and MTLDevice's total, and
// the difference between them named the guest's GPU appetite for the first
// time: 3902 MiB of 4355 at the moment jetsam took launch #10. What it could
// not say was WHICH allocations those were, or whether any of them were mirrored
// host-side. Both questions are answered here, and by pass-through accounting
// rather than by any copy of our own: vkAllocateMemory / vkFreeMemory /
// vkMapMemory / vkUnmapMemory go straight to MoltenVK, exactly as they did
// before, with an atomic add on the way past.
//
// That is worth stating plainly because it is what rules out the first
// hypothesis anybody reaches for. Before this file grew these four entries,
// `g_ours[]` had FOURTEEN names in it and not one of them was a memory or an
// image entry point — the guest's vkAllocateMemory was MoltenVK's, its
// vkMapMemory was MoltenVK's, and there was no seam at which Klepton could have
// been shadowing a guest resource even if it had wanted to. The doubling in the
// footprint was never ours to remove. These counters exist so the next log can
// say so with a number instead of an argument.
#define KLVK_MEMTYPES 32
static uint64_t g_guest_mem_bytes[KLVK_MEMTYPES];   // live, by memory type
static uint32_t g_guest_mem_n[KLVK_MEMTYPES];
static uint64_t g_guest_mem_peak;                   // live total, high-water
static uint64_t g_guest_mem_live;
static uint64_t g_guest_mapped;                     // bytes currently vkMapMemory'd
static uint32_t g_guest_mapped_n;
static VkPhysicalDeviceMemoryProperties g_mem_props;  // as MoltenVK reports them
static int      g_mem_props_read;

// A VkDeviceMemory we handed out, so vkFreeMemory can un-charge the right
// bucket and vkMapMemory can name the size it mapped. A flat table because the
// guest holds hundreds of these, not tens of thousands, and a hash would be a
// second thing to get wrong on the allocation path.
#define KLVK_ALLOC_N 4096
static struct { VkDeviceMemory mem; uint64_t bytes; uint32_t type; uint8_t mapped; }
    g_guest_alloc[KLVK_ALLOC_N];
static pthread_mutex_t g_guest_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_guest_alloc_overflow;

// The GPU half of the memory budget, and the door this runtime had left open.
//
// runtime/libc/kl_libc.c already argues the whole case for the CPU half, and
// the argument is quoted rather than paraphrased because it is exactly the same
// one: "A Vision Pro has 16 GB and a development Mac 32-128; a Quest 2 has 6,
// and every streaming budget, pool ceiling and quality tier in these titles was
// chosen against that number. Telling a 6 GB title it has 20x the room, and
// then never telling it the room ran out, is how it reaches an OOM without ever
// dropping a cache." That file then answers /proc/meminfo, sysinfo(2) and
// ActivityManager with one budget, and fires Application.lowMemory against it.
//
// It stopped at the CPU. On the GPU side MoltenVK reports its DEVICE_LOCAL heap
// as the whole machine, and a Unity Vulkan guest turns that straight into
// SystemInfo.graphicsMemorySize — measured in launch #10, line 1089:
//
//     SystemInfo.systemMemorySize  = 6144      <- ours, Quest-shaped
//     SystemInfo.graphicsMemorySize = 15737    <- MoltenVK's, whole-device
//
// So the guest was told, in one breath, that it is running on a 6 GB machine
// with 16 GB of video memory. On a unified-memory device those are the SAME
// pages, and the second number is the one Unity's render-target pool, texture
// streaming budget and VK_EXT_memory_budget trimming all size themselves
// against. Clamping the heap to the budget the rest of the runtime already
// reports is not a restriction invented here; it is the first time the two
// halves of the answer agree.
//
// KL_VK_HEAP_MB overrides. It is only ever a CLAMP: a heap MoltenVK reports as
// smaller than the budget is passed through untouched, because the point is to
// stop over-reporting, not to start under-reporting.

// vkGetPhysicalDeviceMemoryProperties, clamped. The memory TYPES are passed
// through untouched — which type index carries which property flags is
// MoltenVK's answer and a guest that allocates from a type we invented would be
// allocating from nothing — and only the heap SIZES are touched.
// Defined beside the image ledger below, because that is where the lazy-type
// mask it fills is spent; called from here, because this is where the guest
// asks the question.
static void klvk_dump_mem_types(const VkPhysicalDeviceMemoryProperties *mp);

static void klvk_clamp_heaps(VkPhysicalDeviceMemoryProperties *mp) {
    if (!mp) return;
    uint64_t cap = kl_vulkan_heap_budget_bytes();
    if (!cap) return;
    static int said;
    for (uint32_t i = 0; i < mp->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++) {
        if (mp->memoryHeaps[i].size <= cap) continue;
        if (!said++)
            VKI("memory heap %u reported %llu MiB — clamped to the %llu MiB budget "
                "the rest of this runtime reports (SystemInfo.graphicsMemorySize "
                "follows this number, and it was answering 16 GB inside a 6 GB "
                "machine)\n", i,
                (unsigned long long)(mp->memoryHeaps[i].size >> 20),
                (unsigned long long)(cap >> 20));
        mp->memoryHeaps[i].size = cap;
    }
}

static void klvk_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice phys,
        VkPhysicalDeviceMemoryProperties *out) {
    PFN_vkGetPhysicalDeviceMemoryProperties real =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!real || !out) return;
    real(phys, out);
    if (!g_mem_props_read) { g_mem_props = *out; g_mem_props_read = 1; }
    klvk_dump_mem_types(out);
    klvk_clamp_heaps(out);
}

// The ...2 form, which is also how VK_EXT_memory_budget is asked: the guest
// chains a VkPhysicalDeviceMemoryBudgetPropertiesEXT onto pNext and Vulkan
// fills in a per-heap budget and usage. Unity's Vulkan backend polls exactly
// this to decide when to trim, so leaving it at MoltenVK's whole-machine answer
// would have undone the clamp above through the very structure the engine
// trusts most. heapUsage is answered from [MTLDevice currentAllocatedSize],
// which is the real number and is what the [vk] memory line already prints.
static void klvk_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice phys,
        VkPhysicalDeviceMemoryProperties2 *out) {
    PFN_vkGetPhysicalDeviceMemoryProperties2 real =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties2");
    if (!real) real = (PFN_vkGetPhysicalDeviceMemoryProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties2KHR");
    if (!real || !out) return;
    real(phys, out);
    if (!g_mem_props_read) { g_mem_props = out->memoryProperties; g_mem_props_read = 1; }
    klvk_dump_mem_types(&out->memoryProperties);
    klvk_clamp_heaps(&out->memoryProperties);

    uint64_t cap = kl_vulkan_heap_budget_bytes();
    uint64_t used = klvk_mtl_allocated();
    for (VkBaseOutStructure *n = (VkBaseOutStructure *)out->pNext; n; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            continue;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT *b =
            (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)n;
        for (uint32_t i = 0; i < out->memoryProperties.memoryHeapCount &&
                             i < VK_MAX_MEMORY_HEAPS; i++) {
            if (cap && b->heapBudget[i] > cap) b->heapBudget[i] = cap;
            // Usage is process-wide on a unified-memory device, so it is
            // reported against heap 0 and left alone elsewhere rather than
            // counted once per heap — a guest summing the heaps would otherwise
            // see the same bytes several times.
            if (i == 0 && used) b->heapUsage[i] = used;
        }
    }
}

static VkResult klvk_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai,
        const VkAllocationCallbacks *alloc, VkDeviceMemory *out) {
    klvk_device *d = dev_find(dev);
    PFN_vkAllocateMemory real = d && d->AllocateMemory ? d->AllocateMemory
        : (PFN_vkAllocateMemory)(real_gdpa ? real_gdpa(dev, "vkAllocateMemory") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(dev, ai, alloc, out);
    if (r != VK_SUCCESS || !ai || !out) return r;
    uint32_t t = ai->memoryTypeIndex < KLVK_MEMTYPES ? ai->memoryTypeIndex : KLVK_MEMTYPES - 1;
    uint64_t n = (uint64_t)ai->allocationSize;
    pthread_mutex_lock(&g_guest_alloc_lock);
    g_guest_mem_bytes[t] += n;
    g_guest_mem_n[t]++;
    g_guest_mem_live += n;
    if (g_guest_mem_live > g_guest_mem_peak) g_guest_mem_peak = g_guest_mem_live;
    unsigned i = 0;
    for (; i < KLVK_ALLOC_N; i++) if (!g_guest_alloc[i].mem) break;
    if (i < KLVK_ALLOC_N) {
        g_guest_alloc[i].mem = *out;
        g_guest_alloc[i].bytes = n;
        g_guest_alloc[i].type = t;
        g_guest_alloc[i].mapped = 0;
    } else {
        g_guest_alloc_overflow++;   // the totals stay right; the FREE will miss
    }
    pthread_mutex_unlock(&g_guest_alloc_lock);
    return r;
}

static void klvk_FreeMemory(VkDevice dev, VkDeviceMemory mem,
                            const VkAllocationCallbacks *alloc) {
    klvk_device *d = dev_find(dev);
    PFN_vkFreeMemory real = d && d->FreeMemory ? d->FreeMemory
        : (PFN_vkFreeMemory)(real_gdpa ? real_gdpa(dev, "vkFreeMemory") : NULL);
    if (mem) {
        pthread_mutex_lock(&g_guest_alloc_lock);
        for (unsigned i = 0; i < KLVK_ALLOC_N; i++) {
            if (g_guest_alloc[i].mem != mem) continue;
            uint32_t t = g_guest_alloc[i].type;
            uint64_t n = g_guest_alloc[i].bytes;
            if (g_guest_mem_bytes[t] >= n) g_guest_mem_bytes[t] -= n; else g_guest_mem_bytes[t] = 0;
            if (g_guest_mem_n[t]) g_guest_mem_n[t]--;
            if (g_guest_mem_live >= n) g_guest_mem_live -= n; else g_guest_mem_live = 0;
            if (g_guest_alloc[i].mapped) {
                // Freeing a still-mapped allocation is legal and Unity does it.
                if (g_guest_mapped >= n) g_guest_mapped -= n; else g_guest_mapped = 0;
                if (g_guest_mapped_n) g_guest_mapped_n--;
            }
            memset(&g_guest_alloc[i], 0, sizeof g_guest_alloc[i]);
            break;
        }
        pthread_mutex_unlock(&g_guest_alloc_lock);
    }
    if (real) real(dev, mem, alloc);
}

// Mapped bytes are the whole of the double-count question. A MoltenVK
// allocation on shared storage is an MTLBuffer: charged to
// [MTLDevice currentAllocatedSize] always, and ALSO visible to the region walk
// as anonymous dirty pages once it is mapped. Every other allocation is private
// and invisible to the walk. So "mapped" is exactly the overlap between the two
// halves of the tripwire dump, and printing it is what turns
// anon + Metal ?= footprint from an estimate into an identity.
static VkResult klvk_MapMemory(VkDevice dev, VkDeviceMemory mem, VkDeviceSize off,
        VkDeviceSize size, VkMemoryMapFlags flags, void **out) {
    klvk_device *d = dev_find(dev);
    PFN_vkMapMemory real = d && d->MapMemory ? d->MapMemory
        : (PFN_vkMapMemory)(real_gdpa ? real_gdpa(dev, "vkMapMemory") : NULL);
    if (!real) return VK_ERROR_MEMORY_MAP_FAILED;
    VkResult r = real(dev, mem, off, size, flags, out);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_lock(&g_guest_alloc_lock);
    for (unsigned i = 0; i < KLVK_ALLOC_N; i++) {
        if (g_guest_alloc[i].mem != mem || g_guest_alloc[i].mapped) continue;
        g_guest_alloc[i].mapped = 1;
        g_guest_mapped += g_guest_alloc[i].bytes;
        g_guest_mapped_n++;
        break;
    }
    pthread_mutex_unlock(&g_guest_alloc_lock);
    return r;
}

static void klvk_UnmapMemory(VkDevice dev, VkDeviceMemory mem) {
    klvk_device *d = dev_find(dev);
    PFN_vkUnmapMemory real = d && d->UnmapMemory ? d->UnmapMemory
        : (PFN_vkUnmapMemory)(real_gdpa ? real_gdpa(dev, "vkUnmapMemory") : NULL);
    pthread_mutex_lock(&g_guest_alloc_lock);
    for (unsigned i = 0; i < KLVK_ALLOC_N; i++) {
        if (g_guest_alloc[i].mem != mem || !g_guest_alloc[i].mapped) continue;
        g_guest_alloc[i].mapped = 0;
        if (g_guest_mapped >= g_guest_alloc[i].bytes) g_guest_mapped -= g_guest_alloc[i].bytes;
        else g_guest_mapped = 0;
        if (g_guest_mapped_n) g_guest_mapped_n--;
        break;
    }
    pthread_mutex_unlock(&g_guest_alloc_lock);
    if (real) real(dev, mem);
}

// ---------------------------------------------------------------------------
// The FORMAT seam — runtime half.
// ---------------------------------------------------------------------------
//
// vkCreateImage was NOT intercepted before this round, and that is the whole
// shape of the launch-#11 crash: the guest's image call went straight to
// MoltenVK, MoltenVK mapped the VkFormat to MTLPixelFormatInvalid, built a
// descriptor with pixelFormat 0 anyway, and Metal's validation aborted the
// process from a guest worker thread. There is no Vulkan error code anywhere
// in that path for the guest to have handled.
//
// Two things happen here, and they are deliberately in this order:
//
//   1. a CENSUS. Every guest image is counted by format, and the first of each
//      distinct format is logged whole — format, type, tiling, usage, flags,
//      extent, mips, layers, samples. Round 11's log could name the crash but
//      not the format, because nothing had ever looked. The next log names it
//      whether or not the substitution below turns out to be needed.
//
//   2. a SUBSTITUTION, and only for a format MoltenVK itself reports as
//      unmappable. The preferred fix would have been (a) — answer "not
//      supported" in the format-property queries and let Unity pick its
//      fallback — but that answer was ALREADY being given: these queries were
//      never intercepted, so MoltenVK's own honest zero-feature answer is what
//      the guest saw, and it created the image anyway. Reporting it harder
//      cannot work. So the format is replaced at vkCreateImage with the
//      nearest thing Metal has (kl_vulkan_format_substitute), and every place
//      that has to agree with the image's format follows it.
//
// The honest limit, stated once: a substitution that changes the texel size
// (24-bit RGB -> 32-bit RGBA, or a compressed block -> RGBA8) makes the
// guest's staging data the wrong shape. A vkCmdCopyBufferToImage recorded for
// the original would read 4 bytes per texel out of a buffer holding 3 and run
// off the end. Repacking it properly needs a compute pass injected into the
// guest's command buffer; that is not this round. Until it lands, such a copy
// is replaced with a clear — the texture is a flat mid-grey instead of the
// right pixels, and the game does not die. Same-size substitutions (the
// depth/stencil ones) are exact and are left completely alone.

// MoltenVK's own verdict, cached. A VkFormat it cannot map answers zero in ALL
// THREE feature masks, and nothing else does — a format that is real but
// unusable for a given tiling still reports buffer features. So this is the
// exact predicate, taken from the implementation rather than from a table of
// what Apple GPUs are believed to support.
#define KLVK_FMT_N 256
static int8_t g_fmt_unmappable[KLVK_FMT_N];   // 0 unknown, 1 no, 2 yes

static int fmt_unmappable(klvk_device *d, VkFormat f) {
    if (!d) return 0;
    int idx = (int)f;
    int cached = (idx >= 0 && idx < KLVK_FMT_N) ? g_fmt_unmappable[idx] : 0;
    if (cached) return cached == 2;

    PFN_vkGetPhysicalDeviceFormatProperties gfp =
        (PFN_vkGetPhysicalDeviceFormatProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceFormatProperties");
    if (!gfp) return 0;
    VkFormatProperties p;
    memset(&p, 0, sizeof p);
    gfp(d->phys, f, &p);
    int bad = !(p.linearTilingFeatures | p.optimalTilingFeatures | p.bufferFeatures);
    if (idx >= 0 && idx < KLVK_FMT_N) g_fmt_unmappable[idx] = bad ? 2 : 1;
    return bad;
}

// The census. Indexed by format, with everything past the core range folded
// into one bucket — an extension format is worth counting but not worth a
// 1000-entry table.
#define KLVK_FMTC_N 200
static uint32_t g_img_by_fmt[KLVK_FMTC_N];
static uint32_t g_img_fmt_seen;     // how many distinct formats have appeared
static uint32_t g_img_fmt_printed;  // ...and how many the report line has shown

// Images we substituted, so vkCreateImageView and vkCmdCopyBufferToImage can
// agree with what was actually created. Flat, and cleaned up by
// vkDestroyImage, for the same reason the allocation table is: the guest holds
// hundreds of these at a time, not tens of thousands.
#define KLVK_SUBST_N 4096
static struct {
    VkImage  img;
    uint32_t orig, sub;
    uint32_t mips, layers;
    uint8_t  differs;
} g_subst[KLVK_SUBST_N];
static pthread_mutex_t g_subst_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_subst_n, g_subst_overflow, g_subst_skipped_copies;

static int subst_find(VkImage img, uint32_t *sub, uint32_t *mips,
                      uint32_t *layers, uint8_t *differs) {
    int hit = 0;
    pthread_mutex_lock(&g_subst_lock);
    for (unsigned i = 0; i < KLVK_SUBST_N; i++) {
        if (g_subst[i].img != img) continue;
        if (sub) *sub = g_subst[i].sub;
        if (mips) *mips = g_subst[i].mips;
        if (layers) *layers = g_subst[i].layers;
        if (differs) *differs = g_subst[i].differs;
        hit = 1;
        break;
    }
    pthread_mutex_unlock(&g_subst_lock);
    return hit;
}

static const char *usage_words(VkImageUsageFlags u, char *buf, size_t n) {
    buf[0] = 0;
    struct { VkImageUsageFlags bit; const char *w; } t[] = {
        { VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "xfer-src" },
        { VK_IMAGE_USAGE_TRANSFER_DST_BIT, "xfer-dst" },
        { VK_IMAGE_USAGE_SAMPLED_BIT, "sampled" },
        { VK_IMAGE_USAGE_STORAGE_BIT, "storage" },
        { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "colour-att" },
        { VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, "depth-att" },
        { VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, "transient" },
        { VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, "input-att" },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (u & t[i].bit) {
            if (buf[0]) strlcat(buf, "|", n);
            strlcat(buf, t[i].w, n);
        }
    if (!buf[0]) strlcat(buf, "none", n);
    return buf;
}

// ---------------------------------------------------------------------------
// The live image ledger, by BYTES — the question launch #13 could not answer.
//
// Round 13's census counts images by FORMAT and prints the first of each. A
// count is not a size, and launch #13 proved it: the guest reached 3568 MiB of
// peak in SIXTY-SIX allocations, so ~1757 MiB of that arrived in three of
// them. "390 allocations" says nothing about which three. This table says
// which images are live and what each one actually charges.
//
// The bytes are NOT computed from the extent. They are
// vkGetImageMemoryRequirements' own answer, which is the only one that
// accounts for tiling, alignment, MoltenVK's own format substitutions (launch
// #13's log shows it rewriting D24_UNORM_S8_UINT to D32_SFLOAT_S8_UINT on its
// own, one line before ours would have) and the fact that a depth/stencil
// image is two Metal textures. The memory TYPE is whatever vkBindImageMemory
// bound it to, looked up in the allocation table vkAllocateMemory already
// keeps. Both entry points are pure pass-through accounting.
// A HASH table, not a list (round N12-B): the 4096-slot list it was overflowed
// in every Alyx level (the host after ~640 more images, the headset at ~1800
// live), and an image it never held answered "0 samples" — so the eye depth
// that a level created after the overflow was invisible to the single-sample
// resolve check below, and the census read 0 while the pass ran every frame.
// Open addressing with tombstones (a deleted row keeps its probe chain alive)
// and an in-place rehash when tombstones pile up.
#define KLVK_GIMG_N 32768
typedef struct {
    VkImage  img;
    uint32_t fmt, orig;      // as created (post-substitution), as the guest asked
    uint32_t w, h, d;
    uint16_t mips, layers, samples;
    uint8_t  tomb;           // deleted row: img is 0, the probe chain goes on
    uint32_t usage;
    uint64_t bytes;          // 0 until vkGetImageMemoryRequirements is asked
    int16_t  memtype;        // -1 until vkBindImageMemory says
} klvk_gimg;
static klvk_gimg g_gimg[KLVK_GIMG_N];
static pthread_mutex_t g_gimg_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_gimg_live, g_gimg_overflow, g_gimg_tombs;
static uint64_t g_gimg_bytes;                 // live total, images only
static uint64_t g_gimg_transient_bytes;       // ...of which TRANSIENT_ATTACHMENT
static uint32_t g_gimg_transient_n;

static unsigned gimg_hash(VkImage img) {
    uint64_t h = (uint64_t)(uintptr_t)img;
    h ^= h >> 17; h *= 0x9e3779b97f4a7c15ull; h ^= h >> 29;
    return (unsigned)(h % KLVK_GIMG_N);
}

// Caller holds g_gimg_lock. The row for img, or NULL.
static klvk_gimg *gimg_find_locked(VkImage img) {
    if (!img) return NULL;
    unsigned i = gimg_hash(img);
    for (unsigned n = 0; n < KLVK_GIMG_N; n++, i = (i + 1) % KLVK_GIMG_N) {
        if (g_gimg[i].img == img) return &g_gimg[i];
        if (!g_gimg[i].img && !g_gimg[i].tomb) return NULL;
    }
    return NULL;
}

// Caller holds g_gimg_lock: drop every tombstone by re-inserting the live rows.
static void gimg_rehash_locked(void) {
    static klvk_gimg tmp[KLVK_GIMG_N];
    unsigned n = 0;
    for (unsigned i = 0; i < KLVK_GIMG_N; i++)
        if (g_gimg[i].img) tmp[n++] = g_gimg[i];
    memset(g_gimg, 0, sizeof g_gimg);
    for (unsigned k = 0; k < n; k++) {
        unsigned i = gimg_hash(tmp[k].img);
        while (g_gimg[i].img) i = (i + 1) % KLVK_GIMG_N;
        g_gimg[i] = tmp[k];
    }
    g_gimg_tombs = 0;
}

static void gimg_add(VkImage img, const VkImageCreateInfo *ci, uint32_t created_fmt) {
    if (!img || !ci) return;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(img);   // a handle MoltenVK reused
    int fresh = !g;
    if (!g) {
        if (g_gimg_live + g_gimg_tombs >= KLVK_GIMG_N * 3 / 4) {
            if (g_gimg_tombs) gimg_rehash_locked();
            if (g_gimg_live >= KLVK_GIMG_N * 3 / 4) {
                g_gimg_overflow++;
                pthread_mutex_unlock(&g_gimg_lock);
                return;
            }
        }
        unsigned i = gimg_hash(img);
        while (g_gimg[i].img) i = (i + 1) % KLVK_GIMG_N;
        g = &g_gimg[i];
        if (g->tomb && g_gimg_tombs) g_gimg_tombs--;
    } else if (g->bytes) {
        g_gimg_bytes -= g->bytes > g_gimg_bytes ? g_gimg_bytes : g->bytes;
    }
    memset(g, 0, sizeof *g);
    g->img     = img;
    g->fmt     = created_fmt;
    g->orig    = (uint32_t)ci->format;
    g->w       = ci->extent.width;
    g->h       = ci->extent.height;
    g->d       = ci->extent.depth;
    g->mips    = (uint16_t)(ci->mipLevels ? ci->mipLevels : 1);
    g->layers  = (uint16_t)(ci->arrayLayers ? ci->arrayLayers : 1);
    g->samples = (uint16_t)ci->samples;
    g->usage   = (uint32_t)ci->usage;
    g->memtype = -1;
    if (fresh) g_gimg_live++;
    pthread_mutex_unlock(&g_gimg_lock);
}

static void gimg_del(VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(img);
    if (g) {
        uint64_t b = g->bytes;
        if (b) {
            g_gimg_bytes -= b > g_gimg_bytes ? g_gimg_bytes : b;
            if (g->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
                g_gimg_transient_bytes -=
                    b > g_gimg_transient_bytes ? g_gimg_transient_bytes : b;
                if (g_gimg_transient_n) g_gimg_transient_n--;
            }
        }
        memset(g, 0, sizeof *g);
        g->tomb = 1;
        g_gimg_tombs++;
        if (g_gimg_live) g_gimg_live--;
    }
    pthread_mutex_unlock(&g_gimg_lock);
}

static uint32_t gimg_usage_of(VkImage img) {
    uint32_t u = 0;
    if (!img) return 0;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(img);
    if (g) u = g->usage;
    pthread_mutex_unlock(&g_gimg_lock);
    return u;
}

// 0 when the image is not in the table.
static uint32_t gimg_samples_of(VkImage img) {
    uint32_t n = 0;
    if (!img) return 0;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(img);
    if (g) n = g->samples;
    pthread_mutex_unlock(&g_gimg_lock);
    return n;
}

// 0 when the image is not in the table; else 1 and its created format/extent.
static int gimg_info(VkImage img, uint32_t *fmt, uint32_t *w, uint32_t *h,
                     uint32_t *layers, uint32_t *samples) {
    int found = 0;
    if (!img) return 0;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(img);
    if (g) {
        *fmt = g->fmt; *w = g->w; *h = g->h;
        *layers = g->layers; *samples = g->samples;
        found = 1;
    }
    pthread_mutex_unlock(&g_gimg_lock);
    return found;
}

static void gimg_note_bytes(VkImage img, uint64_t bytes) {
    if (!img || !bytes) return;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(img);
    if (g && !g->bytes) {
        g->bytes = bytes;
        g_gimg_bytes += bytes;
        if (g->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
            g_gimg_transient_bytes += bytes;
            g_gimg_transient_n++;
        }
    }
    pthread_mutex_unlock(&g_gimg_lock);
}

static void gimg_note_type(VkImage img, VkDeviceMemory mem) {
    if (!img || !mem) return;
    int16_t t = -1;
    pthread_mutex_lock(&g_guest_alloc_lock);
    for (unsigned i = 0; i < KLVK_ALLOC_N; i++)
        if (g_guest_alloc[i].mem == mem) { t = (int16_t)g_guest_alloc[i].type; break; }
    pthread_mutex_unlock(&g_guest_alloc_lock);
    if (t < 0) return;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(img);
    if (g) g->memtype = t;
    pthread_mutex_unlock(&g_gimg_lock);
}

// The memory TYPES, dumped once.
//
// Launch #13's ledger listed two of them, because those are the two the guest
// allocated FROM — which is a different question from which ones EXIST. If
// MoltenVK exposes a LAZILY_ALLOCATED type here then a transient attachment (a
// URP depth buffer written and consumed inside one render pass, never spilled)
// can cost zero bytes, and the guest never having used it is a finding rather
// than a fact of the platform. If it does not, that is the answer too, and the
// steer below has nothing to steer to.
static uint32_t g_lazy_mask;      // memory types carrying LAZILY_ALLOCATED
static int g_mem_types_said;

static void klvk_dump_mem_types(const VkPhysicalDeviceMemoryProperties *mp) {
    if (!mp || g_mem_types_said) return;
    g_mem_types_said = 1;
    for (uint32_t i = 0; i < mp->memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++) {
        VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) g_lazy_mask |= 1u << i;
        char w[160]; w[0] = 0;
        struct { VkMemoryPropertyFlags b; const char *n; } t[] = {
            { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,     "DEVICE_LOCAL" },
            { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,     "HOST_VISIBLE" },
            { VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,    "HOST_COHERENT" },
            { VK_MEMORY_PROPERTY_HOST_CACHED_BIT,      "HOST_CACHED" },
            { VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED" },
        };
        for (size_t k = 0; k < sizeof t / sizeof *t; k++)
            if (f & t[k].b) {
                if (w[0]) strlcat(w, " ", sizeof w);
                strlcat(w, t[k].n, sizeof w);
            }
        uint32_t hi = mp->memoryTypes[i].heapIndex;
        unsigned long long hs = hi < mp->memoryHeapCount && hi < VK_MAX_MEMORY_HEAPS
                              ? (unsigned long long)(mp->memoryHeaps[hi].size >> 20) : 0;
        VKI("memory type %u -> heap %u (%llu MiB): %s\n", i, hi, hs,
            w[0] ? w : "(none)");
    }
    VKI("memory types: %u total, LAZILY_ALLOCATED mask 0x%x — %s\n",
        mp->memoryTypeCount, g_lazy_mask,
        g_lazy_mask ? "a transient attachment CAN be memoryless on this device"
                    : "there is no memoryless type here, so every transient "
                      "attachment costs its full size");
}

// vkGetImageMemoryRequirements — pass-through, plus the size the ledger needs.
//
// ...and the one place a transient attachment can be made free. A tile GPU
// never has to spill a depth buffer that is written and consumed inside a
// single render pass; Metal calls that MTLStorageModeMemoryless and MoltenVK
// exposes it as a LAZILY_ALLOCATED memory type. Unity asks for exactly that
// shape — its URP depth attachments carry VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
// and Unity's own log line beside them reads "Ignoring depth surface store
// action as it is memoryless" — but the engine picks the memory type itself
// out of memoryTypeBits, and in every launch so far it picked a DEVICE_LOCAL
// one.
//
// Masking memoryTypeBits down to the lazy types is legal (Vulkan requires a
// transient image's requirements to include a lazily-allocated type wherever
// one exists, and forbids such an image from being a transfer source or
// destination) but it is OFF by default. This port is playable, and a wrong
// answer here is a black picture rather than a log line. The MEASUREMENT is
// unconditional — the report prints how many transient bytes are live — so the
// next round can turn it on against a number instead of a hope.
static int lazy_transient_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_LAZY_TRANSIENT", 0) != 0;
    return v;
}

// ...and the half of the steer that has to happen BEFORE the image exists.
//
// Launch #16 turned the steer on at 4032 and the log answered:
//
//   [vk] memory type 2 -> heap 0: DEVICE_LOCAL LAZILY_ALLOCATED
//   [vk] a transient attachment's memoryTypeBits (0x3) carry no
//        LAZILY_ALLOCATED type — it cannot be made memoryless
//
// Both lines are true at once, and the reason is in MoltenVK's own text: it
// removes the lazily-allocated types from an image whose usage carries
// INPUT_ATTACHMENT alongside TRANSIENT_ATTACHMENT (the disassembly is quoted in
// kl_vulkan.h). Unity sets that bit on every URP attachment unconditionally, so
// six eye-size attachments — 3x 327 MiB D32S8 and 3x 259 MiB RGBA8 at 2x MSAA,
// 1757 MiB — were on real DEVICE_LOCAL memory in a process jetsam killed at
// 7688 MiB. Masking memoryTypeBits after the fact cannot fix that; the mask
// arrives already stripped. The bit has to be gone at vkCreateImage.
//
// KL_VK_LAZY_KEEP_INPUT=1 puts it back without turning the whole steer off,
// which is the one-knob backout if a subpass ever really does read one of
// these. What it would look like: MoltenVK builds the MTLTexture without
// MTLTextureUsageShaderRead, and a subpass input read of it is a Metal
// validation failure that names the texture.
static int lazy_keep_input(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_LAZY_KEEP_INPUT", 0) != 0;
    return v;
}

// The usage to hand MoltenVK. Returns 1 if it differs from the guest's.
static int lazy_fix_usage(const VkImageCreateInfo *ci, VkImageUsageFlags *out) {
    if (!ci || !out) return 0;
    *out = ci->usage;
    if (!lazy_transient_on() || !g_lazy_mask || lazy_keep_input()) return 0;
    VkImageUsageFlags fixed =
        (VkImageUsageFlags)kl_vulkan_lazy_transient_usage((unsigned)ci->usage);
    if (fixed == ci->usage) return 0;
    static int said;
    if (!said++) {
        char u[128];
        VKI("transient attachment created with usage %s — dropping "
            "INPUT_ATTACHMENT (0x%x -> 0x%x) so MoltenVK will still offer the "
            "LAZILY_ALLOCATED type. It strips the memoryless types from any "
            "image carrying both bits, and Unity sets INPUT_ATTACHMENT on every "
            "URP attachment whether a subpass reads one or not. "
            "KL_VK_LAZY_KEEP_INPUT=1 puts it back.\n",
            usage_words(ci->usage, u, sizeof u),
            (unsigned)ci->usage, (unsigned)fixed);
    }
    *out = fixed;
    return 1;
}

static void klvk_note_reqs(VkImage img, VkMemoryRequirements *mr) {
    if (!mr) return;
    gimg_note_bytes(img, (uint64_t)mr->size);
    if (!lazy_transient_on() || !g_lazy_mask) return;
    if (!(gimg_usage_of(img) & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)) return;
    uint32_t want = mr->memoryTypeBits & g_lazy_mask;
    if (!want) {
        static int said_no;
        if (!said_no++)
            VKI("a transient attachment's memoryTypeBits (0x%x) carry no "
                "LAZILY_ALLOCATED type — it cannot be made memoryless\n",
                mr->memoryTypeBits);
        return;
    }
    static int said_yes;
    if (!said_yes++)
        VKI("transient attachments steered to the memoryless type "
            "(memoryTypeBits 0x%x -> 0x%x). KL_VK_LAZY_TRANSIENT=0 turns this "
            "off; a transient image is forbidden from being a transfer source "
            "or destination, which is what makes the steer safe\n",
            mr->memoryTypeBits, want);
    mr->memoryTypeBits = want;
}

static void klvk_GetImageMemoryRequirements(VkDevice dev, VkImage img,
                                            VkMemoryRequirements *out) {
    klvk_device *d = dev_find(dev);
    PFN_vkGetImageMemoryRequirements real = d && d->GetImageMemoryRequirements
        ? d->GetImageMemoryRequirements
        : (PFN_vkGetImageMemoryRequirements)(real_gdpa
              ? real_gdpa(dev, "vkGetImageMemoryRequirements") : NULL);
    if (!real) return;
    real(dev, img, out);
    klvk_note_reqs(img, out);
}

static void klvk_GetImageMemoryRequirements2(VkDevice dev,
        const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *out) {
    PFN_vkGetImageMemoryRequirements2 real = (PFN_vkGetImageMemoryRequirements2)
        (real_gdpa ? real_gdpa(dev, "vkGetImageMemoryRequirements2") : NULL);
    if (!real) real = (PFN_vkGetImageMemoryRequirements2)
        (real_gdpa ? real_gdpa(dev, "vkGetImageMemoryRequirements2KHR") : NULL);
    if (!real) return;
    real(dev, info, out);
    if (info && out) klvk_note_reqs(info->image, &out->memoryRequirements);
}

static VkResult klvk_BindImageMemory(VkDevice dev, VkImage img,
                                     VkDeviceMemory mem, VkDeviceSize off) {
    klvk_device *d = dev_find(dev);
    PFN_vkBindImageMemory real = d && d->BindImageMemory ? d->BindImageMemory
        : (PFN_vkBindImageMemory)(real_gdpa
              ? real_gdpa(dev, "vkBindImageMemory") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(dev, img, mem, off);
    if (r == VK_SUCCESS) gimg_note_type(img, mem);
    return r;
}

static VkResult klvk_BindImageMemory2(VkDevice dev, uint32_t n,
                                      const VkBindImageMemoryInfo *info) {
    PFN_vkBindImageMemory2 real = (PFN_vkBindImageMemory2)
        (real_gdpa ? real_gdpa(dev, "vkBindImageMemory2") : NULL);
    if (!real) real = (PFN_vkBindImageMemory2)
        (real_gdpa ? real_gdpa(dev, "vkBindImageMemory2KHR") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(dev, n, info);
    if (r == VK_SUCCESS && info)
        for (uint32_t i = 0; i < n; i++)
            gimg_note_type(info[i].image, info[i].memory);
    return r;
}

// ---------------------------------------------------------------------------
// Pipelines, and the stutter on first hover.
//
// MoltenVK compiles MSL inside vkCreateGraphicsPipelines. Every pipeline the
// guest has not built yet is a synchronous compile on whichever thread asked,
// and Unity asks the first time a material is actually DRAWN — which for a
// menu is the frame the pointer first lands on it and the hover material runs.
// Austin reports exactly that shape: "it stutters when I first point at the
// menu", consistently, on first hover.
//
// Two halves, and this is the first. Count them, time them, and name the worst
// one, so the next log says whether the stutter is a compile at all instead of
// leaving it an argument. The second half is the pipeline cache below.
static uint64_t g_pipe_ns, g_pipe_worst_ns;
static uint32_t g_pipe_n, g_pipe_calls, g_pipe_slow;
// Round F3: the Metal share of the pipeline time (kl_shc's all-thread sums
// around the call), by category; the rest is MoltenVK's own (SPIRV-Cross).
static uint64_t g_pipe_mtl_ns[KL_SHC_N];

static uint64_t klvk_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// MoltenVK runs each Metal compile on a GCD worker and waits for it, so the
// split is the difference of kl_shc's ALL-thread totals across the call — exact
// while this is the only pipeline being created, a shared figure otherwise
// (flagged "overlapped", and then not added to the totals).
static int g_pipe_inflight;
static uint32_t g_pipe_overlapped;
static void klvk_pipe_split_begin(uint64_t a[KL_SHC_N]) {
    __atomic_add_fetch(&g_pipe_inflight, 1, __ATOMIC_ACQ_REL);
    kl_shc_total_ns(a);
}
static void klvk_pipe_charge_split(uint32_t n, uint64_t ns, const uint64_t a[KL_SHC_N]) {
    uint64_t b[KL_SHC_N], m = 0, d[KL_SHC_N];
    kl_shc_total_ns(b);
    int alone = __atomic_sub_fetch(&g_pipe_inflight, 1, __ATOMIC_ACQ_REL) == 0;
    if (!alone) g_pipe_overlapped++;
    for (int i = 0; i < KL_SHC_N; i++) { d[i] = b[i] - a[i]; m += d[i]; if (alone) g_pipe_mtl_ns[i] += d[i]; }
    // Only once kl_shc is installed (KL_SHC=1): without it every split reads 0.
    if (kl_shc_clock() > 0 && ns > 8333333ull && g_pipe_slow < 400) {
        char nm[32];
        kl_shc_thread_label(nm);
        VKI("t=%.2f pipeline split: %u in %.1f ms = Metal lib %.1f + fn %.1f + pso %.1f + cps %.1f, "
            "MoltenVK/SPIRV-Cross %.1f, thread %s%s\n", kl_shc_clock(), n, (double)ns / 1e6,
            (double)d[KL_SHC_LIB] / 1e6, (double)d[KL_SHC_FN] / 1e6, (double)d[KL_SHC_RPS] / 1e6,
            (double)d[KL_SHC_CPS] / 1e6, (double)(ns > m ? ns - m : 0) / 1e6, nm,
            alone ? "" : " (overlapped with another pipeline creation: shared figures)");
    }
}

static void klvk_pipe_charge(uint32_t n, uint64_t ns) {
    g_pipe_calls++;
    g_pipe_n += n;
    g_pipe_ns += ns;
    if (ns > g_pipe_worst_ns) g_pipe_worst_ns = ns;
    // A compile that costs more than one 120 Hz frame is a visible hitch, and
    // counting them is what turns "it stutters" into a number.
    if (ns > 8333333ull) {
        g_pipe_slow++;
        if (g_pipe_slow <= 12)
            VKI("pipeline: %u pipeline(s) took %.1f ms to create — this is a "
                "SYNCHRONOUS MSL compile on the calling thread, and it is what "
                "a first-hover stutter looks like\n",
                n, (double)ns / 1e6);
    }
}

// --- The depth-state census, and why it is worth its forty lines ------------
//
// UE4 mobile can run a DEPTH PRE-PASS: lay the scene's depth down first, then
// draw the base pass with the depth test set to EQUAL. That is only correct if
// a vertex's clip-space position comes out BIT-IDENTICAL in both passes.
// Vulkan and GLES give that implicitly for one shader source; Metal does NOT —
// the position output has to be declared invariant or the compiler is free to
// fuse or reassociate the transform differently in two pipelines built from the
// same SPIR-V. Disagree by one ULP and the EQUAL test fails and the surface is
// simply NOT DRAWN. That is a per-object, view-dependent, both-eyes-identical
// disappearance with a scale-free trigger — a hair of head motion is enough to
// reshuffle the rounding — which is the shape of the flicker this port is
// chasing.
//
// So the first question is whether this guest uses EQUAL at all. This answers
// it by reading the state the guest hands us, and it is the whole of task one:
// no EQUAL anywhere and the hypothesis is dead without another line of code.
//
// It is provably observation-only: it runs BEFORE the real call, reads the
// caller's own structs, writes only its own file-static counters, and produces
// no value anything reads. Delete it and no pixel moves.
//
// ITS FAILURE MODES, stated so nobody reads a zero as an answer:
//   - pDepthStencilState is allowed to be NULL (no depth attachment in the
//     subpass, or rasterizer discard). Counted separately, never as "not EQUAL".
//   - VK_EXT_extended_dynamic_state lets depthCompareOp be set per-draw with
//     vkCmdSetDepthCompareOp, which would make the pipeline's value a lie. The
//     census looks for that dynamic state and says so loudly if it is present.
//     UE4 4.27 does not use it, but "does not" is not "cannot".
//   - It counts PIPELINES, not draws. A single EQUAL pipeline used for the
//     whole base pass and a thousand never bound both read as one.
static uint32_t g_depth_op[8];        // indexed by VkCompareOp, NEVER..ALWAYS
static uint32_t g_depth_no_state;     // pDepthStencilState == NULL
static uint32_t g_depth_test_off;     // depthTestEnable == VK_FALSE
static uint32_t g_depth_dyn_op;       // depthCompareOp is set dynamically
static uint32_t g_depth_equal_writes; // EQUAL *and* writing depth

// VK_DYNAMIC_STATE_DEPTH_COMPARE_OP / _DEPTH_TEST_ENABLE. Spelled numerically
// because the vendored headers predate VK_EXT_extended_dynamic_state on some
// build hosts, and a census that fails to compile is worth nothing.
#define KLVK_DYN_DEPTH_COMPARE_OP  1000267009
#define KLVK_DYN_DEPTH_TEST_ENABLE 1000267006

// Pipelines that turn the depth-BOUNDS test on (round N12-B): MoltenVK offers
// the feature on the headset and not on the Mac, so a guest that uses it runs
// a test there that no host run ever executes.
static uint32_t g_depth_bounds_on, g_depth_bounds_dyn;
static float g_depth_bounds_min = 2.f, g_depth_bounds_max = -1.f;

static void klvk_depth_note(const VkGraphicsPipelineCreateInfo *p) {
    if (!p) return;
    if (p->pDepthStencilState && p->pDepthStencilState->depthBoundsTestEnable) {
        const VkPipelineDepthStencilStateCreateInfo *b = p->pDepthStencilState;
        g_depth_bounds_on++;
        if (b->minDepthBounds < g_depth_bounds_min) g_depth_bounds_min = b->minDepthBounds;
        if (b->maxDepthBounds > g_depth_bounds_max) g_depth_bounds_max = b->maxDepthBounds;
        if (p->pDynamicState && p->pDynamicState->pDynamicStates)
            for (uint32_t j = 0; j < p->pDynamicState->dynamicStateCount; j++)
                if (p->pDynamicState->pDynamicStates[j] == VK_DYNAMIC_STATE_DEPTH_BOUNDS)
                    g_depth_bounds_dyn++;
    }
    if (p->pDynamicState && p->pDynamicState->pDynamicStates) {
        for (uint32_t j = 0; j < p->pDynamicState->dynamicStateCount; j++) {
            uint32_t s = (uint32_t)p->pDynamicState->pDynamicStates[j];
            if (s == KLVK_DYN_DEPTH_COMPARE_OP || s == KLVK_DYN_DEPTH_TEST_ENABLE)
                g_depth_dyn_op++;
        }
    }
    const VkPipelineDepthStencilStateCreateInfo *d = p->pDepthStencilState;
    if (!d) { g_depth_no_state++; return; }
    if (!d->depthTestEnable) { g_depth_test_off++; return; }
    uint32_t op = (uint32_t)d->depthCompareOp;
    if (op < 8) g_depth_op[op]++;
    if (op == (uint32_t)VK_COMPARE_OP_EQUAL && d->depthWriteEnable)
        g_depth_equal_writes++;
}

// The depth census's own line. Called from BOTH report paths: the device's
// kl_vulkan_memory_report and the host's query_census_tick — nothing on the
// host run path calls the former, and a census that only ever prints on device
// is not an instrument. Learned the hard way twice now.
static void klvk_depth_report(FILE *f) {
    if (!f) return;
    {
        static const char *opn[8] = { "NEVER", "LESS", "EQUAL", "LEQUAL",
                                      "GREATER", "NOTEQUAL", "GEQUAL", "ALWAYS" };
        uint32_t tested = 0;
        for (int i = 0; i < 8; i++) tested += g_depth_op[i];
        if (!tested && !g_depth_no_state && !g_depth_test_off) {
            fprintf(f, "  [vk] depth census: NO GRAPHICS PIPELINE WAS EVER "
                       "CREATED THROUGH THIS LAYER — the census saw nothing and "
                       "its zeros mean nothing\n");
        } else {
            fprintf(f, "  [vk] depth census: %u pipeline(s) with the depth test ON —",
                    tested);
            for (int i = 0; i < 8; i++)
                fprintf(f, " %s %u", opn[i], g_depth_op[i]);
            fprintf(f, " | %u with the test OFF | %u with no depth state at all"
                       " | %u declared depthCompareOp/depthTestEnable DYNAMIC"
                       " | depth-BOUNDS test on %u (%u with dynamic bounds; static "
                       "bounds span %.3f..%.3f)\n",
                    g_depth_test_off, g_depth_no_state, g_depth_dyn_op,
                    g_depth_bounds_on, g_depth_bounds_dyn,
                    g_depth_bounds_on ? (double)g_depth_bounds_min : 0.0,
                    g_depth_bounds_on ? (double)g_depth_bounds_max : 0.0);
            if (g_depth_dyn_op)
                fprintf(f, "  [vk]   *** depthCompareOp is set DYNAMICALLY on %u "
                           "pipeline(s), so the counts above are NOT what the "
                           "draws used. Read them as a lower bound only.\n",
                        g_depth_dyn_op);
            if (g_depth_op[VK_COMPARE_OP_EQUAL])
                fprintf(f, "  [vk]   VERDICT: %u pipeline(s) test depth EQUAL (%u of "
                           "them also WRITE depth) — this guest runs a DEPTH "
                           "PRE-PASS, and those draws are only correct if the "
                           "vertex position is computed bit-identically in both "
                           "passes. On Metal that needs position invariance.\n",
                        g_depth_op[VK_COMPARE_OP_EQUAL], g_depth_equal_writes);
            else
                fprintf(f, "  [vk]   VERDICT: NOT ONE pipeline tests depth EQUAL. "
                           "This guest does not run an EQUAL-test depth pre-pass, "
                           "so position invariance cannot be what drops its "
                           "geometry.\n");
        }
    }

}

static VkResult klvk_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache,
        uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipeline *out) {
    PFN_vkCreateGraphicsPipelines real = (PFN_vkCreateGraphicsPipelines)
        (real_gdpa ? real_gdpa(dev, "vkCreateGraphicsPipelines") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (ci) for (uint32_t i = 0; i < n; i++) klvk_depth_note(&ci[i]);
    uint64_t sh0[KL_SHC_N];
    klvk_pipe_split_begin(sh0);
    uint64_t t0 = klvk_now_ns();
    VkResult r = real(dev, cache, n, ci, alloc, out);
    uint64_t dt = klvk_now_ns() - t0;
    klvk_pipe_charge_split(n, dt, sh0);
    klvk_pipe_charge(n, dt);
    return r;
}

static VkResult klvk_CreateComputePipelines(VkDevice dev, VkPipelineCache cache,
        uint32_t n, const VkComputePipelineCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipeline *out) {
    PFN_vkCreateComputePipelines real = (PFN_vkCreateComputePipelines)
        (real_gdpa ? real_gdpa(dev, "vkCreateComputePipelines") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t sh0[KL_SHC_N];
    klvk_pipe_split_begin(sh0);
    uint64_t t0 = klvk_now_ns();
    VkResult r = real(dev, cache, n, ci, alloc, out);
    uint64_t dt = klvk_now_ns() - t0;
    klvk_pipe_charge_split(n, dt, sh0);
    klvk_pipe_charge(n, dt);
    return r;
}

// The second half: a VkPipelineCache that survives the process.
//
// Unity creates one and never persists it on Android, because Android's own
// drivers keep a per-app shader cache behind the API and the engine has nothing
// to gain. MoltenVK has no such thing — every launch recompiles every pipeline
// from scratch, so the first-hover stutter is paid again on every single run.
// Seeding the guest's cache from a file and writing it back is exactly what a
// VkPipelineCache is for, and it is safe by construction: the header carries a
// vendor/device/driver UUID and the implementation is required to reject blob
// data that does not match it, so a MoltenVK update or a different device
// simply starts from empty.
//
// KL_VK_PIPELINE_CACHE names the file. kl_app.c points it at the app container
// beside klepton-boot.log; unset, nothing is persisted and only the timing
// above happens.
static VkPipelineCache g_pipe_cache;
static VkDevice        g_pipe_cache_dev;
static uint32_t        g_pipe_cache_saved_at;   // g_pipe_n at the last save
static uint64_t        g_pipe_cache_saved_ns;   // klvk_now_ns() at the last save
static pthread_mutex_t g_pipe_cache_save_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        g_pipe_cache_seed_bytes, g_pipe_cache_save_bytes;

static const char *pipe_cache_path(void) {
    const char *p = kl_env_str("KL_VK_PIPELINE_CACHE", "");
    return p && *p ? p : NULL;
}

static VkResult klvk_CreatePipelineCache(VkDevice dev,
        const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkPipelineCache *out) {
    PFN_vkCreatePipelineCache real = (PFN_vkCreatePipelineCache)
        (real_gdpa ? real_gdpa(dev, "vkCreatePipelineCache") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    const char *path = pipe_cache_path();
    void *blob = NULL;
    size_t blob_n = 0;
    // Only seed a cache the guest created EMPTY. One that already carries data
    // is the guest's own business and overwriting it would be a lie about what
    // it holds.
    if (path && ci && !ci->initialDataSize) {
        FILE *fp = fopen(path, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0 && sz < 256 * 1024 * 1024) {
                blob = malloc((size_t)sz);
                if (blob && fread(blob, 1, (size_t)sz, fp) == (size_t)sz)
                    blob_n = (size_t)sz;
                else { free(blob); blob = NULL; }
            }
            fclose(fp);
        }
    }
    VkPipelineCacheCreateInfo mine;
    if (blob_n) {
        mine = *ci;
        mine.initialDataSize = blob_n;
        mine.pInitialData = blob;
        ci = &mine;
    }
    VkResult r = real(dev, ci, alloc, out);
    if (r != VK_SUCCESS && blob_n) {
        // A rejected blob is the UUID check doing its job, not a failure to
        // report as one. Retry empty.
        mine.initialDataSize = 0;
        mine.pInitialData = NULL;
        r = real(dev, &mine, alloc, out);
        VKI("pipeline cache: %s was rejected (%d) — starting empty, which is "
            "what a MoltenVK or device change looks like\n", path, (int)r);
        blob_n = 0;
    }
    free(blob);
    if (r == VK_SUCCESS && out && !g_pipe_cache) {
        g_pipe_cache = *out;
        g_pipe_cache_dev = dev;
        g_pipe_cache_seed_bytes = blob_n;
        if (path)
            VKI("pipeline cache: seeded from %s with %llu KiB — every pipeline "
                "in it is one MoltenVK does not have to compile again\n",
                path, (unsigned long long)(blob_n / 1024));
    }
    return r;
}

// Written back from the memory report, which already runs on a cadence and off
// the frame path. Only when the guest has actually built something new since
// the last save: a rewrite per report of an unchanged cache is a megabyte of
// container I/O a second for nothing.
//
// ...and at most once per KL_VK_PIPELINE_CACHE_SECS (default 30). RE4's launch
// 1 (2026-10-09) built ~15 new pipelines a second for its whole two minutes of
// gameplay, so the "16 new" gate alone rewrote the WHOLE blob (5 -> 19 MB)
// every report: 1078 MB of disk writes in 159 s, a diskwrites_resource report,
// and a 19 MB vkGetPipelineCacheData serialisation a second under MoltenVK's
// cache lock while the guest's render thread was waiting in that same cache to
// compile. kl_vulkan_pipeline_cache_flush (scene -> background) writes the
// tail the throttle held back, so a quit loses nothing a backgrounding saw.
static void klvk_pipe_cache_save_locked(int force) {
    const char *path = pipe_cache_path();
    if (!path || !g_pipe_cache || !g_pipe_cache_dev) return;
    if (force) {
        if (g_pipe_n == g_pipe_cache_saved_at) return;
    } else {
        if (g_pipe_n < g_pipe_cache_saved_at + 16) return;
        static int secs = -1;
        if (secs < 0) secs = kl_env_int("KL_VK_PIPELINE_CACHE_SECS", 30);
        if (secs > 0 && g_pipe_cache_saved_ns &&
            klvk_now_ns() - g_pipe_cache_saved_ns < (uint64_t)secs * 1000000000ull)
            return;
    }
    PFN_vkGetPipelineCacheData get = (PFN_vkGetPipelineCacheData)
        (real_gdpa ? real_gdpa(g_pipe_cache_dev, "vkGetPipelineCacheData") : NULL);
    if (!get) return;
    size_t n = 0;
    if (get(g_pipe_cache_dev, g_pipe_cache, &n, NULL) != VK_SUCCESS || !n) return;
    void *buf = malloc(n);
    if (!buf) return;
    if (get(g_pipe_cache_dev, g_pipe_cache, &n, buf) == VK_SUCCESS && n) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *fp = fopen(tmp, "wb");
        if (fp) {
            size_t w = fwrite(buf, 1, n, fp);
            fclose(fp);
            if (w == n && rename(tmp, path) == 0) {
                g_pipe_cache_saved_at = g_pipe_n;
                g_pipe_cache_saved_ns = klvk_now_ns();
                g_pipe_cache_save_bytes = n;
            } else {
                remove(tmp);
            }
        }
    }
    free(buf);
}

static void klvk_pipe_cache_save(void) {
    pthread_mutex_lock(&g_pipe_cache_save_lock);
    klvk_pipe_cache_save_locked(0);
    pthread_mutex_unlock(&g_pipe_cache_save_lock);
}

void kl_vulkan_pipeline_cache_flush(void) {
    pthread_mutex_lock(&g_pipe_cache_save_lock);
    uint32_t before = g_pipe_cache_saved_at;
    klvk_pipe_cache_save_locked(1);
    if (g_pipe_cache_saved_at != before)
        VKI("pipeline cache: flushed %u pipeline(s) the save throttle held back "
            "(%llu KiB)\n", g_pipe_cache_saved_at - before,
            (unsigned long long)(g_pipe_cache_save_bytes / 1024));
    pthread_mutex_unlock(&g_pipe_cache_save_lock);
}

static void image_census(const VkImageCreateInfo *ci) {
    unsigned idx = (unsigned)ci->format < KLVK_FMTC_N ? (unsigned)ci->format
                                                      : KLVK_FMTC_N - 1;
    if (!g_img_by_fmt[idx]++) {
        g_img_fmt_seen++;
        char u[128];
        VKI("guest image #1 of format %d %s — %uD %ux%ux%u, %u mip, %u layer, "
            "%ux MSAA, tiling %s, usage %s, flags 0x%x\n",
            (int)ci->format, kl_vulkan_format_name((int)ci->format),
            (unsigned)ci->imageType + 1, ci->extent.width, ci->extent.height,
            ci->extent.depth, ci->mipLevels, ci->arrayLayers,
            (unsigned)ci->samples,
            ci->tiling == VK_IMAGE_TILING_LINEAR ? "linear" : "optimal",
            usage_words(ci->usage, u, sizeof u), (unsigned)ci->flags);
    }
}

// ---------------------------------------------------------------------------
// The PLACEMENT-HEAP IMAGE FLAGS — round 21, and the only reason R1 is
// testable at all.
// ---------------------------------------------------------------------------
//
// Round 20 proved `MVK_CONFIG_USE_MTLHEAP=0` reaches both the host and the
// visionOS MoltenVK slice and flips placement heaps off — and then the host
// run died in forty seconds of guest time. `MVKImage.mm:1395-1401`:
//
//     const auto placementHeapFlags = VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT
//                                   | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT
//                                   | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
//     if (!getMetalFeatures().placementHeaps && mvkIsAnyFlagEnabled(flags, ...))
//         reportError(VK_ERROR_FEATURE_NOT_PRESENT, "MTLHeap must be enabled ...");
//
// UE4 does not check the VkResult, hands the null VkImage to
// vkGetImageMemoryRequirements, and Metal takes the SIGSEGV. So the crash is
// not evidence against R1; it is a gate in front of it.
//
// This is the third of round 19's three gates, applied to a code change rather
// than a knob: BEFORE stripping anything, MEASURE whether the guest actually
// needs the capability those flags buy. The flags are only ever needed by a
// VIEW: a 2D or 2D-array view of a 3D image, or a view whose format differs
// from a block-texel-view-compatible image's. If the guest never creates such
// a view, the flags are decoration and stripping them changes nothing that can
// be observed; if it does, stripping them is a real behavioural change and
// this round says so and does not ship.
//
// So both halves are here and BOTH RUN REGARDLESS OF WHETHER STRIPPING IS ON:
// the census of which images carry the flags, and the cross-check at
// vkCreateImageView of whether any view needed one. Stripping itself happens
// ONLY when `MVK_CONFIG_USE_MTLHEAP` is set to 0 in the environment — the flags
// are perfectly legal with heaps on, and removing them there would be a
// gratuitous change riding along on every other launch.
#define KLVK_HEAP_FLAGS ((VkImageCreateFlags)( \
      VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT \
    | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT \
    | VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT))

// "Did the row turn placement heaps off?" Read from the environment, not from
// MoltenVK: the config is latched inside the dylib and there is no supported
// way to read it back from here, but the row that sets it is ours and
// kl_target_use applies it before MoltenVK is ever dlopened (round 20 measured
// 237 log lines of margin). An unset variable, or any value other than 0,
// means heaps are ON and nothing is stripped.
static int mtlheap_off(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MVK_CONFIG_USE_MTLHEAP");
        v = (e && *e && atoi(e) == 0) ? 1 : 0;
    }
    return v;
}

#define KLVK_HFLAG_N 512
static struct {
    VkImage           img;
    VkImageCreateFlags flags;   // the ORIGINAL flags, before any strip
    uint32_t          fmt;
    uint8_t           type;     // VkImageType
} g_hflag[KLVK_HFLAG_N];
static unsigned g_hflag_n, g_hflag_over;
static pthread_mutex_t g_hflag_lock = PTHREAD_MUTEX_INITIALIZER;

// Run-long counts, per flag, plus the strips and the views that needed one.
static uint64_t g_hf_2darr, g_hf_block, g_hf_2dview;
static uint64_t g_hf_stripped, g_hf_images;
static uint64_t g_hf_view_needed_2dof3d, g_hf_view_needed_block, g_hf_views;

static void hflag_census(const VkImageCreateInfo *ci) {
    if (ci->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)          g_hf_2darr++;
    if (ci->flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT)  g_hf_block++;
    if (ci->flags & VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT)       g_hf_2dview++;
    g_hf_images++;
    if (g_hf_images <= 8)
        VKI("placement-heap image #%llu: flags 0x%x on format %d %s — %uD "
            "%ux%ux%u, %u mip, %u layer%s\n",
            (unsigned long long)g_hf_images, (unsigned)ci->flags,
            (int)ci->format, kl_vulkan_format_name((int)ci->format),
            (unsigned)ci->imageType + 1, ci->extent.width, ci->extent.height,
            ci->extent.depth, ci->mipLevels, ci->arrayLayers,
            mtlheap_off() ? " — THE THREE HEAP FLAGS ARE BEING STRIPPED "
                            "(MVK_CONFIG_USE_MTLHEAP=0)" : "");
}

static void hflag_add(VkImage img, const VkImageCreateInfo *ci) {
    if (!img) return;
    pthread_mutex_lock(&g_hflag_lock);
    unsigned i = 0;
    for (; i < KLVK_HFLAG_N; i++) if (!g_hflag[i].img) break;
    if (i < KLVK_HFLAG_N) {
        g_hflag[i].img = img;
        g_hflag[i].flags = ci->flags;
        g_hflag[i].fmt = (uint32_t)ci->format;
        g_hflag[i].type = (uint8_t)ci->imageType;
        g_hflag_n++;
    } else {
        // A full table means a view of a later flagged image would not be
        // cross-checked. Said out loud rather than dropped, because the whole
        // value of this instrument is that its zero is trustworthy.
        g_hflag_over++;
    }
    pthread_mutex_unlock(&g_hflag_lock);
}

static void hflag_del(VkImage img) {
    if (!img || !g_hflag_n) return;
    pthread_mutex_lock(&g_hflag_lock);
    for (unsigned i = 0; i < KLVK_HFLAG_N; i++)
        if (g_hflag[i].img == img) {
            memset(&g_hflag[i], 0, sizeof g_hflag[i]);
            if (g_hflag_n) g_hflag_n--;
            break;
        }
    pthread_mutex_unlock(&g_hflag_lock);
}

// The half that can BLOCK the test. Returns nothing; it counts and it shouts.
static void hflag_check_view(const VkImageViewCreateInfo *ci) {
    if (!ci || !ci->image) return;
    VkImageCreateFlags fl = 0; uint32_t ifmt = 0; uint8_t itype = 0; int hit = 0;
    pthread_mutex_lock(&g_hflag_lock);
    for (unsigned i = 0; i < KLVK_HFLAG_N; i++)
        if (g_hflag[i].img == ci->image) {
            fl = g_hflag[i].flags; ifmt = g_hflag[i].fmt;
            itype = g_hflag[i].type; hit = 1; break;
        }
    pthread_mutex_unlock(&g_hflag_lock);
    if (!hit) return;
    g_hf_views++;
    int flat = (ci->viewType == VK_IMAGE_VIEW_TYPE_2D ||
                ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    if (itype == VK_IMAGE_TYPE_3D && flat) {
        if (!g_hf_view_needed_2dof3d++)
            VKI("*** THE GUEST CREATED A 2D VIEW OF A 3D IMAGE (view type %d, "
                "image format %d %s). That view REQUIRES the 2D-view/2D-array "
                "flags, so stripping them is NOT harmless and "
                "MVK_CONFIG_USE_MTLHEAP=0 CANNOT BE TESTED THIS WAY. ***\n",
                (int)ci->viewType, (int)ifmt, kl_vulkan_format_name((int)ifmt));
    }
    if ((fl & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) &&
        (uint32_t)ci->format != ifmt) {
        if (!g_hf_view_needed_block++)
            VKI("*** THE GUEST CREATED A VIEW OF FORMAT %d %s ON A BLOCK-TEXEL-"
               "VIEW-COMPATIBLE IMAGE OF FORMAT %d %s. That view REQUIRES the "
               "block-texel flag, so stripping it is NOT harmless. ***\n",
               (int)ci->format, kl_vulkan_format_name((int)ci->format),
               (int)ifmt, kl_vulkan_format_name((int)ifmt));
    }
}

static void hflag_report(FILE *f) {
    if (!f) return;
    fprintf(f, "  [vk] placement-heap image flags: %llu image(s) carried one "
               "(2D-array %llu, block-texel %llu, 2D-view %llu) | heaps %s, "
               "%llu image(s) STRIPPED | %llu view(s) on them, %llu needed "
               "2D-of-3D, %llu needed block-texel%s\n",
            (unsigned long long)g_hf_images, (unsigned long long)g_hf_2darr,
            (unsigned long long)g_hf_block, (unsigned long long)g_hf_2dview,
            mtlheap_off() ? "OFF (MVK_CONFIG_USE_MTLHEAP=0)" : "on (default)",
            (unsigned long long)g_hf_stripped, (unsigned long long)g_hf_views,
            (unsigned long long)g_hf_view_needed_2dof3d,
            (unsigned long long)g_hf_view_needed_block,
            g_hflag_over ? " | *** THE FLAGGED-IMAGE TABLE OVERFLOWED, so the "
                           "view counts are a FLOOR ***" : "");
}

// The device's own image-size limits, read once per physical device. An image
// beyond them is invalid usage that MoltenVK passes straight to Metal, whose
// texture-descriptor validation ABORTS the process (SIGABRT, no VkResult) —
// the visionOS Simulator validates 2D textures at 8192 and Alyx loads an
// 8194x4097 texture (round 5). The create is named before it goes on;
// nothing is resized.
static VkPhysicalDeviceLimits *dev_limits(VkPhysicalDevice phys) {
    static struct { VkPhysicalDevice p; VkPhysicalDeviceLimits l; } cache[4];
    static int n;
    for (int i = 0; i < n; i++) if (cache[i].p == phys) return &cache[i].l;
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)
        (real_gipa ? real_gipa(g_instance, "vkGetPhysicalDeviceProperties") : NULL);
    if (!gp || !phys || n >= 4) return NULL;
    VkPhysicalDeviceProperties pr;
    memset(&pr, 0, sizeof pr);
    gp(phys, &pr);
    cache[n].p = phys;
    cache[n].l = pr.limits;
    // KL_VK_LIMIT_2D=<px>: this file's own idea of the 2D limit (the guest is
    // told nothing different) — how a host run exercises the Simulator's 8192
    // clamp below without a Simulator.
    int l2d = kl_env_int("KL_VK_LIMIT_2D", 0);
    if (l2d > 0) cache[n].l.maxImageDimension2D = (uint32_t)l2d;
    return &cache[n++].l;
}
static int image_over_limits(klvk_device *d, const VkImageCreateInfo *ci) {
    const VkPhysicalDeviceLimits *l = d ? dev_limits(d->phys) : NULL;
    if (!l) return 0;
    uint32_t m = ci->imageType == VK_IMAGE_TYPE_1D ? l->maxImageDimension1D
               : ci->imageType == VK_IMAGE_TYPE_3D ? l->maxImageDimension3D
               : (ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? l->maxImageDimensionCube
               : l->maxImageDimension2D;
    if (!m || (ci->extent.width <= m && ci->extent.height <= m &&
               (ci->imageType != VK_IMAGE_TYPE_3D || ci->extent.depth <= m)))
        return 0;
    static unsigned said;
    if (said++ < 8)
        VKI("vkCreateImage %ux%ux%u (format %d, type %d) EXCEEDS this device's limit %u — "
            "clamped below if it can be, else passed through (Metal may abort here)\n",
            ci->extent.width, ci->extent.height, ci->extent.depth, (int)ci->format,
            (int)ci->imageType, m);
    return 1;
}

// ---- over-limit 2D images, clamped (the visionOS Simulator's 8192) ------
//
// The Simulator's Metal caps a 2D texture at 8192 a side (the Mac and the
// headset take 16384) and aborts the process on anything larger. A guest that
// ignores maxImageDimension2D — Alyx loads an 8194x4097 SRGB texture resource
// for its startup scene, one non-mipped vtex, independent of render size and
// of r_texture_stream_max_resolution — cannot be refused either (round 5: it
// does not check the VkResult and faults on the NULL image). So an over-limit
// 2D image of an UNCOMPRESSED format is created clamped to the limit, and
// every buffer->image upload into it is clipped to what exists (with its row
// pitch pinned to the region it came from, so rows stay aligned). The loss is
// the texels past the limit — for Alyx 2 columns and 1 row. Compressed formats
// and other image types are left as they were (named). Device and Mac never
// clamp: nothing there is over their limit. KL_VK_CLAMP_OVERSIZE=0 disables.
#define KLVK_CLAMP_N 16
typedef struct { VkImage img; uint32_t w, h, cw, ch; } klvk_clamp;
static klvk_clamp g_clamp[KLVK_CLAMP_N];
static pthread_mutex_t g_clamp_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_clamp_clipped;

static uint32_t image_limit_2d(klvk_device *d, const VkImageCreateInfo *ci) {
    const VkPhysicalDeviceLimits *l = d ? dev_limits(d->phys) : NULL;
    if (!l || ci->imageType != VK_IMAGE_TYPE_2D) return 0;
    return (ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? l->maxImageDimensionCube
                                                             : l->maxImageDimension2D;
}

static int fmt_block_1x1(VkFormat f) {
    int v = (int)f;
    if (v >= 131 && v <= 184) return 0;                    // BC, ETC2, EAC, ASTC
    if (v >= 1000054000 && v <= 1000054007) return 0;      // PVRTC
    if (v >= 1000066000 && v <= 1000066013) return 0;      // ASTC HDR
    if (v >= 1000156000 && v <= 1000156033) return 0;      // multi-planar YCbCr
    return v > 0;
}

static int clamp_find(VkImage img, klvk_clamp *outc) {
    if (!img) return 0;
    int hit = 0;
    pthread_mutex_lock(&g_clamp_lock);
    for (unsigned i = 0; i < KLVK_CLAMP_N; i++)
        if (g_clamp[i].img == img) { if (outc) *outc = g_clamp[i]; hit = 1; break; }
    pthread_mutex_unlock(&g_clamp_lock);
    return hit;
}

static void clamp_del(VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&g_clamp_lock);
    for (unsigned i = 0; i < KLVK_CLAMP_N; i++)
        if (g_clamp[i].img == img) memset(&g_clamp[i], 0, sizeof g_clamp[i]);
    pthread_mutex_unlock(&g_clamp_lock);
}

// Clip one region to the clamped image. Returns 0 if nothing of it survives.
static int clamp_clip(const klvk_clamp *c, uint32_t mip, VkOffset3D *off, VkExtent3D *ext,
                      uint32_t *row_len, uint32_t *img_h) {
    uint32_t cw = c->cw >> mip, ch = c->ch >> mip;
    if (!cw) cw = 1;
    if (!ch) ch = 1;
    if (!*row_len) *row_len = ext->width;      // tightly packed, as the guest meant it
    if (!*img_h)   *img_h   = ext->height;
    if (off->x < 0 || off->y < 0) return 1;    // not ours to reason about
    if ((uint32_t)off->x >= cw || (uint32_t)off->y >= ch) return 0;
    uint32_t w = ext->width, h = ext->height;
    if ((uint32_t)off->x + w > cw) w = cw - (uint32_t)off->x;
    if ((uint32_t)off->y + h > ch) h = ch - (uint32_t)off->y;
    if ((w != ext->width || h != ext->height) && !g_clamp_clipped++)
        VKI("upload into a clamped %ux%u image clipped: %ux%u at %d,%d -> %ux%u "
            "(row pitch %u kept)\n", c->w, c->h, ext->width, ext->height,
            off->x, off->y, w, h, *row_len);
    ext->width = w; ext->height = h;
    return 1;
}

static VkResult klvk_CreateImage_(VkDevice dev, const VkImageCreateInfo *ci,
                                  const VkAllocationCallbacks *alloc, VkImage *out) {
    klvk_device *d = dev_find(dev);
    PFN_vkCreateImage real = d && d->CreateImage ? d->CreateImage
        : (PFN_vkCreateImage)(real_gdpa ? real_gdpa(dev, "vkCreateImage") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (!ci || !out) return real(dev, ci, alloc, out);
    // Named, not refused: a refusal was tried (round 5) and Alyx does not
    // check the VkResult — it asked the NULL image for its memory
    // requirements. A line naming the image beats either failure unnamed.
    VkImageCreateInfo clamped;
    uint32_t clamp_w = ci->extent.width, clamp_h = ci->extent.height;
    const uint32_t orig_w_of_clamp = ci->extent.width, orig_h_of_clamp = ci->extent.height;
    if (image_over_limits(d, ci) && kl_env_on("KL_VK_CLAMP_OVERSIZE", 1)) {
        uint32_t m = image_limit_2d(d, ci);
        // UNDEFINED too: the format seam below substitutes it by usage with an
        // uncompressed format (and replaces its uploads with a clear), so the
        // clamp is as safe as for any 1x1-block format. The Simulator stopped
        // on Alyx's 16384x4 UNDEFINED image inside a level (round 7).
        if (m && (fmt_block_1x1(ci->format) || ci->format == VK_FORMAT_UNDEFINED)) {
            clamped = *ci;
            if (clamped.extent.width > m)  clamped.extent.width = m;
            if (clamped.extent.height > m) clamped.extent.height = m;
            uint32_t mx = clamped.extent.width > clamped.extent.height
                        ? clamped.extent.width : clamped.extent.height;
            uint32_t lv = 1;
            while (mx > 1) { mx >>= 1; lv++; }
            if (clamped.mipLevels > lv) clamped.mipLevels = lv;
            clamp_w = clamped.extent.width; clamp_h = clamped.extent.height;
            VKI("vkCreateImage %ux%u (format %d) CLAMPED to %ux%u for this device's "
                "limit %u — uploads into it are clipped (KL_VK_CLAMP_OVERSIZE=0 "
                "passes it through)\n", ci->extent.width, ci->extent.height,
                (int)ci->format, clamp_w, clamp_h, m);
            ci = &clamped;
        }
    }
    const uint32_t over_w = (ci == &clamped) ? clamped.extent.width : 0;

    const VkImageCreateInfo orig_ci = *ci;
    const VkImageCreateFlags orig_flags = ci->flags;

    image_census(ci);

    // Round 21. The three placement-heap flags: censused always, stripped only
    // when the row turned heaps off. `ci` is repointed at a local copy so both
    // exits below — the plain one and the format-substituting one — forward the
    // stripped flags; `orig_flags` keeps what the guest asked for so the view
    // cross-check still knows.
    VkImageCreateInfo heapfix;
    if (ci->flags & KLVK_HEAP_FLAGS) {
        hflag_census(ci);
        if (mtlheap_off()) {
            heapfix = *ci;
            heapfix.flags &= ~KLVK_HEAP_FLAGS;
            if (!g_hf_stripped++)
                VKI("FIRST STRIP: MVK_CONFIG_USE_MTLHEAP=0 is set, so the three "
                    "placement-heap creation flags are being removed at "
                    "vkCreateImage — without this MVKImage.mm:1395 refuses the "
                    "image with VK_ERROR_FEATURE_NOT_PRESENT and UE4, which "
                    "does not check the VkResult, segfaults on the null "
                    "handle\n");
            ci = &heapfix;
        }
    }

    if (!d || !fmt_unmappable(d, ci->format)) {
        // The format is fine; the USAGE may still be what keeps this image off
        // memoryless storage. Nothing else about the create-info is touched.
        VkImageCreateInfo lazy;
        VkImageUsageFlags fixed;
        const VkImageCreateInfo *pass = ci;
        if (lazy_fix_usage(ci, &fixed)) {
            lazy = *ci;
            lazy.usage = fixed;
            pass = &lazy;
        }
        VkResult pr = real(dev, pass, alloc, out);
        if (pr == VK_SUCCESS && out && over_w) {
            pthread_mutex_lock(&g_clamp_lock);
            for (unsigned i = 0; i < KLVK_CLAMP_N; i++) {
                if (g_clamp[i].img) continue;
                g_clamp[i] = (klvk_clamp){ *out, orig_w_of_clamp, orig_h_of_clamp,
                                           clamp_w, clamp_h };
                break;
            }
            pthread_mutex_unlock(&g_clamp_lock);
        }
        // The by-BYTES ledger wants every guest image, not only the
        // substituted ones — launch #13's three-allocation spike was in
        // formats Metal spells perfectly well.
        if (pr == VK_SUCCESS && out) {
            gimg_add(*out, ci, (uint32_t)ci->format);
            if (orig_flags & KLVK_HEAP_FLAGS) hflag_add(*out, &orig_ci);
        }
        return pr;
    }

    int differs = 0;
    int sub = kl_vulkan_format_substitute((int)ci->format, &differs);
    if (!sub) {
        // No family answer. Before round 13 this refused with
        // VK_ERROR_FORMAT_NOT_SUPPORTED — legal, and the guest still died: the
        // punch-time UNDEFINED image came back NULL and libunity.so
        // dereferenced it at +0xbf19ec with x21 = x22 = 0. A legal error the
        // caller never checks is a crash with better paperwork, so the last
        // resort answers by USAGE instead. The primary fix is upstream of here
        // — the capability answers now report the depth family SUPPORTED, so
        // Unity should never produce an UNDEFINED at all — and this is the net
        // under it.
        sub = kl_vulkan_format_last_resort((int)ci->format, (unsigned)ci->usage,
                                           &differs);
        if (!sub) {
            static int said;
            if (said++ < 8)
                VKI("format %d %s has no Metal mapping, no substitute and no "
                    "usage the last resort can serve (usage 0x%x) — refusing "
                    "the image with VK_ERROR_FORMAT_NOT_SUPPORTED rather than "
                    "letting MoltenVK build a descriptor with pixelFormat 0\n",
                    (int)ci->format, kl_vulkan_format_name((int)ci->format),
                    (unsigned)ci->usage);
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        static int said_lr[8];
        char lu[128];
        unsigned li = (unsigned)ci->format < 8 ? (unsigned)ci->format : 7;
        if (!said_lr[li]++)
            VKI("format %d %s has no family to widen inside — LAST RESORT by "
                "usage (%s) picks %d %s. An UNDEFINED here means the guest's own "
                "format table answered NONE; the capability seam is what should "
                "have stopped that, and this line means it did not.\n",
                (int)ci->format, kl_vulkan_format_name((int)ci->format),
                usage_words(ci->usage, lu, sizeof lu),
                sub, kl_vulkan_format_name(sub));
    }

    VkImageCreateInfo mine = *ci;
    mine.format = (VkFormat)sub;
    VkImageUsageFlags lazy_usage;
    if (lazy_fix_usage(ci, &lazy_usage)) mine.usage = lazy_usage;

    // The view-format list, if the guest hung one off pNext — Unity does, and
    // it holds the sRGB/linear pair of the ORIGINAL format. Left as it stands
    // it would name formats incompatible with the image we are about to make.
    VkImageFormatListCreateInfo mylist;
    VkFormat myfmts[8];
    const VkBaseInStructure *head = (const VkBaseInStructure *)ci->pNext;
    const VkBaseInStructure *fl = NULL;
    for (const VkBaseInStructure *n = head; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) { fl = n; break; }
    if (fl && fl == head) {
        const VkImageFormatListCreateInfo *src = (const VkImageFormatListCreateInfo *)fl;
        mylist = *src;
        uint32_t n = src->viewFormatCount;
        if (n && n <= (uint32_t)(sizeof myfmts / sizeof *myfmts) && src->pViewFormats) {
            for (uint32_t i = 0; i < n; i++) {
                int vd = 0;
                int vs = kl_vulkan_format_substitute((int)src->pViewFormats[i], &vd);
                myfmts[i] = (vs && fmt_unmappable(d, src->pViewFormats[i]))
                          ? (VkFormat)vs : src->pViewFormats[i];
            }
            mylist.pViewFormats = myfmts;
        } else {
            // A count of zero is legal and means "no restriction", which is the
            // safe answer when the list is bigger than this frame can hold.
            mylist.viewFormatCount = 0;
            mylist.pViewFormats = NULL;
        }
        mine.pNext = &mylist;
    } else if (fl) {
        // Deeper in the chain. Relinking would mean copying structures whose
        // sizes are not knowable from the sType alone, so the chain is dropped
        // whole and said out loud — a silently rewritten pNext is worse than a
        // loud one.
        VKI("format %d substituted, but its VkImageFormatListCreateInfo is not "
            "at the head of pNext — the whole chain was dropped\n", (int)ci->format);
        mine.pNext = NULL;
    }

    VkResult r = real(dev, &mine, alloc, out);
    if (r != VK_SUCCESS) {
        VKI("format %d %s -> %d %s: vkCreateImage still failed %d\n",
            (int)ci->format, kl_vulkan_format_name((int)ci->format),
            sub, kl_vulkan_format_name(sub), r);
        return r;
    }

    static int said_for[KLVK_FMTC_N];
    unsigned fi = (unsigned)ci->format < KLVK_FMTC_N ? (unsigned)ci->format
                                                     : KLVK_FMTC_N - 1;
    if (!said_for[fi]++)
        VKI("format %d %s has no MTLPixelFormat on this device — substituted %d "
            "%s (%s). This is the launch-#11 abort's class.\n",
            (int)ci->format, kl_vulkan_format_name((int)ci->format),
            sub, kl_vulkan_format_name(sub),
            differs ? "TEXEL SIZE DIFFERS — uploads into it are replaced with a "
                      "clear until the repack lands, so it will read flat grey"
                    : "same texel size, uploads pass through unchanged");

    gimg_add(*out, ci, (uint32_t)sub);
    if (orig_flags & KLVK_HEAP_FLAGS) hflag_add(*out, &orig_ci);

    pthread_mutex_lock(&g_subst_lock);
    unsigned i = 0;
    for (; i < KLVK_SUBST_N; i++) if (!g_subst[i].img) break;
    if (i < KLVK_SUBST_N) {
        g_subst[i].img = *out;
        g_subst[i].orig = (uint32_t)ci->format;
        g_subst[i].sub = (uint32_t)sub;
        g_subst[i].mips = ci->mipLevels ? ci->mipLevels : 1;
        g_subst[i].layers = ci->arrayLayers ? ci->arrayLayers : 1;
        g_subst[i].differs = (uint8_t)differs;
        g_subst_n++;
    } else {
        g_subst_overflow++;   // its views will be created with the wrong format
    }
    pthread_mutex_unlock(&g_subst_lock);
    return r;
}

static void hold_born(uint64_t obj);   // the hold census, below
static VkResult klvk_CreateImage(VkDevice dev, const VkImageCreateInfo *ci,
                                 const VkAllocationCallbacks *alloc, VkImage *out) {
    VkResult r = klvk_CreateImage_(dev, ci, alloc, out);
    if (r == VK_SUCCESS && out) hold_born((uint64_t)(uintptr_t)*out);
    return r;
}

static void view_add(VkImageView v, VkImage img,
                     const VkImageSubresourceRange *r);   // the rendering census, below
static void views_image_gone(VkImage img);
static void hold_killed(VkImageView view, VkImage img, int kind);   // the hold census
static void hold_check_submit(const VkCommandBuffer *cbs, uint32_t n);
static void cbh_note_views(VkCommandBuffer cb, const VkImageView *v, uint32_t n);
static void cbh_note_objs(VkCommandBuffer cb, const uint64_t *v, uint32_t n);
static void hold_killed_image(VkImage img);
static void cbh_begin(VkCommandBuffer cb);
static void hold_mark_submit(const VkCommandBuffer *cbs, uint32_t n, int on);
static void hold_submit_delay(void);
static int hold_on(void);

static void klvk_DestroyImage(VkDevice dev, VkImage img,
                              const VkAllocationCallbacks *alloc) {
    klvk_device *d = dev_find(dev);
    PFN_vkDestroyImage real = d && d->DestroyImage ? d->DestroyImage
        : (PFN_vkDestroyImage)(real_gdpa ? real_gdpa(dev, "vkDestroyImage") : NULL);
    if (img) {
        fove_image_gone(img);
        fove_xr_image_gone(img);
        views_image_gone(img);
        hold_killed_image(img);
        gimg_del(img);
        hflag_del(img);
        clamp_del(img);
        pthread_mutex_lock(&g_subst_lock);
        for (unsigned i = 0; i < KLVK_SUBST_N; i++) {
            if (g_subst[i].img != img) continue;
            memset(&g_subst[i], 0, sizeof g_subst[i]);
            if (g_subst_n) g_subst_n--;
            break;
        }
        pthread_mutex_unlock(&g_subst_lock);
    }
    if (real) real(dev, img, alloc);
}

// A view's format has to be compatible with its image's, so a substituted
// image needs a substituted view. Two cases and both are handled: the guest
// asks with the original format (substitute it the same way), or it asks with
// something else entirely on a substituted image (take the image's format,
// which is the only one that can be right).
static VkResult klvk_CreateImageView_(VkDevice dev, const VkImageViewCreateInfo *ci,
                                      const VkAllocationCallbacks *alloc,
                                      VkImageView *out) {
    klvk_device *d = dev_find(dev);
    PFN_vkCreateImageView real =
        (PFN_vkCreateImageView)(real_gdpa ? real_gdpa(dev, "vkCreateImageView") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (!ci) return real(dev, ci, alloc, out);

    // Round 21: does this view NEED one of the placement-heap flags? Asked of
    // every view, before the format-substitution early-out below, because the
    // answer is what decides whether stripping them is harmless.
    hflag_check_view(ci);

    uint32_t isub = 0;
    if (!subst_find(ci->image, &isub, NULL, NULL, NULL))
        return real(dev, ci, alloc, out);

    VkImageViewCreateInfo mine = *ci;
    int vd = 0;
    int vs = kl_vulkan_format_substitute((int)ci->format, &vd);
    if (vs && d && fmt_unmappable(d, ci->format)) mine.format = (VkFormat)vs;
    else if (ci->format != (VkFormat)isub && d && fmt_unmappable(d, ci->format))
        mine.format = (VkFormat)isub;
    return real(dev, &mine, alloc, out);
}

static VkResult klvk_CreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci,
                                     const VkAllocationCallbacks *alloc,
                                     VkImageView *out) {
    VkResult r = klvk_CreateImageView_(dev, ci, alloc, out);
    if (r == VK_SUCCESS && ci && out) view_add(*out, ci->image, &ci->subresourceRange);
    if (r == VK_SUCCESS && out) hold_born((uint64_t)(uintptr_t)*out);
    return r;
}

// The upload path, and the honest limit named above. For a substitution that
// kept the texel size this is a plain pass-through. For one that did not, the
// guest's staging buffer holds fewer bytes per texel than the image now wants,
// and letting the copy run would read past the end of it — so the copy is
// replaced with a clear of the same subresources. Flat grey rather than
// undefined memory, because "the decals came out grey" is a bug report and
// "the decals came out different every run" is three.
static int copy_needs_clear(VkImage img, uint32_t *mips, uint32_t *layers) {
    uint8_t differs = 0;
    if (!subst_find(img, NULL, mips, layers, &differs)) return 0;
    return differs != 0;
}

static void subst_clear_instead(VkCommandBuffer cb, VkImage img, VkImageLayout layout,
                                uint32_t mips, uint32_t layers) {
    klvk_device *d = g_devs[0];
    if (!d || !d->CmdClearColorImage) return;
    VkClearColorValue c;
    memset(&c, 0, sizeof c);
    c.float32[0] = c.float32[1] = c.float32[2] = 0.5f;
    c.float32[3] = 1.0f;
    VkImageSubresourceRange rr;
    memset(&rr, 0, sizeof rr);
    rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rr.levelCount = mips ? mips : 1;
    rr.layerCount = layers ? layers : 1;
    d->CmdClearColorImage(cb, img, layout, &c, 1, &rr);
    if (!(g_subst_skipped_copies++ % 256))
        VKI("%u buffer->image upload(s) into a widened substitute replaced with "
            "a clear — those textures read flat grey (see the format seam)\n",
            g_subst_skipped_copies);
}

static void klvk_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst,
                                      VkImageLayout layout, uint32_t n,
                                      const VkBufferImageCopy *regions) {
    PFN_vkCmdCopyBufferToImage real = g_devs[0] && real_gdpa
        ? (PFN_vkCmdCopyBufferToImage)real_gdpa(g_devs[0]->dev, "vkCmdCopyBufferToImage")
        : NULL;
    uint32_t mips = 1, layers = 1;
    { uint64_t o = (uint64_t)(uintptr_t)dst; cbh_note_objs(cb, &o, 1); }
    if (copy_needs_clear(dst, &mips, &layers)) {
        subst_clear_instead(cb, dst, layout, mips, layers);
        return;
    }
    klvk_clamp c;
    if (real && n && regions && n <= 64 && clamp_find(dst, &c)) {
        VkBufferImageCopy r[64];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            r[k] = regions[i];
            if (clamp_clip(&c, r[k].imageSubresource.mipLevel, &r[k].imageOffset,
                           &r[k].imageExtent, &r[k].bufferRowLength,
                           &r[k].bufferImageHeight)) k++;
        }
        if (k) real(cb, src, dst, layout, k, r);
        return;
    }
    if (real) real(cb, src, dst, layout, n, regions);
}

static void klvk_CmdCopyBufferToImage2(VkCommandBuffer cb,
                                       const VkCopyBufferToImageInfo2 *info) {
    PFN_vkCmdCopyBufferToImage2 real = g_devs[0] && real_gdpa
        ? (PFN_vkCmdCopyBufferToImage2)real_gdpa(g_devs[0]->dev, "vkCmdCopyBufferToImage2")
        : NULL;
    if (!real && g_devs[0] && real_gdpa)
        real = (PFN_vkCmdCopyBufferToImage2)real_gdpa(g_devs[0]->dev,
                                                     "vkCmdCopyBufferToImage2KHR");
    uint32_t mips = 1, layers = 1;
    if (info && copy_needs_clear(info->dstImage, &mips, &layers)) {
        subst_clear_instead(cb, info->dstImage, info->dstImageLayout, mips, layers);
        return;
    }
    klvk_clamp c;
    if (real && info && info->regionCount && info->pRegions && info->regionCount <= 64 &&
        clamp_find(info->dstImage, &c)) {
        VkBufferImageCopy2 r[64];
        uint32_t k = 0;
        for (uint32_t i = 0; i < info->regionCount; i++) {
            r[k] = info->pRegions[i];
            if (clamp_clip(&c, r[k].imageSubresource.mipLevel, &r[k].imageOffset,
                           &r[k].imageExtent, &r[k].bufferRowLength,
                           &r[k].bufferImageHeight)) k++;
        }
        VkCopyBufferToImageInfo2 mine = *info;
        mine.regionCount = k;
        mine.pRegions = r;
        if (k) real(cb, &mine);
        return;
    }
    if (real) real(cb, info);
}

#include "../kl_fault.h"

// ---------------------------------------------------------------------------
// Forward-declared for the index guard below: it owns the vkCmdDrawIndexed
// name, so it is the one that charges the draw census for that kind. The
// census itself lives after the guard, next to the rest of the draw family.
static int drawc_on(void);
enum { KLVK_DK_DRAW = 0, KLVK_DK_IDX, KLVK_DK_INDIRECT, KLVK_DK_IDX_INDIRECT,
       KLVK_DK_INDIRECT_COUNT, KLVK_DK_IDX_INDIRECT_COUNT, KLVK_DK_EXEC,
       KLVK_DK_N };
static void klvk_draw_note(int kind);
// Round 19's geometry census — the per-frame INDEX TOTAL, forward-declared
// here for the same reason klvk_draw_note is: the index guard owns the
// vkCmdDrawIndexed name, so the charge has to happen from inside it.
static void klvk_geom_note(uint64_t elems, int indexed);
static void klvk_drawhist_note(uint32_t idx_n, uint32_t inst_n);
// Round 21's DRAW DIFF — the per-draw fingerprint multiset. Forward-declared
// for the same reason: the index guard owns the vkCmdDrawIndexed name.
static int  ddiff_on(void);
static void klvk_ddiff_note(uint32_t idx_n, uint32_t inst_n, uint32_t first_idx,
                            int32_t vtx_off, uint32_t first_inst, int indexed,
                            const void *pipe, const void *ibuf, uint64_t ioff);
static uint64_t g_geom_dropped;      // indices the index guard dropped

// The index-buffer guard.
//
// A `vkCmdDrawIndexed` recorded with no index buffer bound in that command
// buffer is invalid usage (VUID-vkCmdDrawIndexed-None-07312). Mobile drivers
// tolerate it — the draw comes out as garbage or as nothing and the app lives,
// which is why a shipped Android title can carry one and never know. MoltenVK
// does not: it records MVKCmdDrawIndexed at build time and reads the binding
// only at vkQueueSubmit, where a nil MTLBuffer goes straight into
// -[MTLRenderCommandEncoder drawIndexedPrimitives:...] and Metal dereferences
// it. The result is a SIGSEGV inside AGX, hundreds of frames and one thread
// away from the call that caused it, with nothing in between naming a cause.
//
// That is what killed Into the Radius at ~370 frames: exactly one draw, six
// indices, one instance, from FRHICommandList::EndFrame, into a command buffer
// UE4 had just re-begun while its own index-buffer state still read clean. The
// draw before it and the two thousand before that were all bound.
//
// So: track the binding per command buffer, and DROP an indexed draw that has
// none. Dropping is the only defined thing available — the draw's result is
// undefined by the spec, so nothing correct depends on it, and the alternative
// (binding a substitute buffer) would put whatever bytes it holds through the
// index pipeline as real geometry. Nothing here changes a valid draw: the
// guard passes every draw with a binding through untouched, and the binding it
// checks is the guest's own, recorded by the guest's own vkCmdBindIndexBuffer.
//
// Lock-free on the hot path, because these are the two most frequent calls in
// the API: the table is open-addressed on the command-buffer pointer and a
// slot is claimed by CAS. Recording into one command buffer is
// single-threaded by the spec, so the slot's contents need no lock of their
// own; only the claim races, and only ever once per command buffer.
// ---------------------------------------------------------------------------
static int idxguard_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_IDX_GUARD", 1);
    return v;
}

// Sized for every command buffer the guest ever allocates, not just the ones
// live at once: nothing here hooks vkFreeCommandBuffers, so a slot is never
// handed back. A first cut at 64 entries overflowed inside 2000 draws, and
// every draw into a command buffer it then had no room for was reported
// unbound — an artifact that reads exactly like the bug. Eight thousand wide,
// it does not overflow; if it ever does, `over` says so and the guard stops
// dropping, because a saturated table cannot tell a real unbound draw from a
// bind it had nowhere to write.
#define KLVK_IDX_N 8192
typedef struct {
    VkCommandBuffer cb;       // claimed by CAS; never cleared
    unsigned        binds;    // binds recorded since this cb was begun
    // Round 21: the two pieces of bound state the draw diff's fingerprint
    // needs. Written from the same three hooks that maintain `binds`, and
    // cleared with it at vkBeginCommandBuffer, so a stale binding from the
    // previous recording cannot leak into a new frame's fingerprints.
    VkPipeline      pipe;     // last GRAPHICS pipeline bound in this cb
    VkBuffer        ibuf;     // last index buffer bound in this cb
    VkDeviceSize    ioff;
} klvk_idxbind;
static klvk_idxbind g_idx[KLVK_IDX_N];
static unsigned g_idx_draws, g_idx_dropped, g_idx_over;

static klvk_idxbind *idx_slot(VkCommandBuffer cb, int create) {
    uintptr_t h = ((uintptr_t)cb >> 4) * 0x9E3779B97F4A7C15ull;
    unsigned at = (unsigned)(h >> 40) & (KLVK_IDX_N - 1);
    for (int i = 0; i < 64; i++) {
        klvk_idxbind *s = &g_idx[(at + i) & (KLVK_IDX_N - 1)];
        VkCommandBuffer have = __atomic_load_n(&s->cb, __ATOMIC_ACQUIRE);
        if (have == cb) return s;
        if (!have) {
            if (!create) return NULL;
            VkCommandBuffer want = VK_NULL_HANDLE;
            if (__atomic_compare_exchange_n(&s->cb, &want, cb, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return s;
            if (want == cb) return s;      // lost the race to ourselves
            continue;                      // lost it to another cb; keep probing
        }
    }
    if (create) __atomic_add_fetch(&g_idx_over, 1, __ATOMIC_RELAXED);
    return NULL;
}

static void klvk_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf,
                                    VkDeviceSize off, VkIndexType type) {
    klvk_idxbind *s = idx_slot(cb, 1);
    if (s) { s->binds++; s->ibuf = buf; s->ioff = off; }
    PFN_vkCmdBindIndexBuffer real = g_devs[0] && real_gdpa
        ? (PFN_vkCmdBindIndexBuffer)real_gdpa(g_devs[0]->dev, "vkCmdBindIndexBuffer")
        : NULL;
    if (real) real(cb, buf, off, type);
}

static void klvk_CmdDrawIndexed(VkCommandBuffer cb, uint32_t idx_n, uint32_t inst_n,
                                uint32_t first_idx, int32_t vtx_off, uint32_t first_inst) {
    __atomic_add_fetch(&g_idx_draws, 1, __ATOMIC_RELAXED);
    // Charged to the draw census here rather than from the dispatch, because
    // this name is owned by the index guard. Charged BEFORE the drop test, so
    // the census counts what the guest RECORDED and the guard's own counter
    // stays the one that says what was dropped.
    if (drawc_on()) klvk_draw_note(KLVK_DK_IDX);
    // Same charging point, same reason: what the guest RECORDED. See the
    // geometry census below for what this number can and cannot say.
    klvk_geom_note((uint64_t)idx_n * (uint64_t)inst_n, 1);
    klvk_drawhist_note(idx_n, inst_n);
    klvk_idxbind *s = idx_slot(cb, 0);
    // Round 21's fingerprint, charged at the same point and for the same
    // reason: what the guest RECORDED, before the drop test below.
    klvk_ddiff_note(idx_n, inst_n, first_idx, vtx_off, first_inst, 1,
                    s ? (const void *)s->pipe : NULL,
                    s ? (const void *)s->ibuf : NULL,
                    s ? (uint64_t)s->ioff : 0);
    if ((!s || !s->binds) && !__atomic_load_n(&g_idx_over, __ATOMIC_RELAXED)) {
        unsigned n = __atomic_add_fetch(&g_idx_dropped, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_geom_dropped, (uint64_t)idx_n * (uint64_t)inst_n,
                           __ATOMIC_RELAXED);
        if (n <= 4) {
            VKI("guest recorded vkCmdDrawIndexed(%u indices, %u instance(s)) into "
                "command buffer %p with NO index buffer bound in it — invalid "
                "usage that a mobile driver ignores and Metal segfaults on. The "
                "draw is DROPPED; the frame around it is untouched.\n",
                idx_n, inst_n, (void *)cb);
            kl_fault_print_frames(stderr, NULL);
        } else if (!(n & (n - 1))) {
            // Then only at each doubling. kl_vulkan_memory_report carries the
            // running total, but nothing on the host run path calls it, and a
            // count that only ever appears on device is not an instrument.
            VKI("indexed draws: %u recorded, %u DROPPED for having no index "
                "buffer bound\n",
                __atomic_load_n(&g_idx_draws, __ATOMIC_RELAXED), n);
        }
        return;
    }
    PFN_vkCmdDrawIndexed real = g_devs[0] && real_gdpa
        ? (PFN_vkCmdDrawIndexed)real_gdpa(g_devs[0]->dev, "vkCmdDrawIndexed")
        : NULL;
    if (real) real(cb, idx_n, inst_n, first_idx, vtx_off, first_inst);
}

// A command buffer that is begun loses every binding it had, and so does the
// guard's record of it — otherwise a bind from the previous recording would
// make the first draw of the new one look bound, which is precisely the state
// UE4 itself got wrong.
static VkResult klvk_BeginCommandBuffer(VkCommandBuffer cb,
                                        const VkCommandBufferBeginInfo *bi) {
    cbh_begin(cb);
    cbs_reset(cb);
    klvk_idxbind *s = idx_slot(cb, 1);
    if (s) { s->binds = 0; s->pipe = VK_NULL_HANDLE; s->ibuf = VK_NULL_HANDLE;
             s->ioff = 0; }
    PFN_vkBeginCommandBuffer real = g_devs[0] && real_gdpa
        ? (PFN_vkBeginCommandBuffer)real_gdpa(g_devs[0]->dev, "vkBeginCommandBuffer")
        : NULL;
    return real ? real(cb, bi) : VK_SUCCESS;
}

// Round 21. The pipeline is the field that separates two draws with the same
// index range — a wall and a floor cut from the same mesh buffer render with
// different materials — so the fingerprint needs it. Only GRAPHICS bindings
// are recorded; a compute bind does not change what a draw uses.
static void klvk_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp,
                                 VkPipeline pipe) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        klvk_idxbind *s = idx_slot(cb, 1);
        if (s) s->pipe = pipe;
    }
    PFN_vkCmdBindPipeline real = g_devs[0] && real_gdpa
        ? (PFN_vkCmdBindPipeline)real_gdpa(g_devs[0]->dev, "vkCmdBindPipeline")
        : NULL;
    if (real) real(cb, bp, pipe);
}

// ---------------------------------------------------------------------------
// The DRAW census — "how many draws does this guest actually record, and are
// we even seeing them?"
// ---------------------------------------------------------------------------
//
// Round 15. The index-buffer guard has counted `vkCmdDrawIndexed` since round
// 11, and on this title it reports about **14 indexed draws per frame**. A UE4
// scene does not render in fourteen draws. That number is not a measurement,
// it is a symptom: either the guest draws through entry points nothing here
// hooks, or it reaches the real function pointers without going through our
// dispatch at all. Until that is settled, EVERY conclusion drawn from a draw
// count — including the guard's own "380 drops of 959,508, plateauing" — rests
// on an unknown fraction of the truth.
//
// So this counts every draw entry point the guest can use, per kind, and
// buckets them PER FRAME as a distribution rather than a total.
//
// **THE BLINDNESS ARGUMENT, and it is the point of the whole instrument.**
// A census cannot prove it saw everything by counting what it saw — that is
// exactly how the ~14 got believed for four rounds. So the honesty comes from
// somewhere the census cannot influence: `klvk_GetDeviceProcAddr` sees EVERY
// name this guest resolves, and it records any name beginning `vkCmdDraw` that
// we do NOT hook, BY NAME, along with how many times it was asked for. If the
// guest resolved a draw entry point we are blind to, the census names it and
// says its own numbers are a floor. It cannot manufacture the reassuring
// answer, because the reassuring answer is the ABSENCE of entries in a table
// it does not write to itself.
//
// **ITS REMAINING BLIND SPOTS, stated rather than discovered later:**
//   - A guest that resolves a draw pointer through `dlsym` on libMoltenVK
//     directly, or that was linked against it, bypasses our dispatch entirely
//     and neither the counter nor the name table would see it. That is not
//     detectable from here; what IS detectable is the consequence, and the
//     per-frame distribution below is what makes an implausible number
//     obvious instead of invisible.
//   - It counts CALLS, not primitives or objects. One instanced or indirect
//     call can be thousands of objects, so a low draw count is not by itself a
//     low object count. `vkCmdExecuteCommands` is counted for the same reason:
//     if this guest records into SECONDARY command buffers, the draws are
//     still seen (they go through vkCmdDraw* on the secondary), but knowing
//     the structure changes how the numbers read.
//   - The per-frame bucket is closed at the ovrp frame seam, so draws recorded
//     by a guest thread that is not synchronised with that seam land in
//     whichever bucket was open. The spread, not any single bucket, is the
//     quantity to read.
//
// Observation-only: every hook is the real call plus one relaxed atomic, and
// the whole family leaves the dispatch when KL_VK_DRAW_CENSUS=0 rather than
// leaving a pass-through hop on the hottest path in the API.
static int drawc_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_DRAW_CENSUS", 1);
    return v;
}

static const char *k_drawk_name[KLVK_DK_N] = {
    "Draw", "DrawIndexed", "DrawIndirect", "DrawIndexedIndirect",
    "DrawIndirectCount", "DrawIndexedIndirectCount", "ExecuteCommands"
};
static uint64_t g_drawk[KLVK_DK_N];

// The per-frame bucket, closed at the frame seam.
static uint64_t g_draw_cur;                 // draws recorded in the open frame
static uint64_t g_draw_frames, g_draw_sum, g_draw_max;
static uint64_t g_draw_min = ~0ull;

// The same distribution again over a WINDOW the caller closes, which is what
// the perturbation test needs: the run-long mean cannot answer "did the draw
// count change when the view changed", because both halves of the comparison
// are inside it. A second, resettable set of accumulators is the whole of the
// mechanism; `kl_vulkan_draw_window` reads and clears it.
//
// Kept separate from the run-long set on purpose. Sharing one set and having
// the caller subtract would make the end-of-run census depend on whether
// anyone had windowed it, and a census whose number depends on who else read
// it is not an instrument.
static uint64_t g_draww_frames, g_draww_sum, g_draww_max;
static uint64_t g_draww_min = ~0ull;

static void klvk_draw_note(int kind) {
    __atomic_add_fetch(&g_drawk[kind], 1, __ATOMIC_RELAXED);
    if (kind != KLVK_DK_EXEC)
        __atomic_add_fetch(&g_draw_cur, 1, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// The GEOMETRY census — round 19. "At a fixed view, does the guest submit the
// SAME GEOMETRY every frame, or does geometry go missing for single frames?"
// ---------------------------------------------------------------------------
//
// The draw census counts CALLS. That is the wrong unit for the question this
// port is stuck on. Austin's report is that whole objects vanish for an
// instant: a rock, a wall, a painting. If the guest stops submitting that
// object for one frame, the DRAW COUNT falls by one — indistinguishable from
// the ±1 of any UI element or particle — but the frame's INDEX TOTAL falls by
// that object's entire triangle count, which is hundreds or thousands. So the
// index total is a fingerprint of the submitted scene, and two frames of an
// unchanging view should carry the SAME fingerprint to the index.
//
// Per guest frame, on the same seam the draw census closes on, this
// accumulates
//     sum over vkCmdDrawIndexed of indexCount * instanceCount
//   + sum over vkCmdDraw        of vertexCount * instanceCount
// and every 15 s window reports: frames in the window, per-frame min / max /
// mean, the MODE (the most common exact total) and how many frames carried it,
// how many DISTINCT totals were seen, and the FLAP count — frames whose total
// differs from the immediately preceding frame's by more than 1%.
//
// **HOW TO READ IT.** At a fixed view, in a scene with no animation, a healthy
// guest gives one mode carrying nearly every frame, few distinct values, and a
// flap count near zero. A guest that is dropping objects gives a mode that
// still dominates but a flap count that is not zero, and a min well below the
// mode. Sustained animation (foliage, water, particles) raises the distinct
// count without raising the flap count much, because it changes the total
// smoothly rather than by whole objects.
//
// **ITS BLIND SPOTS, stated here rather than discovered later:**
//   - It counts SUBMITTED geometry, not what reaches the eye. Every loss
//     mechanism downstream of the record — and round 17 argued the loss IS
//     downstream — is invisible to it BY CONSTRUCTION. A flat, constant index
//     total is therefore NOT proof the flicker is absent; it is proof the
//     guest is not the one dropping it. That is the fork this instrument
//     exists to decide, and it can only decide it in one direction.
//   - A HISM/instanced cluster that is culled changes instanceCount, and this
//     DOES see that — which is the case worth catching.
//   - Indirect draws would be invisible (the count lives in a GPU buffer). On
//     this title all six indirect counters read ZERO, so nothing is hidden
//     there; the report says so from the draw census's own numbers rather
//     than from an assumption.
//   - The per-frame bucket closes at the ovrp frame seam, so a guest thread
//     recording off that seam lands in whichever bucket is open. Read the
//     distribution, never one bucket.
//   - It is charged BEFORE the index guard's drop test, for the same reason
//     the draw census is: it says what the guest RECORDED. Indices the guard
//     dropped are counted separately and printed, so the two cannot disagree
//     silently.
//
// It cannot report a comfortable zero: a window in which frames closed but no
// geometry was ever charged prints as a LOUD failure, because that would mean
// the hook is not on the path and every number here is void.
static int geomc_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_GEOM_CENSUS", 1);
    return v;
}

static uint64_t g_geom_cur;          // elements charged into the open frame
static uint64_t g_geom_calls;        // run-long: charges taken (cross-check)
static uint64_t g_geom_total;        // run-long: elements charged

// The window, closed by the 15 s census clock.
#define KLVK_GEOM_MODES 96
static uint64_t g_geomw_frames, g_geomw_sum, g_geomw_max;
static uint64_t g_geomw_min = ~0ull;
static uint64_t g_geomw_prev;        // previous frame's total, 0 = none yet
static uint64_t g_geomw_flaps;       // |n - prev| > 1% of prev
static uint64_t g_geomw_worstflap;   // largest single-frame delta in the window
// The same three again over NON-EMPTY frames only. A frame that closed with
// zero geometry is a real observation and is kept in the raw numbers, but on
// this title ~38% of seams close empty (the guest renders on a subset of ovrp
// frame seams), and a zero next to a full frame is a 100% delta that would
// make the flap count read as catastrophic flicker in a scene Austin calls
// clean. The empty frames are reported as their own count; the flap question
// is then asked of the frames that actually drew.
static uint64_t g_geomw_empty;
static uint64_t g_geomw_nz_frames, g_geomw_nz_sum, g_geomw_nz_max;
static uint64_t g_geomw_nz_min = ~0ull;
static uint64_t g_geomw_nz_prev, g_geomw_nz_flaps, g_geomw_nz_worst;
static struct { uint64_t v; uint64_t n; } g_geomw_mode[KLVK_GEOM_MODES];
static unsigned g_geomw_modes, g_geomw_mode_over;

// KL_VK_DRAW_HIST=1 (diagnostic, round N15): where the frame's indices go —
// recorded draws bucketed by instance count and by log2(index count), each
// bucket's draws and indices x instances per frame, every 600 frames.
static int drawhist_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_DRAW_HIST", 0);
    return on;
}
static uint64_t g_dh_inst[6][2], g_dh_log[33][2], g_dh_frames;
static void klvk_drawhist_note(uint32_t idx_n, uint32_t inst_n) {
    if (!drawhist_on()) return;
    uint64_t e = (uint64_t)idx_n * inst_n;
    int ib = inst_n <= 1 ? 0 : inst_n == 2 ? 1 : inst_n <= 4 ? 2 : inst_n <= 16 ? 3 : inst_n <= 128 ? 4 : 5;
    int lb = idx_n ? 32 - __builtin_clz(idx_n) : 0;
    __atomic_add_fetch(&g_dh_inst[ib][0], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_dh_inst[ib][1], e, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_dh_log[lb][0], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_dh_log[lb][1], e, __ATOMIC_RELAXED);
}
static void klvk_drawhist_frame(void) {
    if (!drawhist_on() || ++g_dh_frames < 600) return;
    double f = (double)g_dh_frames;
    static const char *in[6] = { "1", "2", "3-4", "5-16", "17-128", ">128" };
    fprintf(stderr, "  [vk] draw hist over %.0f frames, by instance count (draws/f, Mindices x inst/f):", f);
    for (int i = 0; i < 6; i++)
        if (g_dh_inst[i][0]) fprintf(stderr, " [%s] %.0f %.2f", in[i], g_dh_inst[i][0] / f, g_dh_inst[i][1] / f / 1e6);
    fprintf(stderr, "\n  [vk] draw hist by index count <2^k:");
    for (int i = 0; i < 33; i++)
        if (g_dh_log[i][0]) fprintf(stderr, " [2^%d] %.0f %.2f", i, g_dh_log[i][0] / f, g_dh_log[i][1] / f / 1e6);
    fprintf(stderr, "\n");
    memset(g_dh_inst, 0, sizeof g_dh_inst);
    memset(g_dh_log, 0, sizeof g_dh_log);
    g_dh_frames = 0;
}

static void klvk_geom_note(uint64_t elems, int indexed) {
    (void)indexed;
    if (!geomc_on()) return;
    __atomic_add_fetch(&g_geom_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_geom_total, elems, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_geom_cur, elems, __ATOMIC_RELAXED);
}

// Called from klvk_draw_frame_close, i.e. the same seam and the same instant
// as the draw census's own bucket close. Single-threaded there.
static void klvk_geom_frame_close(void) {
    klvk_drawhist_frame();
    if (!geomc_on()) return;
    uint64_t n = __atomic_exchange_n(&g_geom_cur, 0, __ATOMIC_RELAXED);

    g_geomw_frames++;
    g_geomw_sum += n;
    if (n > g_geomw_max) g_geomw_max = n;
    if (n < g_geomw_min) g_geomw_min = n;

    if (g_geomw_prev) {
        uint64_t d = n > g_geomw_prev ? n - g_geomw_prev : g_geomw_prev - n;
        if (d > g_geomw_worstflap) g_geomw_worstflap = d;
        // Strictly more than 1% of the previous frame, so a scene that is
        // genuinely static reads zero rather than reading rounding noise.
        if (d * 100 > g_geomw_prev) g_geomw_flaps++;
    }
    g_geomw_prev = n;

    if (!n) {
        g_geomw_empty++;
    } else {
        g_geomw_nz_frames++;
        g_geomw_nz_sum += n;
        if (n > g_geomw_nz_max) g_geomw_nz_max = n;
        if (n < g_geomw_nz_min) g_geomw_nz_min = n;
        if (g_geomw_nz_prev) {
            uint64_t d = n > g_geomw_nz_prev ? n - g_geomw_nz_prev
                                             : g_geomw_nz_prev - n;
            if (d > g_geomw_nz_worst) g_geomw_nz_worst = d;
            if (d * 100 > g_geomw_nz_prev) g_geomw_nz_flaps++;
        }
        g_geomw_nz_prev = n;
    }

    for (unsigned i = 0; i < g_geomw_modes; i++)
        if (g_geomw_mode[i].v == n) { g_geomw_mode[i].n++; return; }
    if (g_geomw_modes >= KLVK_GEOM_MODES) { g_geomw_mode_over++; return; }
    g_geomw_mode[g_geomw_modes].v = n;
    g_geomw_mode[g_geomw_modes].n = 1;
    g_geomw_modes++;
}

static void klvk_geom_report(FILE *f) {
    if (!f) return;
    if (!geomc_on()) {
        fprintf(f, "  [vk] geometry census: OFF (KL_VK_GEOM_CENSUS=0) — no "
                   "number below is available\n");
        return;
    }
    uint64_t frames = g_geomw_frames, sum = g_geomw_sum;
    uint64_t mn = g_geomw_min == ~0ull ? 0 : g_geomw_min, mx = g_geomw_max;
    uint64_t flaps = g_geomw_flaps, worst = g_geomw_worstflap;
    unsigned modes = g_geomw_modes, over = g_geomw_mode_over;
    uint64_t empty = g_geomw_empty;
    uint64_t zf = g_geomw_nz_frames, zs = g_geomw_nz_sum;
    uint64_t zmn = g_geomw_nz_min == ~0ull ? 0 : g_geomw_nz_min;
    uint64_t zmx = g_geomw_nz_max, zfl = g_geomw_nz_flaps, zw = g_geomw_nz_worst;
    // The mode is taken over NON-EMPTY frames: the empty ones have their own
    // count and would otherwise win the mode outright and say nothing.
    uint64_t bv = 0, bn = 0;
    for (unsigned i = 0; i < modes; i++)
        if (g_geomw_mode[i].v && g_geomw_mode[i].n > bn) {
            bn = g_geomw_mode[i].n; bv = g_geomw_mode[i].v;
        }

    // Reset the window before printing anything, so an early return cannot
    // leave a stale window to be read as the next one.
    g_geomw_frames = 0; g_geomw_sum = 0; g_geomw_max = 0; g_geomw_min = ~0ull;
    g_geomw_flaps = 0; g_geomw_worstflap = 0;
    g_geomw_modes = 0; g_geomw_mode_over = 0;
    g_geomw_empty = 0;
    g_geomw_nz_frames = 0; g_geomw_nz_sum = 0; g_geomw_nz_max = 0;
    g_geomw_nz_min = ~0ull; g_geomw_nz_flaps = 0; g_geomw_nz_worst = 0;
    // g_geomw_prev deliberately SURVIVES the window: the frame either side of
    // a window boundary is still two adjacent frames, and zeroing it would
    // silently discard one comparison every 15 s.

    if (!frames) {
        fprintf(f, "  [vk] geometry census: NO FRAME SEAM CLOSED IN THIS "
                   "WINDOW — the census has nothing to say and the absence is "
                   "the finding\n");
        return;
    }
    if (!g_geom_calls) {
        fprintf(f, "  [vk] geometry census: *** NOT ONE DRAW WAS EVER CHARGED "
                   "TO THIS CENSUS across %llu frame(s) — the hook is not on "
                   "the guest's path and EVERY GEOMETRY NUMBER IN THIS LOG IS "
                   "VOID ***\n", (unsigned long long)frames);
        return;
    }
    fprintf(f, "  [vk] geometry census: %llu frame seam(s), %llu of them EMPTY "
               "(no geometry recorded) | raw indices/frame: %llu min, %llu max, "
               "%.0f mean | FLAPS >1%%: %llu, worst delta %llu\n",
            (unsigned long long)frames, (unsigned long long)empty,
            (unsigned long long)mn, (unsigned long long)mx,
            (double)sum / (double)frames,
            (unsigned long long)flaps, (unsigned long long)worst);
    if (!zf)
        fprintf(f, "  [vk]   and EVERY ONE OF THEM WAS EMPTY — the guest "
                   "recorded no geometry at all in this window\n");
    else
        fprintf(f, "  [vk]   drawing frames only (%llu): %llu min, %llu max, "
                   "%.0f mean | mode %llu carried by %llu of %llu, %u distinct "
                   "value(s)%s | FLAPS >1%%: %llu, worst delta %llu\n",
                (unsigned long long)zf,
                (unsigned long long)zmn, (unsigned long long)zmx,
                (double)zs / (double)zf,
                (unsigned long long)bv, (unsigned long long)bn,
                (unsigned long long)zf, modes,
                over ? " (+ MORE — the mode table overflowed, so the mode is a "
                       "FLOOR and the distinct count is a floor too)" : "",
                (unsigned long long)zfl, (unsigned long long)zw);
    // The cross-check against the draw census, on the same line family: the
    // charge count must equal Draw + DrawIndexed exactly. If it does not, one
    // of the two instruments is not on the path it claims to be on.
    uint64_t dc = g_drawk[KLVK_DK_DRAW] + g_drawk[KLVK_DK_IDX];
    if (g_geom_calls != dc)
        fprintf(f, "  [vk]   *** THE TWO CENSUSES DISAGREE: %llu geometry "
                   "charge(s) against %llu Draw+DrawIndexed call(s). One of "
                   "them is not seeing the path it claims to. ***\n",
                (unsigned long long)g_geom_calls, (unsigned long long)dc);
    else
        fprintf(f, "  [vk]   run total %llu indices over %llu charge(s), which "
                   "MATCHES the draw census's Draw+DrawIndexed exactly; %llu "
                   "index(es) were dropped by the index guard and are counted "
                   "above as recorded\n",
                (unsigned long long)g_geom_total,
                (unsigned long long)g_geom_calls,
                (unsigned long long)g_geom_dropped);
    uint64_t ind = g_drawk[KLVK_DK_INDIRECT] + g_drawk[KLVK_DK_IDX_INDIRECT]
                 + g_drawk[KLVK_DK_INDIRECT_COUNT]
                 + g_drawk[KLVK_DK_IDX_INDIRECT_COUNT];
    if (ind)
        fprintf(f, "  [vk]   *** %llu INDIRECT draw(s) were recorded this run. "
                   "Their geometry lives in a GPU buffer and is NOT in the "
                   "totals above, which are therefore a FLOOR. ***\n",
                (unsigned long long)ind);
}

// ---------------------------------------------------------------------------
// The DRAW-DIFF — round 21. "Does the guest stop submitting THAT ONE OBJECT
// for a frame?"
// ---------------------------------------------------------------------------
//
// Round 19's geometry census reads the frame's INDEX TOTAL, and round 20 read
// 418764-421044 indices/frame at rest in the level with 0 flaps over 1% and a
// worst adjacent delta of 324. That sounds like a null, and it is not one: the
// wall Austin photographed missing (docs/shots/launch8-missing-wall.png) is a
// dozen low-poly planks, on the order of 400 indices — a TENTH of one percent
// of the frame. The census CANNOT SEE ONE WALL GO MISSING. This instrument
// exists because of that arithmetic.
//
// So instead of a scalar per frame, this keeps the frame's MULTISET of draw
// FINGERPRINTS and diffs it against the previous frame's. A fingerprint is
//     (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance,
//      the bound graphics VkPipeline, the bound index VkBuffer + its offset)
// hashed to 64 bits. Two frames of an unchanging view should carry the SAME
// multiset, element for element. A draw present last frame and absent this one
// is VANISHED; the reverse is APPEARED.
//
// **THE READING THAT DECIDES THE ROUND.** At rest in a level, with Austin
// watching a wall blink:
//   - vanished == 0 every frame  ⇒ the guest submits the wall's draw every
//     single frame, and the corruption is in PER-INSTANCE data or on the GPU
//     (which is where R1/R3 live).
//   - one-frame BLINKS at rest   ⇒ the guest itself omits the draw, and the
//     search goes back into the guest's visibility path. Not culling —
//     occlusion is off by config — so: cached mesh draw command invalidation.
// A fingerprint that vanishes and STAYS gone is a legitimate visibility
// change and is counted separately, which is why the blink count is the
// headline and the raw vanished count is not.
//
// **ITS BLIND SPOTS, stated here rather than discovered later:**
//   - IDENTICAL FINGERPRINTS COLLAPSE. The same mesh drawn twice with the same
//     index range and the same pipeline is one multiset entry with a count of
//     two, and losing one of the two shows up as a count change — but two
//     DIFFERENT objects that happen to share a mesh, a pipeline and an index
//     range are indistinguishable from each other. Instancing hides more of
//     this than anything: a HISM cluster is one draw whatever it contains.
//   - PER-INSTANCE DATA IS INVISIBLE. UE4's PrimitiveId stream and GPUScene
//     carry the transform and the primitive index; none of it is in the draw
//     arguments. A draw submitted with a WRONG TRANSFORM has a fingerprint
//     identical to the correct one. This instrument cannot see the failure
//     mode it is most likely to be looking at — it can only rule the OTHER
//     one (the guest omitting the draw) in or out.
//   - It is charged where the guest RECORDS, so everything downstream of the
//     record is invisible by construction, exactly as the geometry census is.
//   - The per-cb pipeline/index-buffer state is tracked through
//     vkCmdBindPipeline / vkCmdBindIndexBuffer / vkBeginCommandBuffer. A guest
//     that reaches those through a pointer we never handed out would leave the
//     state stale; the same GetDeviceProcAddr blindness table that guards the
//     draw census covers the draw names, and the two binds are hooked from the
//     same dispatch.
//   - Indirect draws carry their arguments in a GPU buffer and are not
//     fingerprinted at all. On this title all six indirect counters read ZERO.
//   - Secondary command buffers: draws recorded into them are still seen (they
//     go through vkCmdDraw* on the secondary), but they land in whichever
//     frame bucket is open.
//
// It cannot report a comfortable zero: a window in which frames closed but no
// draw was ever fingerprinted prints as a LOUD failure, and its own charge
// count is cross-checked against the draw census's Draw+DrawIndexed every
// window exactly as the geometry census's is.
static int ddiff_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_DRAW_DIFF", 1);
    return v;
}

// A frame of this title records ~230 draws (round 20's whole-run mean) and the
// brief's ceiling is ~1300. Four thousand wide, a frame that overflows is
// counted and EXCLUDED from the diff rather than silently truncated — a
// truncated multiset would manufacture vanished draws out of nothing.
#define KLVK_DD_N 4096
// Two fingerprints, not one, and the second is what makes the first readable.
//
// THE FIRST HOST RUN OF THIS INSTRUMENT measured, in the menu at rest, exactly
// 24 vanished and 24 appeared draws EVERY frame, 0 frames identical, and the
// blink histogram came back "0 at N+1, 48822 at N+2" — a strict PERIOD-2
// alternation. That is not the guest dropping anything. It is UE4's
// double-buffered dynamic index ring: the same draws are re-recorded every
// frame against an index buffer that alternates between two allocations, so
// the (buffer, offset) half of the fine fingerprint flips every other frame
// while the geometry census reads one byte-identical index total.
//
// So the fine fingerprint has a structural noise floor of ~24/frame here, and
// a wall going missing is ONE draw. The COARSE fingerprint drops the two
// fields that flip — the index buffer handle and its offset, and firstIndex
// with them — and keeps (indexCount, instanceCount, vertexOffset, pipeline).
// A draw that is merely re-bound is IDENTICAL under it; a draw that stopped
// being submitted is still missing. The coarse count is the one to read; the
// fine count is kept because it is the one that proves the coarse count is
// not hiding a rebind.
typedef struct { uint64_t h, h2; uint32_t n; } klvk_dd;  // fine, coarse, indexCount

static klvk_dd g_dd_a[KLVK_DD_N], g_dd_b[KLVK_DD_N];
static klvk_dd *g_dd_cur = g_dd_a, *g_dd_prev = g_dd_b;
static unsigned g_dd_cur_n, g_dd_prev_n;
static int      g_dd_cur_bad, g_dd_prev_bad;   // frame overflowed -> unusable
static uint64_t g_dd_charges;                  // run-long, for the cross-check

// The two previous frames' VANISHED fingerprints, so a reappearance can be
// called a BLINK (gap 1 or 2) rather than just an appearance.
#define KLVK_DD_V 512
static klvk_dd  g_dd_van1[KLVK_DD_V], g_dd_van2[KLVK_DD_V];
static unsigned g_dd_van1_n, g_dd_van2_n;

// The window, closed by the same 15 s census clock as everything else.
static uint64_t g_ddw_frames, g_ddw_cmp, g_ddw_identical;
static uint64_t g_ddw_with_vanish, g_ddw_vanished, g_ddw_appeared;
static uint64_t g_ddw_h0, g_ddw_h1, g_ddw_h2_5, g_ddw_h6_20, g_ddw_h21;
static uint32_t g_ddw_biggest;                 // largest vanished indexCount
static uint64_t g_ddw_blink1, g_ddw_blink2;    // came back at N+1 / N+2
static uint64_t g_ddw_overflow;
// The COARSE diff — the headline numbers. Same merge, on the fingerprint that
// ignores which ring-buffer allocation the draw was bound to.
static uint64_t g_ddw_c_vanished, g_ddw_c_appeared, g_ddw_c_with_vanish;
static uint64_t g_ddw_c_identical, g_ddw_c_worst;
static struct { uint32_t n; uint8_t gap; } g_ddw_sample[6];
static unsigned g_ddw_sample_n;

static int dd_cmp(const void *a, const void *b) {
    uint64_t x = ((const klvk_dd *)a)->h, y = ((const klvk_dd *)b)->h;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int dd_cmp2(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// splitmix64 over the eight fields. Any decent avalanche will do; what matters
// is that every field is in it, so two draws that differ in ONE argument do
// not collide.
static uint64_t dd_mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Called from klvk_CmdDrawIndexed and klvk_CmdDraw, at the same charging point
// the draw and geometry censuses use: what the guest RECORDED, before the
// index guard's drop test. `ibuf`/`ioff`/`pipe` come from the per-command-
// buffer slot; a draw whose command buffer has no slot contributes zeroes
// there, which is a real state and is not treated as an error.
static void klvk_ddiff_note(uint32_t idx_n, uint32_t inst_n, uint32_t first_idx,
                            int32_t vtx_off, uint32_t first_inst, int indexed,
                            const void *pipe, const void *ibuf, uint64_t ioff) {
    if (!ddiff_on()) return;
    __atomic_add_fetch(&g_dd_charges, 1, __ATOMIC_RELAXED);
    uint64_t h = dd_mix((uint64_t)idx_n * 0x100000001B3ull + (uint64_t)indexed);
    h = dd_mix(h ^ ((uint64_t)inst_n << 32 | first_idx));
    h = dd_mix(h ^ ((uint64_t)(uint32_t)vtx_off << 32 | first_inst));
    h = dd_mix(h ^ (uintptr_t)pipe);
    h = dd_mix(h ^ (uintptr_t)ibuf);
    h = dd_mix(h ^ ioff);
    uint64_t h2 = dd_mix((uint64_t)idx_n * 0x100000001B3ull + (uint64_t)indexed);
    h2 = dd_mix(h2 ^ ((uint64_t)inst_n << 32 | (uint32_t)vtx_off));
    h2 = dd_mix(h2 ^ (uintptr_t)pipe);
    if (g_dd_cur_n >= KLVK_DD_N) { g_dd_cur_bad = 1; return; }
    g_dd_cur[g_dd_cur_n].h = h;
    g_dd_cur[g_dd_cur_n].h2 = h2;
    g_dd_cur[g_dd_cur_n].n = idx_n;
    g_dd_cur_n++;
}

static int dd_in(const klvk_dd *set, unsigned n, uint64_t h) {
    for (unsigned i = 0; i < n; i++) if (set[i].h == h) return (int)i + 1;
    return 0;
}

// Same seam, same instant as the other two censuses.
static void klvk_ddiff_frame_close(void) {
    if (!ddiff_on()) return;
    unsigned cn = g_dd_cur_n; int cbad = g_dd_cur_bad;
    klvk_dd *cur = g_dd_cur, *prev = g_dd_prev;
    unsigned pn = g_dd_prev_n; int pbad = g_dd_prev_bad;

    g_ddw_frames++;
    if (cbad) g_ddw_overflow++;

    if (!cbad && !pbad && pn) {
        qsort(cur, cn, sizeof *cur, dd_cmp);
        g_ddw_cmp++;
        // Multiset difference by one merge over two sorted arrays.
        unsigned i = 0, j = 0, vanished = 0, appeared = 0;
        unsigned van_n = 0;
        static klvk_dd van[KLVK_DD_V];
        while (i < pn || j < cn) {
            if (j >= cn || (i < pn && prev[i].h < cur[j].h)) {
                // In prev, not in cur: VANISHED.
                vanished++;
                if (prev[i].n > g_ddw_biggest) g_ddw_biggest = prev[i].n;
                if (van_n < KLVK_DD_V) van[van_n++] = prev[i];
                i++;
            } else if (i >= pn || cur[j].h < prev[i].h) {
                // In cur, not in prev: APPEARED. Was it one we lost recently?
                appeared++;
                int k = dd_in(g_dd_van1, g_dd_van1_n, cur[j].h);
                int gap = 0;
                if (k) { gap = 1; g_dd_van1[k - 1].h = 0; g_ddw_blink1++; }
                else {
                    k = dd_in(g_dd_van2, g_dd_van2_n, cur[j].h);
                    if (k) { gap = 2; g_dd_van2[k - 1].h = 0; g_ddw_blink2++; }
                }
                if (gap && g_ddw_sample_n < 6) {
                    g_ddw_sample[g_ddw_sample_n].n = cur[j].n;
                    g_ddw_sample[g_ddw_sample_n].gap = (uint8_t)gap;
                    g_ddw_sample_n++;
                }
                j++;
            } else { i++; j++; }
        }
        g_ddw_vanished += vanished;
        g_ddw_appeared += appeared;
        if (vanished) g_ddw_with_vanish++;
        if (!vanished && !appeared) g_ddw_identical++;
        if (!vanished)              g_ddw_h0++;
        else if (vanished == 1)     g_ddw_h1++;
        else if (vanished <= 5)     g_ddw_h2_5++;
        else if (vanished <= 20)    g_ddw_h6_20++;
        else                        g_ddw_h21++;

        // The coarse pass: the same merge again, over h2 alone.
        {
            static uint64_t ca[KLVK_DD_N], pa[KLVK_DD_N];
            for (unsigned k = 0; k < cn; k++) ca[k] = cur[k].h2;
            for (unsigned k = 0; k < pn; k++) pa[k] = prev[k].h2;
            qsort(ca, cn, sizeof ca[0], dd_cmp2);
            qsort(pa, pn, sizeof pa[0], dd_cmp2);
            unsigned x = 0, y = 0, cv = 0, capp = 0;
            while (x < pn || y < cn) {
                if (y >= cn || (x < pn && pa[x] < ca[y]))      { cv++;   x++; }
                else if (x >= pn || ca[y] < pa[x])             { capp++; y++; }
                else                                           { x++; y++; }
            }
            g_ddw_c_vanished += cv;
            g_ddw_c_appeared += capp;
            if (cv) g_ddw_c_with_vanish++; else if (!capp) g_ddw_c_identical++;
            if (cv > g_ddw_c_worst) g_ddw_c_worst = cv;
        }

        // Age the vanished lists by one frame.
        memcpy(g_dd_van2, g_dd_van1, sizeof g_dd_van1[0] * g_dd_van1_n);
        g_dd_van2_n = g_dd_van1_n;
        memcpy(g_dd_van1, van, sizeof van[0] * van_n);
        g_dd_van1_n = van_n;
    } else if (!cbad) {
        qsort(cur, cn, sizeof *cur, dd_cmp);
    }

    // Swap the buffers: this frame becomes the previous one.
    g_dd_prev = cur; g_dd_prev_n = cn; g_dd_prev_bad = cbad;
    g_dd_cur  = prev; g_dd_cur_n = 0;  g_dd_cur_bad = 0;
}

static void klvk_ddiff_report(FILE *f) {
    if (!f) return;
    if (!ddiff_on()) {
        fprintf(f, "  [vk] draw diff: OFF (KL_VK_DRAW_DIFF=0) — no number "
                   "below is available\n");
        return;
    }
    uint64_t frames = g_ddw_frames, cmp = g_ddw_cmp, ident = g_ddw_identical;
    uint64_t wv = g_ddw_with_vanish, van = g_ddw_vanished, app = g_ddw_appeared;
    uint64_t h0 = g_ddw_h0, h1 = g_ddw_h1, h25 = g_ddw_h2_5;
    uint64_t h620 = g_ddw_h6_20, h21 = g_ddw_h21;
    uint32_t big = g_ddw_biggest;
    uint64_t b1 = g_ddw_blink1, b2 = g_ddw_blink2, ovf = g_ddw_overflow;
    uint64_t cv = g_ddw_c_vanished, capp = g_ddw_c_appeared;
    uint64_t cwv = g_ddw_c_with_vanish, cid = g_ddw_c_identical, cw = g_ddw_c_worst;
    unsigned sn = g_ddw_sample_n;
    struct { uint32_t n; uint8_t gap; } smp[6];
    memcpy(smp, g_ddw_sample, sizeof smp);

    g_ddw_frames = 0; g_ddw_cmp = 0; g_ddw_identical = 0;
    g_ddw_with_vanish = 0; g_ddw_vanished = 0; g_ddw_appeared = 0;
    g_ddw_h0 = 0; g_ddw_h1 = 0; g_ddw_h2_5 = 0; g_ddw_h6_20 = 0; g_ddw_h21 = 0;
    g_ddw_biggest = 0; g_ddw_blink1 = 0; g_ddw_blink2 = 0; g_ddw_overflow = 0;
    g_ddw_sample_n = 0;
    g_ddw_c_vanished = 0; g_ddw_c_appeared = 0; g_ddw_c_with_vanish = 0;
    g_ddw_c_identical = 0; g_ddw_c_worst = 0;

    if (!frames) {
        fprintf(f, "  [vk] draw diff: NO FRAME SEAM CLOSED IN THIS WINDOW\n");
        return;
    }
    if (!g_dd_charges) {
        fprintf(f, "  [vk] draw diff: *** NOT ONE DRAW WAS EVER FINGERPRINTED "
                   "across %llu frame(s) — the hook is not on the guest's path "
                   "and EVERY DIFF NUMBER IN THIS LOG IS VOID ***\n",
                (unsigned long long)frames);
        return;
    }
    fprintf(f, "  [vk] draw diff: %llu frame(s), %llu compared, %llu IDENTICAL "
               "to the previous frame | vanished %llu in %llu frame(s), "
               "appeared %llu | biggest vanished draw %u indices%s\n",
            (unsigned long long)frames, (unsigned long long)cmp,
            (unsigned long long)ident, (unsigned long long)van,
            (unsigned long long)wv, (unsigned long long)app, big,
            ovf ? " | *** SOME FRAMES OVERFLOWED THE 4096-DRAW TABLE AND WERE "
                  "EXCLUDED ***" : "");
    fprintf(f, "  [vk]   vanished-per-frame: 0:%llu 1:%llu 2-5:%llu 6-20:%llu "
               ">20:%llu | BLINKS (gone then back): %llu at N+1, %llu at N+2 — "
               "%lld of the vanished stayed gone\n",
            (unsigned long long)h0, (unsigned long long)h1,
            (unsigned long long)h25, (unsigned long long)h620,
            (unsigned long long)h21, (unsigned long long)b1,
            (unsigned long long)b2,
            (long long)van - (long long)(b1 + b2));
    if (sn) {
        fprintf(f, "  [vk]   blink sample (indices@gap):");
        for (unsigned i = 0; i < sn; i++)
            fprintf(f, " %u@%u", smp[i].n, (unsigned)smp[i].gap);
        fprintf(f, "\n");
    }
    fprintf(f, "  [vk]   COARSE (pipeline + counts, ring rebinds ignored — THIS "
               "IS THE ONE TO READ): %llu IDENTICAL of %llu compared | vanished "
               "%llu in %llu frame(s), appeared %llu, worst single frame %llu\n",
            (unsigned long long)cid, (unsigned long long)cmp,
            (unsigned long long)cv, (unsigned long long)cwv,
            (unsigned long long)capp, (unsigned long long)cw);
    uint64_t dc = g_drawk[KLVK_DK_DRAW] + g_drawk[KLVK_DK_IDX];
    if (g_dd_charges != dc)
        fprintf(f, "  [vk]   *** THE DRAW DIFF AND THE DRAW CENSUS DISAGREE: "
                   "%llu fingerprint(s) against %llu Draw+DrawIndexed call(s). "
                   "One of them is not seeing the path it claims to. ***\n",
                (unsigned long long)g_dd_charges, (unsigned long long)dc);
}

// Closed from kl_vulkan_frame_done, which is the ovrp frame seam.
static void klvk_draw_frame_close(void) {
    if (!drawc_on()) return;
    uint64_t n = __atomic_exchange_n(&g_draw_cur, 0, __ATOMIC_RELAXED);
    // A frame that recorded nothing is a real observation (a loading screen,
    // a frame that submitted no eye layer) and is counted, not skipped —
    // skipping it would quietly flatter the mean.
    g_draw_frames++;
    g_draw_sum += n;
    if (n > g_draw_max) g_draw_max = n;
    if (n < g_draw_min) g_draw_min = n;

    // Same seam, same instant: the geometry census's bucket closes with the
    // draw census's so the two are always describing the same frame.
    klvk_geom_frame_close();
    klvk_ddiff_frame_close();

    g_draww_frames++;
    g_draww_sum += n;
    if (n > g_draww_max) g_draww_max = n;
    if (n < g_draww_min) g_draww_min = n;
}

// Read and clear the window. Returns 1 when the numbers mean something and 0
// when the census is OFF — the caller must say "unavailable" rather than print
// a plausible zero, which is the failure mode this port has paid for seven
// times. `frames` is the count the mean is over; a caller that gets frames==0
// closed a window no frame seam passed through and must not divide by it.
int kl_vulkan_draw_window(unsigned long long *frames, unsigned long long *sum,
                          unsigned long long *min, unsigned long long *max) {
    if (frames) *frames = 0;
    if (sum)    *sum    = 0;
    if (min)    *min    = 0;
    if (max)    *max    = 0;
    if (!drawc_on()) return 0;
    if (frames) *frames = g_draww_frames;
    if (sum)    *sum    = g_draww_sum;
    if (min)    *min    = g_draww_min == ~0ull ? 0 : g_draww_min;
    if (max)    *max    = g_draww_max;
    g_draww_frames = 0; g_draww_sum = 0; g_draww_max = 0; g_draww_min = ~0ull;
    return 1;
}

// The names the guest asked for that we do NOT hook. Written only from
// klvk_GetDeviceProcAddr; this is the half the census cannot fake.
#define KLVK_DRAWMISS_N 16
static struct { char name[48]; unsigned hits; } g_drawmiss[KLVK_DRAWMISS_N];
static unsigned g_drawmiss_n, g_drawmiss_over;

static void klvk_draw_note_unhooked(const char *name) {
    for (unsigned i = 0; i < g_drawmiss_n; i++)
        if (!strcmp(g_drawmiss[i].name, name)) { g_drawmiss[i].hits++; return; }
    if (g_drawmiss_n >= KLVK_DRAWMISS_N) { g_drawmiss_over++; return; }
    snprintf(g_drawmiss[g_drawmiss_n].name, sizeof g_drawmiss[0].name, "%s", name);
    g_drawmiss[g_drawmiss_n].hits = 1;
    g_drawmiss_n++;
}

#define KLVK_DRAW_REAL(fn) \
    (g_devs[0] && real_gdpa ? (PFN_##fn)real_gdpa(g_devs[0]->dev, #fn) : NULL)

static void klvk_CmdDraw(VkCommandBuffer cb, uint32_t v, uint32_t i,
                         uint32_t fv, uint32_t fi) {
    klvk_draw_note(KLVK_DK_DRAW);
    klvk_geom_note((uint64_t)v * (uint64_t)i, 0);
    {
        // A non-indexed draw has no index buffer; the field is zero and that
        // is a real state, not a missing measurement.
        klvk_idxbind *s = idx_slot(cb, 0);
        klvk_ddiff_note(v, i, 0, 0, fi, 0,
                        s ? (const void *)s->pipe : NULL, NULL, 0);
    }
    PFN_vkCmdDraw real = KLVK_DRAW_REAL(vkCmdDraw);
    if (real) real(cb, v, i, fv, fi);
}

static void klvk_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o,
                                 uint32_t n, uint32_t stride) {
    klvk_draw_note(KLVK_DK_INDIRECT);
    PFN_vkCmdDrawIndirect real = KLVK_DRAW_REAL(vkCmdDrawIndirect);
    if (real) real(cb, b, o, n, stride);
}

static void klvk_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b,
                                        VkDeviceSize o, uint32_t n,
                                        uint32_t stride) {
    klvk_draw_note(KLVK_DK_IDX_INDIRECT);
    PFN_vkCmdDrawIndexedIndirect real = KLVK_DRAW_REAL(vkCmdDrawIndexedIndirect);
    if (real) real(cb, b, o, n, stride);
}

static void klvk_CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b,
                                      VkDeviceSize o, VkBuffer cbuf,
                                      VkDeviceSize coff, uint32_t maxn,
                                      uint32_t stride) {
    klvk_draw_note(KLVK_DK_INDIRECT_COUNT);
    PFN_vkCmdDrawIndirectCount real = KLVK_DRAW_REAL(vkCmdDrawIndirectCount);
    if (real) real(cb, b, o, cbuf, coff, maxn, stride);
}

static void klvk_CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b,
                                             VkDeviceSize o, VkBuffer cbuf,
                                             VkDeviceSize coff, uint32_t maxn,
                                             uint32_t stride) {
    klvk_draw_note(KLVK_DK_IDX_INDIRECT_COUNT);
    PFN_vkCmdDrawIndexedIndirectCount real =
        KLVK_DRAW_REAL(vkCmdDrawIndexedIndirectCount);
    if (real) real(cb, b, o, cbuf, coff, maxn, stride);
}

static void klvk_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n,
                                    const VkCommandBuffer *sec) {
    klvk_draw_note(KLVK_DK_EXEC);
    if (perf_split_on() && sec) {                  // fold the secondaries' passes in
        unsigned k = cbs_slot(cb, 1);
        for (uint32_t i = 0; i < n && k != UINT32_MAX; i++) {
            unsigned j = cbs_slot(sec[i], 0);
            if (j == UINT32_MAX) continue;
            g_cbs[k].passes += g_cbs[j].passes;
            g_cbs[k].copies += g_cbs[j].copies;
            g_cbs[k].px += g_cbs[j].px;
        }
    }
    PFN_vkCmdExecuteCommands real = KLVK_DRAW_REAL(vkCmdExecuteCommands);
    if (real) real(cb, n, sec);
}

static void klvk_draw_report(FILE *f) {
    if (!f || !drawc_on()) return;
    uint64_t total = 0;
    for (int i = 0; i < KLVK_DK_N; i++)
        if (i != KLVK_DK_EXEC) total += g_drawk[i];
    if (!total && !g_drawk[KLVK_DK_EXEC] && !g_drawmiss_n) {
        fprintf(f, "  [vk] draw census: NOT ONE DRAW OF ANY KIND REACHED THIS "
                   "LAYER, and the guest asked for no draw entry point we do "
                   "not hook either — so this guest is not drawing through our "
                   "dispatch at all and every draw number in this log is void\n");
        return;
    }
    fprintf(f, "  [vk] draw census: %llu draw call(s) —",
            (unsigned long long)total);
    for (int i = 0; i < KLVK_DK_N; i++)
        if (g_drawk[i])
            fprintf(f, " %s %llu", k_drawk_name[i],
                    (unsigned long long)g_drawk[i]);
    if (g_draw_frames)
        fprintf(f, " | per frame: %.1f mean, %llu min, %llu max over %llu frame(s)",
                (double)g_draw_sum / (double)g_draw_frames,
                (unsigned long long)(g_draw_min == ~0ull ? 0 : g_draw_min),
                (unsigned long long)g_draw_max,
                (unsigned long long)g_draw_frames);
    fprintf(f, "\n");
    if (g_drawmiss_n) {
        fprintf(f, "  [vk]   *** THE CENSUS IS BLIND, and here is what it is "
                   "blind to: the guest resolved %u draw entry point(s) this "
                   "layer does NOT hook —", g_drawmiss_n);
        for (unsigned i = 0; i < g_drawmiss_n; i++)
            fprintf(f, " %s x%u", g_drawmiss[i].name, g_drawmiss[i].hits);
        fprintf(f, "%s. Every draw count above is a FLOOR.\n",
                g_drawmiss_over ? " (and the name table overflowed)" : "");
    } else {
        fprintf(f, "  [vk]   the guest resolved NO draw entry point this layer "
                   "does not hook, so the kinds above are all the kinds it can "
                   "use through our dispatch\n");
    }
}

// ---------------------------------------------------------------------------
// The TEXEL-BUFFER VIEW census — "does this guest create VkBufferViews at all,
// and does it ever create one at a NON-ZERO offset?"  (KL_VK_BUFVIEW_CENSUS)
// ---------------------------------------------------------------------------
//
// Round 24. Upstream PR #3 ships ~1900 lines of texel-buffer EMULATION on the
// claim that MoltenVK reads a VkBufferView at a non-zero offset wrong, which
// zeroes UE's manual-vertex-fetch reads and collapses geometry to the origin.
// Before porting any of that, the question that decides whether the candidate
// is even alive for THIS title has to be asked of THIS guest: it costs two
// hooks and no behaviour.
//
// Both hooks are pure pass-through — create calls MoltenVK's own entry point
// and records the shape it was asked for; destroy only decrements. Nothing is
// substituted, mirrored or rewritten, so a run with this on and a run with it
// off differ by log lines only.
//
// What is recorded, and why each of them:
//   - total created / destroyed / live: separates "created once at load" from
//     "churned per frame", which is the difference between a static pool and
//     the per-frame window churn PR #3's mechanism needs.
//   - created per frame (against the draw census's frame count): the same
//     question as a rate rather than a total.
//   - non-zero-offset count, and the min/max non-zero offset: the whole
//     candidate. Zero here kills it outright for this title.
//   - the alignment histogram (16 / 64 / 256 / 1024): if every non-zero offset
//     is already a multiple of the device's minTexelBufferOffsetAlignment then
//     Metal's own texture-from-buffer offset rule is satisfied and a rounding
//     fix has nothing to round.
//   - distinct (format, offset!=0, range) shapes, printed once each: enough to
//     recognise a manual-vertex-fetch pool without dumping every view.
#define KLVK_BVSHAPE_N 32
static struct {
    uint32_t     format;
    int          nonzero;
    VkDeviceSize range;
    uint64_t     hits;
} g_bv_shape[KLVK_BVSHAPE_N];
static unsigned g_bv_shape_n, g_bv_shape_over;

static uint64_t g_bv_created, g_bv_destroyed, g_bv_nonzero;
static uint64_t g_bv_win_created;            // created since the last window
static VkDeviceSize g_bv_off_min = ~(VkDeviceSize)0, g_bv_off_max;
static uint64_t g_bv_align[4];               // multiples of 16 / 64 / 256 / 1024
static uint64_t g_bv_unaligned16;            // not even a multiple of 16
static uint64_t g_bv_whole_size;             // range == VK_WHOLE_SIZE

static int bufviewc_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_BUFVIEW_CENSUS", 0);
    return v > 0;
}

static void klvk_bv_note(const VkBufferViewCreateInfo *ci) {
    if (!ci) return;
    g_bv_created++;
    g_bv_win_created++;
    int nz = ci->offset != 0;
    if (nz) {
        g_bv_nonzero++;
        if (ci->offset < g_bv_off_min) g_bv_off_min = ci->offset;
        if (ci->offset > g_bv_off_max) g_bv_off_max = ci->offset;
        if (ci->offset % 16)   g_bv_unaligned16++;
        if (!(ci->offset % 16))   g_bv_align[0]++;
        if (!(ci->offset % 64))   g_bv_align[1]++;
        if (!(ci->offset % 256))  g_bv_align[2]++;
        if (!(ci->offset % 1024)) g_bv_align[3]++;
    }
    if (ci->range == VK_WHOLE_SIZE) g_bv_whole_size++;
    for (unsigned i = 0; i < g_bv_shape_n; i++)
        if (g_bv_shape[i].format == (uint32_t)ci->format &&
            g_bv_shape[i].nonzero == nz && g_bv_shape[i].range == ci->range) {
            g_bv_shape[i].hits++;
            return;
        }
    if (g_bv_shape_n >= KLVK_BVSHAPE_N) { g_bv_shape_over++; return; }
    g_bv_shape[g_bv_shape_n].format  = (uint32_t)ci->format;
    g_bv_shape[g_bv_shape_n].nonzero = nz;
    g_bv_shape[g_bv_shape_n].range   = ci->range;
    g_bv_shape[g_bv_shape_n].hits    = 1;
    g_bv_shape_n++;
    // Once per DISTINCT shape, so a pool of hundreds of views prints a handful
    // of lines and the first one arrives long before the first census window.
    VKI("buffer-view shape #%u: format %u, offset %s (this one 0x%llx), "
        "range %llu\n", g_bv_shape_n, (unsigned)ci->format,
        nz ? "NON-ZERO" : "zero", (unsigned long long)ci->offset,
        (unsigned long long)ci->range);
}

static VkResult VKAPI_CALL klvk_CreateBufferView(VkDevice dev,
                                                 const VkBufferViewCreateInfo *ci,
                                                 const VkAllocationCallbacks *a,
                                                 VkBufferView *out) {
    klvk_bv_note(ci);
    PFN_vkCreateBufferView real = KLVK_DRAW_REAL(vkCreateBufferView);
    if (!real) { if (out) *out = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
    return real(dev, ci, a, out);
}

static void VKAPI_CALL klvk_DestroyBufferView(VkDevice dev, VkBufferView v,
                                              const VkAllocationCallbacks *a) {
    if (v != VK_NULL_HANDLE) g_bv_destroyed++;
    PFN_vkDestroyBufferView real = KLVK_DRAW_REAL(vkDestroyBufferView);
    if (real) real(dev, v, a);
}

static void klvk_bufview_report(FILE *f) {
    if (!f || !bufviewc_on()) return;
    if (!g_bv_created) {
        fprintf(f, "  [vk] texel-buffer view census: THIS GUEST HAS NOT "
                   "CREATED ONE VkBufferView — no uniform or storage texel "
                   "buffer exists in this run, so the non-zero-offset class is "
                   "DEAD for this title\n");
        return;
    }
    uint64_t win = g_bv_win_created;
    g_bv_win_created = 0;
    fprintf(f, "  [vk] texel-buffer view census: %llu created, %llu destroyed, "
               "%llu live | %llu in this window | %llu at a NON-ZERO offset "
               "(%.1f%%)\n",
            (unsigned long long)g_bv_created,
            (unsigned long long)g_bv_destroyed,
            (unsigned long long)(g_bv_created - g_bv_destroyed),
            (unsigned long long)win,
            (unsigned long long)g_bv_nonzero,
            g_bv_created ? 100.0 * (double)g_bv_nonzero / (double)g_bv_created : 0.0);
    if (g_draw_frames)
        fprintf(f, "  [vk]   rate: %.4f view(s) created per frame over %llu "
                   "frame(s) — %s\n",
                (double)g_bv_created / (double)g_draw_frames,
                (unsigned long long)g_draw_frames,
                win ? "views are still being created, so the pool is NOT static"
                    : "nothing was created in this window, so the pool looks "
                      "STATIC");
    if (g_bv_nonzero)
        fprintf(f, "  [vk]   non-zero offsets: min 0x%llx, max 0x%llx | "
                   "multiples of 16: %llu, 64: %llu, 256: %llu, 1024: %llu | "
                   "NOT a multiple of 16: %llu\n",
                (unsigned long long)g_bv_off_min,
                (unsigned long long)g_bv_off_max,
                (unsigned long long)g_bv_align[0],
                (unsigned long long)g_bv_align[1],
                (unsigned long long)g_bv_align[2],
                (unsigned long long)g_bv_align[3],
                (unsigned long long)g_bv_unaligned16);
    fprintf(f, "  [vk]   %u distinct (format, offset!=0, range) shape(s)%s, "
               "%llu view(s) with range = VK_WHOLE_SIZE\n",
            g_bv_shape_n,
            g_bv_shape_over ? " (and the shape table OVERFLOWED)" : "",
            (unsigned long long)g_bv_whole_size);
}

// ---------------------------------------------------------------------------
// The OCCLUSION QUERY census — "does the guest cull from query results, and do
// the results say anything?"
// ---------------------------------------------------------------------------
//
// Round 11 of the Into the Radius port. Austin's report on launch #3 is that
// floors, walls, flowers, fences and planks "flicker in and out of visibility
// as I move my head" — per-object, head-motion-correlated visibility, which is
// what a CULLING decision looks like and is not what any compositor fault
// looks like. UE4's mobile renderer decides that with hardware occlusion
// queries (libUE4.so carries FVulkanDevice::AcquireOcclusionQueryPool), and
// nothing in this runtime had ever observed one.
//
// It is a census, not a guard: every call is passed straight through and the
// numbers are the whole of it. What it answers is (a) does this guest use
// occlusion queries at all, (b) what fraction of the results it reads back are
// ZERO — "nothing of this object was visible", the answer that culls — and (c)
// whether vkGetQueryPoolResults ever returns VK_NOT_READY, which a renderer
// reading last frame's pool treats as "keep the old answer". KL_VK_QUERY_CENSUS=1
// turns it on; it is off by default because it sits on vkCmdBeginQuery, which
// is a hot call.
static uint64_t g_fsync_n, g_fsync_ns, g_fsync_worst, g_fsync_over1ms;

static int queryc_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_QUERY_CENSUS", 0) != 0;
    return v;
}
static uint32_t g_q_pools, g_q_pools_occl, g_q_begins, g_q_gets;
static uint32_t g_q_get_ok, g_q_get_notready, g_q_get_other;
static uint64_t g_q_results, g_q_results_zero;
static uint32_t g_q_maxcount;

static VkResult VKAPI_CALL klvk_CreateQueryPool(VkDevice dev,
                                                const VkQueryPoolCreateInfo *ci,
                                                const VkAllocationCallbacks *alloc,
                                                VkQueryPool *out) {
    PFN_vkCreateQueryPool real = (PFN_vkCreateQueryPool)(real_gdpa
        ? real_gdpa(dev, "vkCreateQueryPool") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (ci) {
        g_q_pools++;
        if (ci->queryType == VK_QUERY_TYPE_OCCLUSION) {
            g_q_pools_occl++;
            if (ci->queryCount > g_q_maxcount) g_q_maxcount = ci->queryCount;
        }
    }
    return real(dev, ci, alloc, out);
}

static void VKAPI_CALL klvk_CmdBeginQuery(VkCommandBuffer cb, VkQueryPool pool,
                                          uint32_t query, VkQueryControlFlags fl) {
    __atomic_add_fetch(&g_q_begins, 1, __ATOMIC_RELAXED);
    PFN_vkCmdBeginQuery real = g_devs[0] && real_gdpa
        ? (PFN_vkCmdBeginQuery)real_gdpa(g_devs[0]->dev, "vkCmdBeginQuery") : NULL;
    if (real) real(cb, pool, query, fl);
}

static VkResult VKAPI_CALL klvk_GetQueryPoolResults(VkDevice dev, VkQueryPool pool,
                                                    uint32_t first, uint32_t count,
                                                    size_t sz, void *data,
                                                    VkDeviceSize stride,
                                                    VkQueryResultFlags fl) {
    PFN_vkGetQueryPoolResults real = (PFN_vkGetQueryPoolResults)(real_gdpa
        ? real_gdpa(dev, "vkGetQueryPoolResults") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(dev, pool, first, count, sz, data, stride, fl);
    g_q_gets++;
    if (r == VK_SUCCESS)        g_q_get_ok++;
    else if (r == VK_NOT_READY) g_q_get_notready++;
    else                        g_q_get_other++;
    // Only a SUCCESS carries values worth reading, and only when the guest did
    // not ask for the availability word to be appended — with
    // WITH_AVAILABILITY every other slot is that flag, and counting it as a
    // sample would move the zero fraction for a reason that is not the guest's.
    if (r == VK_SUCCESS && data && stride &&
        !(fl & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)) {
        int wide = (fl & VK_QUERY_RESULT_64_BIT) != 0;
        for (uint32_t i = 0; i < count; i++) {
            size_t off = (size_t)((uint64_t)stride * i);
            if (off + (wide ? 8u : 4u) > sz) break;
            uint64_t v = wide ? *(const uint64_t *)(const void *)((const char *)data + off)
                              : *(const uint32_t *)(const void *)((const char *)data + off);
            g_q_results++;
            if (!v) g_q_results_zero++;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// The QUEUE census — "does the frame-seam drain cover every queue the guest
// submits on?"
// ---------------------------------------------------------------------------
//
// kl_vulkan_frame_done drains ONE queue: `d->queue`, which is
// vkGetDeviceQueue(dev, pQueueCreateInfos[0].queueFamilyIndex, 0). If the guest
// submits rendering work anywhere else — a second queue in the same family, a
// second family, a transfer/compute queue used for streaming uploads — that
// work is still in flight when the frame record is filed, and round 11's fix
// closes only part of the window it was built for.
//
// Nothing here changes behaviour. Both interceptions are the real call plus a
// counter, and neither reads or writes any Vulkan object. The instrument's own
// failure modes, stated so a clean answer can be weighed:
//
//   * It sees a submit's COMMAND BUFFER COUNT, not what is inside them. A queue
//     is reported as carrying work when it is handed command buffers, and the
//     family's own VK_QUEUE_GRAPHICS_BIT is printed beside it. It does not
//     decode a command buffer and does not claim to.
//   * A submit on a queue we never saw fetched is registered as
//     `family ?/index ?` and the report says UNKNOWN in capitals. That is the
//     loud failure: it means vkGetDeviceQueue went somewhere other than our
//     seam, and every family/index number in the report is then suspect.
//   * It is a HOST instrument unless a device build carries it. MoltenVK
//     decides how many families and queues exist, and macOS and visionOS are
//     two different MoltenVK runs. A clean host census is evidence about the
//     host; the vkCreateDevice line below is what would settle the device, and
//     it prints there too once one is built.
//   * It cannot see a submit that never reaches Vulkan at all (Metal encoded
//     directly). Nothing in this guest does that, but the census cannot say so.
#define KLVK_MAX_QUEUES 16
#define KLVK_MAX_QTIDS   6
typedef struct {
    VkQueue  h;
    uint32_t family, index;      // UINT32_MAX = never fetched through our seam
    uint64_t submits;            // vkQueueSubmit/2 calls
    uint64_t batches;            // VkSubmitInfo structs across those calls
    uint64_t cmdbufs;            // command buffers across those batches
    uint64_t empty;              // batches with NO command buffer (signal-only)
    uint64_t tids[KLVK_MAX_QTIDS];
    int      ntids, tid_overflow;
} klvk_qrec;
static klvk_qrec g_q[KLVK_MAX_QUEUES];
static int       g_nq;
static int       g_q_overflow;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;

static klvk_qrec *qrec_find(VkQueue q) {          // g_qlock held
    for (int i = 0; i < g_nq; i++) if (g_q[i].h == q) return &g_q[i];
    if (g_nq >= KLVK_MAX_QUEUES) { g_q_overflow++; return NULL; }
    klvk_qrec *r = &g_q[g_nq++];
    r->h = q; r->family = r->index = UINT32_MAX;
    return r;
}

static void qrec_note_fetch(VkQueue q, uint32_t family, uint32_t index) {
    if (!q) return;
    pthread_mutex_lock(&g_qlock);
    klvk_qrec *r = qrec_find(q);
    if (r) { r->family = family; r->index = index; }
    pthread_mutex_unlock(&g_qlock);
}

static void qrec_note_submit(VkQueue q, uint64_t batches, uint64_t cmdbufs,
                             uint64_t empty) {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    pthread_mutex_lock(&g_qlock);
    klvk_qrec *r = qrec_find(q);
    if (r) {
        r->submits++;
        r->batches += batches;
        r->cmdbufs += cmdbufs;
        r->empty   += empty;
        int seen = 0;
        for (int i = 0; i < r->ntids; i++) if (r->tids[i] == tid) { seen = 1; break; }
        if (!seen) {
            if (r->ntids < KLVK_MAX_QTIDS) r->tids[r->ntids++] = tid;
            else r->tid_overflow++;
        }
    }
    pthread_mutex_unlock(&g_qlock);
}

static void VKAPI_CALL klvk_GetDeviceQueue(VkDevice dev, uint32_t family,
                                           uint32_t index, VkQueue *out) {
    PFN_vkGetDeviceQueue real = (PFN_vkGetDeviceQueue)(real_gdpa
        ? real_gdpa(dev, "vkGetDeviceQueue") : NULL);
    if (!real) { if (out) *out = VK_NULL_HANDLE; return; }
    real(dev, family, index, out);
    if (out) qrec_note_fetch(*out, family, index);
}

static void VKAPI_CALL klvk_GetDeviceQueue2(VkDevice dev,
                                            const VkDeviceQueueInfo2 *qi,
                                            VkQueue *out) {
    PFN_vkGetDeviceQueue2 real = (PFN_vkGetDeviceQueue2)(real_gdpa
        ? real_gdpa(dev, "vkGetDeviceQueue2") : NULL);
    if (!real) { if (out) *out = VK_NULL_HANDLE; return; }
    real(dev, qi, out);
    if (out && qi) qrec_note_fetch(*out, qi->queueFamilyIndex, qi->queueIndex);
}

static VkResult perf_split_submit(PFN_vkQueueSubmit real, VkQueue q, uint32_t n,
                                  const VkSubmitInfo *si, VkFence fence) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = si[i].commandBufferCount;
        uint32_t parts = k ? k : 1;
        for (uint32_t j = 0; j < parts; j++) {
            VkSubmitInfo one = si[i];
            if (j) { one.waitSemaphoreCount = 0; one.pWaitSemaphores = NULL;
                     one.pWaitDstStageMask = NULL; }
            if (j + 1 < parts) { one.signalSemaphoreCount = 0; one.pSignalSemaphores = NULL; }
            one.commandBufferCount = k ? 1 : 0;
            one.pCommandBuffers = k ? &si[i].pCommandBuffers[j] : NULL;
            // A timeline-semaphore/protected pNext names values per semaphore
            // array; split only plain submits, pass the rest through whole.
            if (si[i].pNext && parts > 1) {
                kl_perf_gpu_tag(kl_perf_gpu_guest_tag());
                VkResult r = real(q, 1, &si[i], (i + 1 == n) ? fence : VK_NULL_HANDLE);
                if (r != VK_SUCCESS) return r;
                break;
            }
            uint32_t passes = 0, copies = 0;
            uint64_t px = 0;
            if (k) {
                unsigned s = cbs_slot(si[i].pCommandBuffers[j], 0);
                if (s != UINT32_MAX) { passes = g_cbs[s].passes; copies = g_cbs[s].copies; px = g_cbs[s].px; }
            }
            int tag = kl_perf_gpu_guest_tag();
            kl_perf_ord_note(tag, passes, copies, px);
            kl_perf_gpu_tag(tag);
            VkResult r = real(q, 1, &one, (i + 1 == n && j + 1 == parts) ? fence : VK_NULL_HANDLE);
            if (r != VK_SUCCESS) return r;
        }
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL klvk_QueueSubmit(VkQueue q, uint32_t n,
                                            const VkSubmitInfo *si, VkFence fence) {
    uint64_t cb = 0, empty = 0;
    for (uint32_t i = 0; si && i < n; i++) {
        cb += si[i].commandBufferCount;
        if (!si[i].commandBufferCount) empty++;
    }
    qrec_note_submit(q, n, cb, empty);
    for (uint32_t i = 0; si && i < n; i++)
        hold_check_submit(si[i].pCommandBuffers, si[i].commandBufferCount);
    klvk_device *d = g_devs[0];
    PFN_vkQueueSubmit real = (d && d->QueueSubmit) ? d->QueueSubmit
        : (PFN_vkQueueSubmit)(real_gdpa && d ? real_gdpa(d->dev, "vkQueueSubmit") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    hold_submit_delay();
    for (uint32_t i = 0; si && i < n; i++)
        hold_mark_submit(si[i].pCommandBuffers, si[i].commandBufferCount, 1);
    // kl_perf: the Metal command buffer MoltenVK commits inside this call is the
    // guest's, numbered by its order in the frame — unless a caller of ours
    // (kl_openvr's eye copy) already said whose it is.
    int tag_was = kl_perf_gpu_tag(0);
    VkResult r;
    int64_t t0 = tag_was ? 0 : kl_perf_now_ns();
    if (!tag_was && perf_split_on() && si && n) r = perf_split_submit(real, q, n, si, fence);
    else {
        kl_perf_gpu_tag(tag_was ? tag_was : kl_perf_gpu_guest_tag());
        r = real(q, n, si, fence);
    }
    if (!tag_was) kl_perf_guest_submit(kl_perf_now_ns() - t0);
    kl_perf_gpu_tag(tag_was);
    for (uint32_t i = 0; si && i < n; i++)
        hold_mark_submit(si[i].pCommandBuffers, si[i].commandBufferCount, 0);
    return r;
}

static VkResult VKAPI_CALL klvk_QueueSubmit2(VkQueue q, uint32_t n,
                                             const VkSubmitInfo2 *si, VkFence fence) {
    uint64_t cb = 0, empty = 0;
    for (uint32_t i = 0; si && i < n; i++) {
        cb += si[i].commandBufferInfoCount;
        if (!si[i].commandBufferInfoCount) empty++;
    }
    qrec_note_submit(q, n, cb, empty);
    for (uint32_t i = 0; si && i < n; i++)
        for (uint32_t j = 0; j < si[i].commandBufferInfoCount && si[i].pCommandBufferInfos; j++)
            hold_check_submit(&si[i].pCommandBufferInfos[j].commandBuffer, 1);
    klvk_device *d = g_devs[0];
    PFN_vkQueueSubmit2 real = (PFN_vkQueueSubmit2)(real_gdpa && d
        ? real_gdpa(d->dev, "vkQueueSubmit2") : NULL);
    if (!real && real_gdpa && d)
        real = (PFN_vkQueueSubmit2)real_gdpa(d->dev, "vkQueueSubmit2KHR");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    int tag_was = kl_perf_gpu_tag(0);
    kl_perf_gpu_tag(tag_was ? tag_was : kl_perf_gpu_guest_tag());
    int64_t t0 = tag_was ? 0 : kl_perf_now_ns();
    VkResult r = real(q, n, si, fence);
    if (!tag_was) kl_perf_guest_submit(kl_perf_now_ns() - t0);
    kl_perf_gpu_tag(tag_was);
    return r;
}

static void queue_census_report(FILE *f) {
    if (!f) return;
    pthread_mutex_lock(&g_qlock);
    int nq = g_nq;
    klvk_qrec snap[KLVK_MAX_QUEUES];
    memcpy(snap, g_q, sizeof snap);
    int over = g_q_overflow;
    pthread_mutex_unlock(&g_qlock);

    VkQueue drained = g_ndev ? g_devs[0]->queue : VK_NULL_HANDLE;
    uint64_t work_off_drain = 0;
    int unknown = 0;

    fprintf(f, "  [vk] queue census: %d distinct VkQueue handle(s) ever submitted on "
               "or fetched%s. The frame seam drains %p.\n",
            nq, over ? " (TABLE OVERFLOWED — the numbers below are incomplete)" : "",
            (void *)drained);
    for (int i = 0; i < nq; i++) {
        const klvk_qrec *r = &snap[i];
        const char *gfx = "family properties unknown";
        if (r->family != UINT32_MAX && r->family < g_nqfam)
            gfx = (g_qfam[r->family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                ? "GRAPHICS family" : "NOT a graphics family";
        if (r->family == UINT32_MAX) unknown++;
        if (r->h != drained) work_off_drain += r->cmdbufs;
        fprintf(f, "  [vk]   queue %p ", (void *)r->h);
        if (r->family == UINT32_MAX) fprintf(f, "family UNKNOWN/index UNKNOWN — "
            "SUBMITTED ON BUT NEVER FETCHED THROUGH OUR vkGetDeviceQueue ");
        else fprintf(f, "family %u index %u (%s) ", r->family, r->index, gfx);
        fprintf(f, ": %llu submit(s), %llu batch(es), %llu command buffer(s), "
                   "%llu signal-only batch(es), on %d thread(s)%s%s\n",
                (unsigned long long)r->submits, (unsigned long long)r->batches,
                (unsigned long long)r->cmdbufs, (unsigned long long)r->empty,
                r->ntids, r->tid_overflow ? "+ (more than we track)" : "",
                r->h == drained ? "  <-- THIS is the one the frame seam drains" : "");
        for (int t = 0; t < r->ntids; t++)
            fprintf(f, "  [vk]     thread %llu\n", (unsigned long long)r->tids[t]);
    }
    if (!nq)
        fprintf(f, "  [vk]   NOTHING WAS COUNTED. Either the guest is not on the "
                   "Vulkan path or vkQueueSubmit did not come through this seam — "
                   "do not read that as 'one queue'.\n");
    else if (unknown)
        fprintf(f, "  [vk]   VERDICT: %d queue(s) were submitted on that we never "
                   "handed out. The family/index numbers cannot be trusted and the "
                   "drain's coverage is UNPROVEN.\n", unknown);
    else if (work_off_drain)
        fprintf(f, "  [vk]   VERDICT: %llu command buffer(s) were submitted on a "
                   "queue the frame seam does NOT drain. Round 11's ordering is "
                   "INCOMPLETE.\n", (unsigned long long)work_off_drain);
    else
        fprintf(f, "  [vk]   VERDICT: every command buffer this guest submitted "
                   "went to the queue the frame seam drains. Round 11's drain is "
                   "COMPLETE for this run.\n");
}

static void query_census_report(FILE *f);

// The three LOG-ONLY riders of round 18, on ONE 15-second clock — the depth
// census, the draw census, and the format-substitution table's occupancy.
//
// The clock is the point, and it is a device concern rather than a host one.
// Both report paths reach this: the host's query_census_tick below, and the
// DEVICE's kl_vulkan_memory_report, which the heartbeat calls ONCE A SECOND
// (kl_app.c, `ticks % 4` at a 250 ms tick). Four lines a second over a
// six-minute session is ~1400 lines of census, and the device log rotates —
// which is exactly how launch #6's six-minute log came to be unavailable when
// round 16 went looking for it. A shared static means a run cannot print these
// twice in one window no matter which path ticks first.
//
// The substitution line rides here rather than on the image-format line
// (kl_vulkan_memory_report), because that line only prints when the set of
// distinct formats has GROWN — so an overflow arriving after the last new
// format would never be announced at all. Round 17's table audit named
// KLVK_SUBST_N its one weak suspect precisely because it degrades SILENTLY;
// an instrument for it that can itself stay silent is no instrument. It
// therefore prints every window, zeros included: a zero here is the answer.
static void klvk_rc_report(FILE *f);   // dynamic-rendering pass census (below)
static void cons_report(FILE *f);      // ...who reads its resolve targets (below)
static void klvk_census_riders(FILE *f) {
    if (!f) return;
    static uint64_t last;
    uint64_t now = (uint64_t)time(NULL);
    if (last && now - last < 15) return;
    last = now;
    klvk_depth_report(f);
    klvk_draw_report(f);
    klvk_geom_report(f);
    klvk_ddiff_report(f);
    klvk_bufview_report(f);
    hflag_report(f);
    klvk_rc_report(f);
    cons_report(f);
    fprintf(f, "  [vk] format substitution: %u live of %d slots, %u DROPPED ON "
               "OVERFLOW%s\n",
            g_subst_n, KLVK_SUBST_N, g_subst_overflow,
            g_subst_overflow ? " — a dropped entry means its views were built "
                               "with the WRONG format" : " (the table has "
                               "never overflowed in this run)");
}

// Every 15 s from the frame seam, because nothing on the HOST run path calls
// kl_vulkan_memory_report (see the indexed-draw guard's own note beside it) and
// a census that only ever prints on device is not an instrument.
static void query_census_tick(void) {
    // The depth census is NOT gated on KL_VK_QUERY_CENSUS and rides its own
    // 15 s clock, because the question it answers is not optional.
    klvk_census_riders(stderr);
    if (!queryc_on()) return;
    static uint64_t last;
    uint64_t now = (uint64_t)time(NULL);
    if (last && now - last < 15) return;
    last = now;
    query_census_report(stderr);
    queue_census_report(stderr);
    if (g_fsync_n)
        VKI("frame seam: the guest's queue was still busy at ovrp_EndFrame4 for "
            "%.2f ms mean / %.2f ms worst over %llu frames, %llu of them over "
            "1 ms — that is the window in which a compositor selecting on the "
            "frame record can sample an eye texture the GPU is still writing\n",
            (double)g_fsync_ns / (double)g_fsync_n / 1e6,
            (double)g_fsync_worst / 1e6,
            (unsigned long long)g_fsync_n, (unsigned long long)g_fsync_over1ms);
}

static void query_census_report(FILE *f) {
    if (!f || !queryc_on()) return;
    if (!g_q_pools && !g_q_gets) {
        fprintf(f, "  [vk] occlusion queries: the guest created NO query pool and "
                   "read NO results — hardware occlusion culling is not in use "
                   "here, so it cannot be what makes objects come and go\n");
        return;
    }
    fprintf(f, "  [vk] occlusion queries: %u pool(s) (%u OCCLUSION, worst %u "
               "queries), %u vkCmdBeginQuery, %u result reads (%u SUCCESS, %u "
               "NOT_READY, %u other) | %llu value(s) read, %llu ZERO (%.1f%% — "
               "a zero is 'nothing of this object was visible', i.e. the answer "
               "that CULLS)\n",
            g_q_pools, g_q_pools_occl, g_q_maxcount, g_q_begins, g_q_gets,
            g_q_get_ok, g_q_get_notready, g_q_get_other,
            (unsigned long long)g_q_results, (unsigned long long)g_q_results_zero,
            g_q_results ? 100.0 * (double)g_q_results_zero / (double)g_q_results
                        : 0.0);
}

static void idx_guard_report(FILE *f) {
    if (!f || !g_idx_draws) return;
    fprintf(f, "  [vk] indexed draws: %u recorded, %u DROPPED for having no index "
               "buffer bound%s\n", g_idx_draws, g_idx_dropped,
            g_idx_over ? " (the command-buffer table saturated — the guard stopped "
                         "dropping and the count is a floor)" : "");
}


// The format-property queries, passed through untouched and LOGGED. This is
// the measurement that decides whether path (a) was ever available: if the
// next log shows the guest asking about the format it went on to create, then
// it asked, was told zero, and created it anyway — and substitution is the
// only fix. If it shows no query at all, Unity is using a static table and the
// same conclusion follows by a different road.
// ---------------------------------------------------------------------------
// The capability answers, and round 13's actual fix.
//
// Round 12 passed both of these through untouched, on the reasoning that
// MoltenVK's honest "unsupported" was the strongest possible hint and Unity
// built the image anyway. That reading was half right. Unity DID take the
// answer — it just did not take it as a hint. It marked the GraphicsFormat
// unsupported in its own capability table, and when the punch-time render pass
// asked for a depth format, the table answered NONE, which its
// GraphicsFormat->VkFormat translation spells VK_FORMAT_UNDEFINED. Launch #12's
// log carries the whole chain: exactly one depth/stencil refusal all boot
// (`vkGetPhysicalDeviceImageFormatProperties(127 S8_UINT) ... -11`) and then,
// 1300 lines later, a 5088x3234 two-layer `depth-att|transient|input-att` image
// of format 0.
//
// So the answer has to be SUPPORTED, and it has to be backed. Every one of
// these formats has a byte-safe substitute at vkCreateImage
// (D24_UNORM_S8_UINT, D16_UNORM_S8_UINT and S8_UINT all land on
// D32_SFLOAT_S8_UINT, which every Apple GPU has), so reporting the family
// supported is not a lie the runtime cannot keep: the guest asks for the
// format, gets an image, and the image is the substitute.
// ---------------------------------------------------------------------------

// VK_FORMAT_D16_UNORM .. VK_FORMAT_D32_SFLOAT_S8_UINT — the whole depth and
// stencil block, contiguous in the core enum.
static int fmt_is_depth_stencil(int f) { return f >= 124 && f <= 130; }

// KL_VK_DEPTH_NOFILTER=1 (test knob): the depth/stencil family answers
// WITHOUT VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, the way MoltenVK
// answers on the headset (D32_SFLOAT optimal 0xce01 there, 0xde01 on the Mac).
static void depth_nofilter_apply(VkFormatProperties *out) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_DEPTH_NOFILTER", 0);
    if (on && out) out->optimalTilingFeatures &= ~(VkFormatFeatureFlags)
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}

static void klvk_GetPhysicalDeviceFormatProperties(VkPhysicalDevice phys, VkFormat f,
                                                   VkFormatProperties *out) {
    PFN_vkGetPhysicalDeviceFormatProperties real =
        (PFN_vkGetPhysicalDeviceFormatProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceFormatProperties");
    if (!real || !out) return;
    real(phys, f, out);
    int zero = !(out->linearTilingFeatures | out->optimalTilingFeatures
                 | out->bufferFeatures);

    // The depth/stencil family answers for its substitute, not for itself.
    if (zero && fmt_is_depth_stencil((int)f)) {
        int differs = 0;
        int sub = kl_vulkan_format_substitute((int)f, &differs);
        VkFormatProperties p;
        memset(&p, 0, sizeof p);
        if (sub) real(phys, (VkFormat)sub, &p);
        if (p.linearTilingFeatures | p.optimalTilingFeatures | p.bufferFeatures) {
            *out = p;
            depth_nofilter_apply(out);
            static int said_fp[8];
            int si = (int)f - 124;
            if (si >= 0 && si < 8 && !said_fp[si]++)
                VKI("guest asked vkGetPhysicalDeviceFormatProperties(%d %s), which "
                    "Metal cannot spell — answering with %d %s's features instead, "
                    "which is the image vkCreateImage will actually hand back\n",
                    (int)f, kl_vulkan_format_name((int)f),
                    sub, kl_vulkan_format_name(sub));
            return;
        }
    }

    // Every depth/stencil query is logged whether it was refused or not. Round
    // 12 logged only refusals, so the log could say S8_UINT was turned down but
    // not what else Unity had asked about the family — and the format that came
    // back UNDEFINED came out of exactly that gap.
    if (fmt_is_depth_stencil((int)f)) {
        depth_nofilter_apply(out);
        static int said_ds[8];
        int si = (int)f - 124;
        if (si >= 0 && si < 8 && !said_ds[si]++)
            VKI("guest asked vkGetPhysicalDeviceFormatProperties(%d %s) -> "
                "linear 0x%x optimal 0x%x buffer 0x%x\n",
                (int)f, kl_vulkan_format_name((int)f),
                (unsigned)out->linearTilingFeatures,
                (unsigned)out->optimalTilingFeatures,
                (unsigned)out->bufferFeatures);
        return;
    }
    if (!zero) return;
    static uint32_t asked;
    if (asked++ < 24)
        VKI("guest asked vkGetPhysicalDeviceFormatProperties(%d %s) and was told "
            "UNSUPPORTED (all three feature masks zero)\n",
            (int)f, kl_vulkan_format_name((int)f));
}

static VkResult klvk_GetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice phys, VkFormat f, VkImageType type, VkImageTiling tiling,
        VkImageUsageFlags usage, VkImageCreateFlags flags,
        VkImageFormatProperties *out) {
    PFN_vkGetPhysicalDeviceImageFormatProperties real =
        (PFN_vkGetPhysicalDeviceImageFormatProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceImageFormatProperties");
    if (!real) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkResult r = real(phys, f, type, tiling, usage, flags, out);

    // Refused, and in the family we can back. Ask again for the substitute and
    // hand its answer over: the guest is about to create an image of `f`, and
    // what it will actually get is `sub`, so `sub`'s limits are the true ones.
    if (r != VK_SUCCESS && fmt_is_depth_stencil((int)f)) {
        int differs = 0;
        int sub = kl_vulkan_format_substitute((int)f, &differs);
        if (sub) {
            VkImageFormatProperties p;
            memset(&p, 0, sizeof p);
            VkResult r2 = real(phys, (VkFormat)sub, type, tiling, usage, flags, &p);
            if (r2 == VK_SUCCESS) {
                if (out) *out = p;
                static int said_ifp[8];
                int si = (int)f - 124;
                if (si >= 0 && si < 8 && !said_ifp[si]++)
                    VKI("guest asked vkGetPhysicalDeviceImageFormatProperties(%d %s) "
                        "and Metal refused it — answering SUPPORTED on behalf of %d "
                        "%s, which is what vkCreateImage substitutes. Refusing here "
                        "is what made Unity's GraphicsFormat table answer NONE, and "
                        "NONE is VK_FORMAT_UNDEFINED at the next vkCreateImage.\n",
                        (int)f, kl_vulkan_format_name((int)f),
                        sub, kl_vulkan_format_name(sub));
                return VK_SUCCESS;
            }
        }
    }

    // Every query, with its arguments, capped. Two of these appeared in all of
    // launch #12 and neither said what was being asked FOR; a refusal is only
    // interpretable next to the usage and tiling that provoked it.
    static uint32_t asked;
    if (asked++ < 64) {
        char u[128];
        VKI("guest asked vkGetPhysicalDeviceImageFormatProperties(%d %s, %uD, "
            "tiling %s, usage %s, flags 0x%x) -> %d\n",
            (int)f, kl_vulkan_format_name((int)f), (unsigned)type + 1,
            tiling == VK_IMAGE_TILING_LINEAR ? "linear" : "optimal",
            usage_words(usage, u, sizeof u), (unsigned)flags, r);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Render passes — the third place that has to agree with vkCreateImage.
//
// A render pass names the FORMAT of every attachment, and Vulkan requires that
// format to match the image view bound to it in the framebuffer. Round 12
// rewrote the image and the view and stopped there, which was survivable only
// because nothing unmappable had ever reached an attachment slot. Launch #12's
// crashing image is an attachment — `depth-att|transient|input-att` — so the
// pass that consumes it names the same unusable format, and a
// VK_FORMAT_UNDEFINED attachment is a descriptor with pixelFormat 0 by another
// road.
//
// Both the 1.0 and the 1.2 forms, because Unity picks by device version and a
// missed one routes straight past the seam.
// ---------------------------------------------------------------------------
static int layout_is_depth_stencil(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
        return 1;
    default:
        return 0;
    }
}

// One attachment's replacement format, or 0 to leave it alone. An attachment
// has no usage field, so the last resort is steered by the LAYOUTS instead —
// which is the same question asked a different way: an attachment that lives in
// a depth layout is a depth attachment.
static VkFormat attachment_sub(klvk_device *d, VkFormat f,
                               VkImageLayout initial, VkImageLayout final_) {
    if (!d || !fmt_unmappable(d, f)) return (VkFormat)0;
    int differs = 0;
    int sub = kl_vulkan_format_substitute((int)f, &differs);
    if (!sub) {
        unsigned usage = (layout_is_depth_stencil(initial)
                          || layout_is_depth_stencil(final_))
                       ? KL_VK_USAGE_DEPTH_STENCIL_ATTACHMENT
                       : KL_VK_USAGE_COLOR_ATTACHMENT;
        sub = kl_vulkan_format_last_resort((int)f, usage, &differs);
    }
    if (!sub) return (VkFormat)0;
    static int said[8];
    unsigned i = (unsigned)f < 8 ? (unsigned)f : 7;
    if (!said[i]++)
        VKI("render-pass attachment of format %d %s rewritten to %d %s, to agree "
            "with the image vkCreateImage substitutes\n",
            (int)f, kl_vulkan_format_name((int)f), sub, kl_vulkan_format_name(sub));
    return (VkFormat)sub;
}

// Does any subpass actually DECLARE an input attachment?
//
// This is the safety case for dropping VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT off
// transient attachments (kl_vulkan.h). Unity sets that bit on every URP
// attachment unconditionally; whether a subpass ever reads one back is a
// different question, and it is the question that decides whether the drop can
// be observed at all. A forward URP pipeline declares none. If this ever says
// otherwise, the line names itself and KL_VK_LAZY_KEEP_INPUT=1 is the backout.
static void rp_note_inputs(uint32_t subpasses, uint32_t inputs) {
    static int said_none, said_some;
    if (inputs) {
        if (!said_some++)
            fprintf(stderr,
                    "  [vk] render pass declares %u INPUT ATTACHMENT(s) across %u "
                    "subpass(es) — a subpass really does read an attachment back. "
                    "If the picture is wrong or Metal complains about a texture "
                    "without ShaderRead, KL_VK_LAZY_KEEP_INPUT=1 restores the "
                    "usage bit (and with it ~1.7 GiB of transient memory)\n",
                    inputs, subpasses);
        return;
    }
    if (!said_none++)
        fprintf(stderr,
                "  [vk] render passes so far declare NO input attachments (%u "
                "subpass(es) in the first one) — the INPUT_ATTACHMENT bit Unity "
                "puts on every attachment is unused, which is what makes "
                "dropping it for memoryless storage free\n", subpasses);
}

static VkResult klvk_CreateRenderPass_(VkDevice dev, const VkRenderPassCreateInfo *ci,
                                       const VkAllocationCallbacks *alloc,
                                       VkRenderPass *out) {
    klvk_device *d = dev_find(dev);
    PFN_vkCreateRenderPass real =
        (PFN_vkCreateRenderPass)(real_gdpa ? real_gdpa(dev, "vkCreateRenderPass") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (ci && ci->pSubpasses) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < ci->subpassCount; i++)
            n += ci->pSubpasses[i].inputAttachmentCount;
        rp_note_inputs(ci->subpassCount, n);
    }
    if (!ci || !ci->attachmentCount || !ci->pAttachments || !d)
        return real(dev, ci, alloc, out);

    VkAttachmentDescription *copy = NULL;
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        VkFormat sub = attachment_sub(d, ci->pAttachments[i].format,
                                      ci->pAttachments[i].initialLayout,
                                      ci->pAttachments[i].finalLayout);
        if (!sub) continue;
        if (!copy) {
            copy = (VkAttachmentDescription *)malloc(sizeof *copy * ci->attachmentCount);
            if (!copy) return real(dev, ci, alloc, out);
            memcpy(copy, ci->pAttachments, sizeof *copy * ci->attachmentCount);
        }
        copy[i].format = sub;
    }
    if (!copy) return real(dev, ci, alloc, out);
    VkRenderPassCreateInfo mine = *ci;
    mine.pAttachments = copy;
    VkResult r = real(dev, &mine, alloc, out);
    free(copy);
    return r;
}

static VkResult klvk_CreateRenderPass2_(VkDevice dev, const VkRenderPassCreateInfo2 *ci,
                                        const VkAllocationCallbacks *alloc,
                                        VkRenderPass *out) {
    klvk_device *d = dev_find(dev);
    PFN_vkCreateRenderPass2 real =
        (PFN_vkCreateRenderPass2)(real_gdpa ? real_gdpa(dev, "vkCreateRenderPass2") : NULL);
    if (!real && real_gdpa)
        real = (PFN_vkCreateRenderPass2)real_gdpa(dev, "vkCreateRenderPass2KHR");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (ci && ci->pSubpasses) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < ci->subpassCount; i++)
            n += ci->pSubpasses[i].inputAttachmentCount;
        rp_note_inputs(ci->subpassCount, n);
    }
    if (!ci || !ci->attachmentCount || !ci->pAttachments || !d)
        return real(dev, ci, alloc, out);

    VkAttachmentDescription2 *copy = NULL;
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        VkFormat sub = attachment_sub(d, ci->pAttachments[i].format,
                                      ci->pAttachments[i].initialLayout,
                                      ci->pAttachments[i].finalLayout);
        if (!sub) continue;
        if (!copy) {
            copy = (VkAttachmentDescription2 *)malloc(sizeof *copy * ci->attachmentCount);
            if (!copy) return real(dev, ci, alloc, out);
            memcpy(copy, ci->pAttachments, sizeof *copy * ci->attachmentCount);
        }
        copy[i].format = sub;
    }
    if (!copy) return real(dev, ci, alloc, out);
    VkRenderPassCreateInfo2 mine = *ci;
    mine.pAttachments = copy;
    VkResult r = real(dev, &mine, alloc, out);
    free(copy);
    return r;
}

// The render-pass shapes kl_fove's eye-pass check needs (round G3): recorded
// only when KL_FOVEATION is on, from the guest's own create info.
static VkResult klvk_CreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo *ci,
                                      const VkAllocationCallbacks *alloc, VkRenderPass *out) {
    VkResult r = klvk_CreateRenderPass_(dev, ci, alloc, out);
    if (r == VK_SUCCESS && out) fove_rp_note1(ci, *out);
    return r;
}
static VkResult klvk_CreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2 *ci,
                                       const VkAllocationCallbacks *alloc, VkRenderPass *out) {
    VkResult r = klvk_CreateRenderPass2_(dev, ci, alloc, out);
    if (r == VK_SUCCESS && out) fove_rp_note2(ci, *out);
    return r;
}

// ---------------------------------------------------------------------------
// Swapchain — ours end to end
// ---------------------------------------------------------------------------
static uint32_t mem_type(klvk_device *d, uint32_t bits, VkMemoryPropertyFlags want) {
    PFN_vkGetPhysicalDeviceMemoryProperties gmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    if (gmp) gmp(d->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkResult klvk_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
        const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    (void)alloc;
    klvk_device *d = dev_find(dev);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;

    klvk_swapchain *sc = calloc(1, sizeof *sc);
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->magic = KLVK_SWAP_MAGIC;
    sc->d = d;
    sc->format = ci->imageFormat;
    sc->extent = ci->imageExtent;
    sc->n = ci->minImageCount < 2 ? 2 : ci->minImageCount;
    if (sc->n > KLVK_MAX_IMAGES) sc->n = KLVK_MAX_IMAGES;

    for (uint32_t i = 0; i < sc->n; i++) {
        VkImageCreateInfo ic = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = sc->format,
            .extent = { sc->extent.width, sc->extent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = ci->imageArrayLayers ? ci->imageArrayLayers : 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            // TRANSFER_SRC on top of whatever the guest asked for: the capture
            // copies out of exactly these images.
            .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = ci->imageSharingMode,
            .queueFamilyIndexCount = ci->queueFamilyIndexCount,
            .pQueueFamilyIndices = ci->pQueueFamilyIndices,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkResult r = d->CreateImage(dev, &ic, NULL, &sc->images[i]);
        if (r != VK_SUCCESS) { VKI("swapchain image %u: %d\n", i, r); goto fail; }

        VkMemoryRequirements mr;
        d->GetImageMemoryRequirements(dev, sc->images[i], &mr);
        uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
        };
        r = d->AllocateMemory(dev, &ai, NULL, &sc->mem[i]);
        if (r != VK_SUCCESS) { VKI("swapchain memory %u: %d\n", i, r); goto fail; }
        sc->bytes[i] = (uint64_t)mr.size;
        ledger_add(KLVK_LEDGER_SWAP, sc->bytes[i]);
        r = d->BindImageMemory(dev, sc->images[i], sc->mem[i], 0);
        if (r != VK_SUCCESS) { VKI("swapchain bind %u: %d\n", i, r); goto fail; }
    }

    *out = (VkSwapchainKHR)sc;
    for (uint32_t i = 0; i < sc->n; i++) hold_born((uint64_t)(uintptr_t)sc->images[i]);
    VKI("vkCreateSwapchainKHR -> %u image(s) %ux%u fmt %d (ours, no presentation engine; "
        "oldSwapchain %p)\n", sc->n, sc->extent.width, sc->extent.height, (int)sc->format,
        (void *)ci->oldSwapchain);
    return VK_SUCCESS;
fail:
    free(sc);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static klvk_swapchain *swap_of(VkSwapchainKHR h) {
    klvk_swapchain *s = (klvk_swapchain *)h;
    return (s && s->magic == KLVK_SWAP_MAGIC) ? s : NULL;
}

static void klvk_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR h,
                                     const VkAllocationCallbacks *alloc) {
    (void)alloc;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return;
    klvk_device *d = sc->d;
    if (d->DeviceWaitIdle) d->DeviceWaitIdle(dev);
    VKI("vkDestroySwapchainKHR %p (%u image(s))\n", (void *)h, sc->n);
    for (uint32_t i = 0; i < sc->n; i++) {
        if (sc->images[i]) hold_killed_image(sc->images[i]);
        if (sc->images[i]) d->DestroyImage(dev, sc->images[i], NULL);
        if (sc->mem[i]) d->FreeMemory(dev, sc->mem[i], NULL);
        if (sc->bytes[i]) ledger_sub(KLVK_LEDGER_SWAP, sc->bytes[i]);
    }
    // The readback scratch is the DEVICE's, not this swapchain's — it outlives
    // any one swapchain and is shared with the eye path.
    sc->magic = 0;
    free(sc);
}

static VkResult klvk_GetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR h,
        uint32_t *count, VkImage *out) {
    (void)dev;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return VK_ERROR_INITIALIZATION_FAILED;
    if (!out) { *count = sc->n; return VK_SUCCESS; }
    uint32_t give = *count < sc->n ? *count : sc->n;
    memcpy(out, sc->images, give * sizeof *out);
    *count = give;
    return give < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}

// Signal whatever the guest asked to be signalled, with no work attached.
//
// This is the one place a real presentation engine is genuinely being stood in
// for: an acquire hands back an image AND a signal that it is safe to use. With
// no engine there is nothing to wait for, so the signal is immediate — an empty
// submit is the only legal way to signal a binary semaphore from the host side.
//
// It goes on the guest's own queue, which is a shared resource: this is safe
// because acquire and submit are called from the same thread in every renderer
// that exists (Vulkan queues are externally synchronized and Unity's render
// thread owns this one). Worth naming, because a second presenting thread would
// make it a race with no error surface.
static VkResult klvk_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR h,
        uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index) {
    (void)timeout;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return VK_ERROR_OUT_OF_DATE_KHR;
    klvk_device *d = sc->d;

    *index = sc->next;
    sc->next = (sc->next + 1) % sc->n;

    if (sem || fence) {
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = sem ? 1u : 0u,
            .pSignalSemaphores = sem ? &sem : NULL,
        };
        VkResult r = d->QueueSubmit(d->queue, 1, &si, fence);
        if (r != VK_SUCCESS) VKT("acquire signal submit: %d\n", r);
    }
    (void)dev;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Present — and the capture
// ---------------------------------------------------------------------------
// Readback scratch big enough for `need` bytes, on the device rather than on a
// swapchain: the OVRPlugin eye path has no swapchain at all and needs the same
// machinery, and the two never capture at once.
static int cap_prepare(klvk_device *d, VkDeviceSize need) {
    if (d->cap_buf && d->cap_size >= need) return 1;
    if (d->cap_buf) {                         // grew — tear the old one down
        if (d->DeviceWaitIdle) d->DeviceWaitIdle(d->dev);
        d->DestroyBuffer(d->dev, d->cap_buf, NULL);
        d->FreeMemory(d->dev, d->cap_mem, NULL);
        d->cap_buf = VK_NULL_HANDLE; d->cap_mem = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = need,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (d->CreateBuffer(d->dev, &bi, NULL, &d->cap_buf) != VK_SUCCESS) return 0;
    d->cap_size = need;

    VkMemoryRequirements mr;
    d->GetBufferMemoryRequirements(d->dev, d->cap_buf, &mr);
    uint32_t mt = mem_type(d, mr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX)
        mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mt == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
    };
    if (d->AllocateMemory(d->dev, &ai, NULL, &d->cap_mem) != VK_SUCCESS) return 0;
    if (d->BindBufferMemory(d->dev, d->cap_buf, d->cap_mem, 0) != VK_SUCCESS) return 0;

    if (d->cap_pool) return 1;                // pool/cmd/fence are size-independent
    VkCommandPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->queue_family,
    };
    if (d->CreateCommandPool(d->dev, &pi, NULL, &d->cap_pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d->cap_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (d->AllocateCommandBuffers(d->dev, &cbi, &d->cap_cmd) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (d->CreateFence(d->dev, &fi, NULL, &d->cap_fence) != VK_SUCCESS) return 0;
    return 1;
}

static const char *cap_dir(void) { return kl_env_str("KL_VK_OUT", NULL); }

// Copy any image out and write it as a PNG.
//
// `cur` is the layout the guest left the image in, and getting it wrong is
// cheap here in a way it would not be on a real driver: **MoltenVK does not
// implement image layouts** — Metal has no such concept — so the barrier is in
// practice a memory barrier and the transition is bookkeeping. It is still
// written correctly, because this file is also the model for the device path.
// `layer` is the array slice to read. It is 0 for every 2D image here and 0/1
// for the two eyes of an Array-layout eye image, and it has to travel all the
// way into the copy region: a barrier and a copy that both say slice 0 read the
// LEFT eye twice and produce two identical PNGs, which reads exactly like a
// guest that renders one eye — the failure this capture exists to distinguish.
static int cap_image_layer(klvk_device *d, VkQueue q, VkImage img, uint32_t w, uint32_t h,
                           VkFormat fmt, VkImageLayout cur, uint32_t layer,
                           const char *path,
                           const VkSemaphore *waits, uint32_t nwait) {
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (!cap_prepare(d, need)) { VKI("capture setup failed\n"); return 0; }

    d->ResetCommandBuffer(d->cap_cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    d->BeginCommandBuffer(d->cap_cmd, &bi);

    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = cur,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 },
    };
    d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { w, h, 1 },
    };
    d->CmdCopyImageToBuffer(d->cap_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            d->cap_buf, 1, &region);

    // Put it back where the guest left it, or its next render pass starts from
    // a layout it did not expect.
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = cur;
    d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    d->EndCommandBuffer(d->cap_cmd);

    // Waiting on the caller's semaphores does double duty: it orders the copy
    // after the guest's rendering, and it CONSUMES signals that would otherwise
    // stay high forever — a binary semaphore nobody waits on is a validation
    // error and, on a real driver, a hang two frames later.
    VkPipelineStageFlags *stages = NULL;
    if (nwait) {
        stages = calloc(nwait, sizeof *stages);
        for (uint32_t i = 0; i < nwait; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = nwait,
        .pWaitSemaphores = nwait ? waits : NULL,
        .pWaitDstStageMask = stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &d->cap_cmd,
    };
    d->ResetFences(d->dev, 1, &d->cap_fence);
    VkResult r = d->QueueSubmit(q, 1, &si, d->cap_fence);
    free(stages);
    if (r != VK_SUCCESS) { VKI("capture submit: %d\n", r); return 0; }
    d->WaitForFences(d->dev, 1, &d->cap_fence, VK_TRUE, 2000000000ull);

    void *mapped = NULL;
    if (d->MapMemory(d->dev, d->cap_mem, 0, need, 0, &mapped) != VK_SUCCESS) return 0;
    VkMappedMemoryRange mmr = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = d->cap_mem, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    if (d->InvalidateMappedMemoryRanges) d->InvalidateMappedMemoryRanges(d->dev, 1, &mmr);

    uint8_t *px = mapped;
    size_t n = (size_t)w * h;
    if (fmt_is_bgra(fmt))
        for (size_t i = 0; i < n; i++) {
            uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t;
        }
    // Alpha is not a coverage value here. A guest that never authors alpha
    // leaves it 0, and a correct RGB frame then
    // writes out as a fully TRANSPARENT PNG, which is visually identical to
    // black and has no error surface at all.
    for (size_t i = 0; i < n; i++) px[i * 4 + 3] = 255;

    // Is there anything in it? A dark frame and a never-written frame look the
    // same in a file browser, and this is the whole question the arc is asking.
    uint64_t sum = 0;
    size_t lit = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned v = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        sum += v;
        if (v) lit++;
    }

    int ok = png_write_rgba(path, px, (int)w, (int)h);
    d->UnmapMemory(d->dev, d->cap_mem);
    if (ok) {
        g_captured++;
        VKI("-> %s (%zu lit / %zu, mean %.1f)\n", path, lit, n,
            n ? (double)sum / (double)(n * 3) : 0.0);
    } else {
        VKI("PNG write failed (%s)\n", path);
    }
    return ok;
}

static int cap_image(klvk_device *d, VkQueue q, VkImage img, uint32_t w, uint32_t h,
                     VkFormat fmt, VkImageLayout cur, const char *path,
                     const VkSemaphore *waits, uint32_t nwait) {
    return cap_image_layer(d, q, img, w, h, fmt, cur, 0, path, waits, nwait);
}

static void cap_frame(klvk_swapchain *sc, VkQueue q, uint32_t idx,
                      const VkSemaphore *waits, uint32_t nwait) {
    char path[1024];
    snprintf(path, sizeof path, "%s/vk_%05u.png", cap_dir(), g_presented);
    cap_image(sc->d, q, sc->images[idx], sc->extent.width, sc->extent.height,
              sc->format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, path, waits, nwait);
}

// ---------------------------------------------------------------------------
// The OVRPlugin eye layer — the path this guest actually takes
// ---------------------------------------------------------------------------
//
// BONELAB never creates a WSI swapchain. Unity's Oculus XR path asks OVRPlugin
// for its eye textures instead (`ovrp_GetLayerTexture2`) and submits them to the
// Oculus compositor, so `VK_KHR_swapchain` above is exercised by nothing on this
// title — it stays because it is the general answer for a flat Vulkan guest and
// because the two share every line of the capture below.
//
// kl_ovrp.c owns that seam and has always answered with GL texture names. On the
// Vulkan path those names go straight into `vkCreateImageView` as VkImage
// handles, which is a segfault inside MoltenVK with a GL name for an address.
// So the eye textures have to be real VkImages, and this is where they are made.
// Two eye LAYOUTS reach here, and they differ in how many images exist rather
// than in anything about one image:
//
//   Stereo (ovrpLayout 0) — one 2D image per (stage, eye), array layer 0.
//   Array  (ovrpLayout 3) — ONE image per stage with two array layers, handed
//                           back for both eyes; the eye IS the layer index.
//
// The second is what Unity calls Single Pass Instanced / Multiview, and it is
// how this title ships on a Quest. `layers` is therefore a property of the
// stage, not of the eye: kl_ovrp.c passes 2 for Array and 1 for Stereo, and the
// slot for eye 1 is left empty under Array so nothing can hand out a second
// image that does not exist.
#define KLVK_EYE_STAGES 4
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    uint32_t       w, h;
    uint32_t       layers;
    VkFormat       fmt;
    // What vkGetImageMemoryRequirements said, which is what was actually
    // charged to the process — not w*h*bpp. The ledger below is only worth
    // reading if it reports the number the kernel meters.
    uint64_t       bytes;
} klvk_img_slot;

static klvk_img_slot g_eye[KLVK_EYE_STAGES][2];

// ...and the OTHER layers, which until now shared the table above.
//
// `ovrp_GetLayerTexture2` names a LAYER, and the storage was keyed on (stage,
// eye) alone — so every layer the guest set up was handed the eye layer's
// images. For a Unity guest that was invisible, because its only other layer is
// a 1x1 dummy nothing renders into. An Unreal guest has real ones: OculusHMD's
// FSplash and its IStereoLayers quads are how RE4 draws its intro logos, and
// they were being rendered straight into the corner of the eye image — a small
// logo in the top-left of an otherwise black eye, which reads as a broken
// projection rather than as a layer with no storage of its own.
//
// Small and fixed: a guest that runs out says so by name rather than silently
// aliasing again, which is the failure this replaces.
#define KLVK_MAX_LAYERS 8
static struct {
    int           key;                       // the guest's layer id; 0 = free
    klvk_img_slot s[KLVK_EYE_STAGES][2];
} g_layer_img[KLVK_MAX_LAYERS];

int kl_vulkan_guest_active(void) { return g_avail && g_ndev > 0; }

// ---------------------------------------------------------------------------
// The compositor seam — the MTLTexture behind an eye VkImage
// ---------------------------------------------------------------------------
//
// Both compositors (KleptonCompositor.swift on device, kl_view_mtl.m on the
// host) sample an `id<MTLTexture>` the guest rendered into, found through
// `kl_glfb_eye_mtl_texture`. On the GL path the host ALLOCATES that texture and
// ANGLE is told to use it as the eye's storage. On the Vulkan path the
// direction is the other way round and it is simpler: MoltenVK has already
// backed our VkImage with an MTLTexture, so there is nothing to import,
// negotiate a format for, or keep in step — we ask for the one that exists.
//
// **That asymmetry is the whole reason there is no provider call here.** An
// import (`VkImportMetalTextureInfoEXT`, or the deprecated `vkSetMTLTextureMVK`)
// would let the host choose the storage, and buys nothing: this eye image is
// never a drawable, the compositor only ever reads it, and a host-chosen
// texture would have to agree with the VkFormat exactly — the same
// "Incompatible format" class kl_glfb.h records for ANGLE, in an API where the
// failure is silent instead.
//
// Two entry points can answer, and they are tried in that order:
//
//   vkExportMetalObjectsEXT   VK_EXT_metal_objects, the standard one. Needs the
//                             extension ENABLED on the device, which is why
//                             klvk_CreateDevice adds it.
//   vkGetMTLTextureMVK        MoltenVK's own, deprecated but a plain dylib
//                             export that needs no extension — the fallback for
//                             a device created before this existed.
//
// Failure is not fatal anywhere: the capture path reads the VkImage directly
// and does not need this at all. A run with no compositor (every host run today)
// simply publishes a texture nobody samples.
static void *eye_mtl_texture(klvk_device *d, VkImage img) {
    if (!d || !img) return NULL;

    // vkGetMTLTextureMVK's own prototype lives in MoltenVK/mvk_deprecated_api.h,
    // which is an Objective-C header (`id<MTLTexture> *`) and cannot be included
    // from this plain-C file. The ABI is a pointer either way, so it is declared
    // here rather than dragging the translation unit into ObjC for one symbol.
    typedef void (*pfn_get_mtl_texture_mvk)(VkImage, void **);

    static PFN_vkExportMetalObjectsEXT export_ext;
    static pfn_get_mtl_texture_mvk     get_mvk;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        export_ext = (PFN_vkExportMetalObjectsEXT)
            (real_gdpa ? real_gdpa(d->dev, "vkExportMetalObjectsEXT") : NULL);
        get_mvk = (pfn_get_mtl_texture_mvk)mvk_sym("vkGetMTLTextureMVK");
        if (!export_ext && !get_mvk)
            VKI("no way to reach the MTLTexture behind an eye image — neither "
                "vkExportMetalObjectsEXT nor vkGetMTLTextureMVK resolves; the "
                "compositor will have nothing to sample (the capture is "
                "unaffected)\n");
    }

    if (export_ext) {
        VkExportMetalTextureInfoEXT tex = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .image = img,
            .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        };
        VkExportMetalObjectsInfoEXT info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &tex,
        };
        export_ext(d->dev, &info);
        if (tex.mtlTexture) return (void *)tex.mtlTexture;
    }
    if (get_mvk) {
        void *t = NULL;
        get_mvk(img, &t);
        return t;
    }
    return NULL;
}

// Tell kl_glfb which MTLTexture is which eye, so every compositor and readback
// finds the Vulkan eyes through the same accessor it already uses for the GL
// ones. Under the Array layout one texture serves both eyes and the eye is the
// SLICE, which is exactly what that table's `slice` field was added for.
static void eye_publish_mtl(int stage, int eye, int layers) {
    klvk_device *d = g_devs[0];
    VkImage img = g_eye[stage][eye].img;
    void *tex = eye_mtl_texture(d, img);
    if (!tex) return;
    int w = (int)g_eye[stage][eye].w, h = (int)g_eye[stage][eye].h;
    if (layers > 1) {
        kl_glfb_note_eye_mtl_texture(0, stage, tex, 0, w, h);
        kl_glfb_note_eye_mtl_texture(1, stage, tex, 1, w, h);
    } else {
        kl_glfb_note_eye_mtl_texture(eye, stage, tex, 0, w, h);
    }
}

// ---------------------------------------------------------------------------
// The image ledger, and the graveyard
// ---------------------------------------------------------------------------
// Round 10 (Paint the Town Red VR, launch #9) died of a footprint that went
// 620 MiB -> 7170 MiB in two seconds, and the tripwire's region walk could
// account for only 1.5 GiB of it. That is not the walk being wrong: an image
// backed by MTLStorageModePrivate lives in an MTLHeap that is charged to
// phys_footprint in full and mapped into the address space not at all, so a
// vm_region_recurse sweep is structurally blind to every byte of it. The
// process can be four gigabytes over budget with nothing on screen to say so.
//
// So keep our own count. Every image this file allocates goes through
// klvk_image_alloc and every one it releases goes through klvk_image_free, and
// between them they are the whole of what Klepton asks the GPU for. What is
// left over in the tripwire dump — total footprint, minus the region walk,
// minus this — is the GUEST's Vulkan allocations, which is the number that was
// missing when round 10 started and the one that named the cause.
static const char *const k_ledger_name[KLVK_LEDGER_N] = {
    "eye", "spacewarp", "layer", "swapchain", "openxr", "scratch",
};
static uint64_t g_ledger_bytes[KLVK_LEDGER_N];
static uint32_t g_ledger_n[KLVK_LEDGER_N];
static uint64_t g_ledger_retired;      // bytes waiting in the graveyard
static uint64_t g_ledger_freed;        // bytes the graveyard has handed back

static void ledger_add(int kind, uint64_t bytes) {
    if (kind < 0 || kind >= KLVK_LEDGER_N) return;
    __atomic_add_fetch(&g_ledger_bytes[kind], bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_ledger_n[kind], 1, __ATOMIC_RELAXED);
}
static void ledger_sub(int kind, uint64_t bytes) {
    if (kind < 0 || kind >= KLVK_LEDGER_N) return;
    __atomic_sub_fetch(&g_ledger_bytes[kind], bytes, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&g_ledger_n[kind], 1, __ATOMIC_RELAXED);
}

// A superseded image is NOT freed where it is replaced. The guest may hold a
// Unity render texture wrapping it, and a compositor on another thread may be
// sampling it this instant; freeing storage under either is a worse failure
// than holding an image that no longer describes anything. That rule stays.
//
// What round 10 changed is that "not freed" used to mean "never freed". Paint
// the Town Red VR sets its eye layer up twice — 5088x3234, then 7584x4851 once
// Unity applies its render scale — and the first generation, 473 MiB of eye and
// SpaceWarp storage, stayed resident for the life of the process with nothing
// holding a reference to it. So retire instead: park the pair here, and free it
// once the guest has submitted KL_VK_RETIRE_FRAMES whole frames since. A frame
// end is a queue-idle point (kl_vulkan_frame_done), so at 120 frames the GPU
// has been idle a hundred and twenty times since anything could have referenced
// it. That is not a race that can be lost.
#define KLVK_GRAVE_N 32
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    uint64_t       bytes;
    unsigned long long frame;   // the serial it was retired at
    int            kind;
} klvk_grave;
static klvk_grave g_grave[KLVK_GRAVE_N];

static void klvk_image_free(klvk_device *d, VkImage img, VkDeviceMemory mem) {
    if (!d) return;
    if (img) hold_killed_image(img);
    if (img) { fove_image_gone(img); fove_xr_image_gone(img); }
    if (img) d->DestroyImage(d->dev, img, NULL);
    if (mem) d->FreeMemory(d->dev, mem, NULL);
}

// Hand a slot's storage to the graveyard and blank the slot. Safe to call with
// an empty slot. When the graveyard is full the oldest entry is freed to make
// room — it is by definition the one that has been unreferenced longest.
static void klvk_retire_slot(klvk_device *d, klvk_img_slot *slot, int kind) {
    if (!slot || !slot->img) { if (slot) { slot->img = VK_NULL_HANDLE; slot->mem = VK_NULL_HANDLE; } return; }
    ledger_sub(kind, slot->bytes);
    int at = -1;
    for (int i = 0; i < KLVK_GRAVE_N; i++)
        if (!g_grave[i].img && !g_grave[i].mem) { at = i; break; }
    if (at < 0) {
        int oldest = 0;
        for (int i = 1; i < KLVK_GRAVE_N; i++)
            if (g_grave[i].frame < g_grave[oldest].frame) oldest = i;
        klvk_image_free(d, g_grave[oldest].img, g_grave[oldest].mem);
        g_ledger_retired -= g_grave[oldest].bytes;
        g_ledger_freed   += g_grave[oldest].bytes;
        at = oldest;
    }
    g_grave[at].img   = slot->img;
    g_grave[at].mem   = slot->mem;
    g_grave[at].bytes = slot->bytes;
    g_grave[at].kind  = kind;
    g_grave[at].frame = __atomic_load_n(&g_frame_serial, __ATOMIC_ACQUIRE);
    g_ledger_retired += slot->bytes;
    slot->img = VK_NULL_HANDLE;
    slot->mem = VK_NULL_HANDLE;
    slot->bytes = 0;
}

// Called from kl_vulkan_frame_done, i.e. with the guest's queue idle.
static void klvk_grave_sweep(klvk_device *d, unsigned long long now) {
    int keep = kl_env_int("KL_VK_RETIRE_FRAMES", 120);
    if (keep < 1) keep = 1;
    for (int i = 0; i < KLVK_GRAVE_N; i++) {
        if (!g_grave[i].img && !g_grave[i].mem) continue;
        if (now < g_grave[i].frame + (unsigned long long)keep) continue;
        VKI("released a superseded image %.0f MiB (%s) — %llu frames since it "
            "was replaced\n", (double)g_grave[i].bytes / (1024 * 1024),
            k_ledger_name[g_grave[i].kind],
            (unsigned long long)(now - g_grave[i].frame));
        klvk_image_free(d, g_grave[i].img, g_grave[i].mem);
        g_ledger_retired -= g_grave[i].bytes;
        g_ledger_freed   += g_grave[i].bytes;
        memset(&g_grave[i], 0, sizeof g_grave[i]);
    }
}

// [MTLDevice currentAllocatedSize] — the whole process's Metal footprint, in
// bytes, or 0 when it cannot be reached.
//
// Plain objc_msgSend rather than dragging this translation unit into ObjC for
// one selector, which is the same call the MTLTexture export next door already
// makes through MoltenVK's C entry point. The device is MoltenVK's own
// (vkGetMTLDeviceMVK off the physical device), not MTLCreateSystemDefaultDevice:
// on a machine with one GPU those are the same object, and asking the loader
// that made the allocations is the one that stays true if they ever are not.
static unsigned long long klvk_mtl_allocated(void) {
    typedef void (*pfn_get_mtl_device_mvk)(VkPhysicalDevice, void **);
    static pfn_get_mtl_device_mvk get_dev;
    static int resolved;
    if (!resolved) { resolved = 1; get_dev = (pfn_get_mtl_device_mvk)mvk_sym("vkGetMTLDeviceMVK"); }
    klvk_device *d = g_devs[0];
    if (!get_dev || !d || !d->phys) return 0;
    void *mtl = NULL;
    get_dev(d->phys, &mtl);
    if (!mtl) return 0;
    SEL sel = sel_registerName("currentAllocatedSize");
    if (!sel || !class_respondsToSelector(object_getClass((id)mtl), sel)) return 0;
    unsigned long (*send)(id, SEL) = (unsigned long (*)(id, SEL))(void *)objc_msgSend;
    return (unsigned long long)send((id)mtl, sel);
}

unsigned long long kl_vulkan_mtl_allocated_bytes(void) { return klvk_mtl_allocated(); }

// kl_perf's GPU clock wraps -commit on the command-buffer classes of MoltenVK's
// own MTLDevice, once, when the first device is registered.
static void klvk_perf_install(klvk_device *d) {
    typedef void (*pfn_get_mtl_device_mvk)(VkPhysicalDevice, void **);
    pfn_get_mtl_device_mvk get_dev = (pfn_get_mtl_device_mvk)mvk_sym("vkGetMTLDeviceMVK");
    void *mtl = NULL;
    if (get_dev && d && d->phys) get_dev(d->phys, &mtl);
    if (mtl) kl_perf_gpu_install(mtl);
    if (mtl) kl_fove_install(mtl);      // KL_FOVEATION (default off: returns at once)
    if (mtl) kl_shc_install(mtl);       // KL_SHC=1 (default off); AFTER kl_fove (outer wrapper)
    // The MoltenVK settings that decide where the CPU goes, read back from the
    // running library (round N14: what is set, not what the release notes say).
    typedef VkResult (*pfn_getcfg)(VkInstance, MVKConfiguration *, size_t *);
    pfn_getcfg getcfg = (pfn_getcfg)mvk_sym("vkGetMoltenVKConfigurationMVK");
    MVKConfiguration c;
    size_t sz = sizeof c;
    memset(&c, 0, sizeof c);
    if (getcfg && getcfg(VK_NULL_HANDLE, &c, &sz) >= VK_SUCCESS)
        VKI("MoltenVK config: synchronousQueueSubmits %u, prefillMetalCommandBuffers %d, "
            "maxActiveMetalCommandBuffersPerQueue %u, fastMath %d, useMetalArgumentBuffers %u, "
            "useMTLHeap %d, useCommandPooling %u, shouldMaximizeConcurrentCompilation %u, "
            "performanceTracking %u, semaphoreSupportStyle %d (KL_MVKCFG_<NAME> sets them)\n",
            c.synchronousQueueSubmits, (int)c.prefillMetalCommandBuffers,
            c.maxActiveMetalCommandBuffersPerQueue, (int)c.fastMathEnabled,
            c.useMetalArgumentBuffers, (int)c.useMTLHeap, c.useCommandPooling,
            c.shouldMaximizeConcurrentCompilation, c.performanceTracking,
            (int)c.semaphoreSupportStyle);
}

// Create an image and back it with device-local memory.
//
// Shared by the two callers that need one — the OVRPlugin eye images below and
// the OpenXR swapchain images (kl_vulkan_xr_image) — because the sequence is
// identical and only the description differs. Written out twice it would be two
// copies of the memory-type fallback, which is exactly the kind of thing that
// drifts and then differs on the path nobody ran.
// `kind` and `bytes_out` are the ledger's half: the caller says which bucket
// this image belongs to and gets back what it actually cost, so the slot can
// carry the number that will be un-charged when it is retired. Passed rather
// than stashed in a global, because two threads can be in here at once and a
// global would mislabel whichever one lost.
static VkImage klvk_image_alloc(klvk_device *d, VkFormat fmt,
                                unsigned w, unsigned h, uint32_t layers,
                                uint32_t mips, VkImageUsageFlags usage,
                                VkDeviceMemory *mem_out,
                                int kind, uint64_t *bytes_out) {
    if (bytes_out) *bytes_out = 0;
    if (mem_out) *mem_out = VK_NULL_HANDLE;
    VkImageCreateInfo ic = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { w, h, 1 },
        .mipLevels = mips ? mips : 1,
        .arrayLayers = layers ? layers : 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img = VK_NULL_HANDLE;
    if (d->CreateImage(d->dev, &ic, NULL, &img) != VK_SUCCESS) {
        VKI("image %ux%u (%u layer(s)): vkCreateImage failed\n", w, h, layers);
        return VK_NULL_HANDLE;
    }
    VkMemoryRequirements mr;
    d->GetImageMemoryRequirements(d->dev, img, &mr);
    uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (d->AllocateMemory(d->dev, &ai, NULL, &mem) != VK_SUCCESS ||
        d->BindImageMemory(d->dev, img, mem, 0) != VK_SUCCESS) {
        VKI("image %ux%u: memory failed\n", w, h);
        d->DestroyImage(d->dev, img, NULL);
        return VK_NULL_HANDLE;
    }
    if (mem_out) *mem_out = mem;
    if (bytes_out) *bytes_out = (uint64_t)mr.size;
    ledger_add(kind, (uint64_t)mr.size);
    hold_born((uint64_t)(uintptr_t)img);
    return img;
}

// A non-eye layer's slot table, found or claimed by the guest's layer id.
// NULL when the table is full, which is named by the caller.
static klvk_img_slot *layer_slots_for(int key) {
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (g_layer_img[i].key == key) return &g_layer_img[i].s[0][0];
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (!g_layer_img[i].key) {
            g_layer_img[i].key = key;
            return &g_layer_img[i].s[0][0];
        }
    return NULL;
}

unsigned long long kl_vulkan_layer_image(int layer_key, int stage, int eye,
                                         unsigned w, unsigned h, int srgb, int layers) {
    if (!kl_vulkan_guest_active()) return 0;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return 0;
    if (!w || !h) return 0;
    if (layers < 1) layers = 1;
    if (layers > 2) layers = 2;
    // Under the Array layout there is one image for the stage and both eyes get
    // it; slot 0 owns it, so the eye argument selects a LAYER later and not a
    // second allocation here.
    if (layers > 1) eye = 0;
    klvk_device *d = g_devs[0];

    int is_eye = layer_key == KLVK_EYE_LAYER;
    klvk_img_slot *slots = is_eye ? &g_eye[0][0] : layer_slots_for(layer_key);
    if (!slots) {
        VKI("layer %d: all %d layer image slots are taken — refusing rather than "
            "handing back another layer's storage\n", layer_key, KLVK_MAX_LAYERS);
        return 0;
    }
    klvk_img_slot *slot = is_eye ? &g_eye[stage][eye] : &slots[stage * 2 + eye];

    VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    // A cached image is only the right answer if it is the image that was
    // ASKED for. RE4 sets up its eye layer twice — 2290x2400, then 2748x2880
    // once UE4 applies its own pixel density — and the size was not consulted,
    // so the second layer was handed the first one's storage and the engine
    // rendered a 2748x2880 frame into a 2290x2400 image.
    //
    // The old image is not destroyed HERE. It is the GL path's rule for the GL
    // path's reason: a compositor or a capture may be sampling it on another
    // thread this instant, and freeing storage under them is a worse failure
    // than holding a few images that no longer describe anything. It is retired
    // to the graveyard instead and released a hundred and twenty queue-idle
    // frames later — see klvk_retire_slot for the 473 MiB that used to stay
    // resident forever, and the jetsam it contributed to.
    if (slot->img && (slot->w != w || slot->h != h ||
                      slot->layers != (uint32_t)layers || slot->fmt != fmt)) {
        VKI("layer %d stage %d %s: was %ux%u (%u layer(s)), now %ux%u (%d) — "
            "allocating fresh; the old image (%.0f MiB) is RETIRED, and released "
            "once the guest has submitted KL_VK_RETIRE_FRAMES frames past it\n",
            layer_key, stage, eye ? "eye 1" : "eye 0",
            slot->w, slot->h, slot->layers, w, h, layers,
            (double)slot->bytes / (1024 * 1024));
        klvk_retire_slot(d, slot, is_eye ? KLVK_LEDGER_EYE : KLVK_LEDGER_LAYER);
    }
    if (slot->img) return (uint64_t)(uintptr_t)slot->img;

    // The guest renders into it, samples it for its own post chain, and we copy
    // out of it. TRANSFER_DST is there because Unity clears some targets with
    // vkCmdClearColorImage rather than a load op.
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t bytes = 0;
    VkImage img = klvk_image_alloc(d, fmt, w, h, (uint32_t)layers, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT, &mem,
                                   is_eye ? KLVK_LEDGER_EYE : KLVK_LEDGER_LAYER,
                                   &bytes);
    if (img == VK_NULL_HANDLE) return 0;
    // KL_VK_EYE_TINT=1 pre-clears the image to a colour nothing in a rendered
    // scene produces, which is the A/B that separates the two ways an eye
    // texture can come back wrong. They are indistinguishable in the capture
    // otherwise: if the tint survives to the PNG, the guest never drew into
    // this image at all; if it does not, the guest drew and the content is
    // genuinely what it rendered. Off by default — it costs a submit per image
    // and it is a diagnostic, not a fix.
    if (kl_env_on("KL_VK_EYE_TINT", 0) && d->CmdClearColorImage &&
        cap_prepare(d, 4)) {
        VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        d->ResetCommandBuffer(d->cap_cmd, 0);
        d->BeginCommandBuffer(d->cap_cmd, &bi);
        VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
            // EVERY layer: under the Array layout the two eyes are layers of
            // this one image, and tinting only layer 0 would leave the right
            // eye's "did the guest draw?" question unanswered.
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)layers },
        };
        d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        // A DIFFERENT colour per array layer, and that is the second question
        // this diagnostic answers. Under the Array layout the two eyes are two
        // slices of one image, and "the guest drew" and "the capture reads the
        // right slice" are separate failures that look identical in the files:
        // a capture that reads layer 0 twice produces two byte-identical PNGs,
        // which is exactly what a guest rendering only the left eye produces
        // too. Green for eye 0, blue for eye 1 — so a run says which.
        static const VkClearColorValue TINT[2] = {
            { .float32 = { 0.0f, 1.0f, 0.0f, 1.0f } },   // eye 0: green
            { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } },   // eye 1: blue
        };
        for (int ly = 0; ly < layers; ly++) {
            VkImageSubresourceRange rng =
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, (uint32_t)ly, 1 };
            d->CmdClearColorImage(d->cap_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &TINT[layers > 1 ? ly : (eye & 1)], 1, &rng);
        }
        d->EndCommandBuffer(d->cap_cmd);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &d->cap_cmd };
        d->ResetFences(d->dev, 1, &d->cap_fence);
        if (d->QueueSubmit(d->queue, 1, &si, d->cap_fence) == VK_SUCCESS)
            d->WaitForFences(d->dev, 1, &d->cap_fence, VK_TRUE, 2000000000ull);
        VKI("eye image stage %d pre-cleared: %s (KL_VK_EYE_TINT)\n", stage,
            layers > 1 ? "layer 0 GREEN, layer 1 BLUE"
                       : (eye ? "eye 1 BLUE" : "eye 0 GREEN"));
    }

    slot->img = img;
    slot->mem = mem;
    slot->w = w;
    slot->h = h;
    slot->layers = (uint32_t)layers;
    slot->fmt = fmt;
    slot->bytes = bytes;
    // Only the EYE layer is published into kl_glfb's eye table. That table is
    // what every compositor and every readback samples, so a splash quad
    // announcing itself there would replace the eye on screen with a logo.
    if (is_eye) eye_publish_mtl(stage, eye, layers);
    VKI("%s stage %d %s = VkImage %p (%ux%u %s)\n",
        is_eye ? "eye image" : "layer image",
        stage,
        layers > 1 ? "both eyes (2 array layers)" :
                     (eye ? "eye 1" : "eye 0"),
        (void *)img, w, h, srgb ? "R8G8B8A8_SRGB" : "R8G8B8A8_UNORM");
    return (uint64_t)(uintptr_t)img;
}

unsigned long long kl_vulkan_eye_image_layers(int stage, int eye, unsigned w, unsigned h,
                                              int srgb, int layers) {
    return kl_vulkan_layer_image(KLVK_EYE_LAYER, stage, eye, w, h, srgb, layers);
}

// Application SpaceWarp's motion-vector storage. See the header for why it
// exists at all; this is only the allocation.
//
// Kept in a table of its own rather than in g_layer_img: those slots are keyed
// by the guest's LAYER id and hand back one image per (stage, eye), and the
// SpaceWarp pair is two more images for the SAME layer, stage and eye. Reusing
// the layer table would have meant inventing two synthetic layer ids that no
// guest ever names — a second vocabulary for the same thing, and the exact
// shape of aliasing bug the layer table was introduced to end.
//
// Neither image is published into kl_glfb's eye table: nothing composites a
// motion vector, and a target the guest draws garbage into announcing itself as
// an eye is how a frame goes green.
static klvk_img_slot g_spacewarp[KLVK_EYE_STAGES][2][2];   // [stage][eye][depth?]

unsigned long long kl_vulkan_spacewarp_image(int stage, int eye,
                                             unsigned w, unsigned h,
                                             int layers, int depth) {
    if (!kl_vulkan_guest_active()) return 0;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return 0;
    if (!w || !h) return 0;
    if (layers < 1) layers = 1;
    if (layers > 2) layers = 2;
    // Same rule as the eye images under ovrpLayout_Array: one image with a
    // layer per eye, owned by slot 0.
    if (layers > 1) eye = 0;
    klvk_device *d = g_devs[0];
    if (!d) return 0;

    // The colour format is R16G16B16A16_SFLOAT because that is the
    // ovrpTextureFormat the provider asks for (2, R16G16B16A16_FP — measured in
    // ovrp_CalculateEyeLayerDesc3's motionVectorFormat argument), and the depth
    // one is D32_SFLOAT_S8_UINT because Apple GPUs have no D24_UNORM_S8_UINT at
    // all: MoltenVK reports it unsupported, and asking for it is a vkCreateImage
    // failure rather than a slower path.
    VkFormat fmt = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT
                         : VK_FORMAT_R16G16B16A16_SFLOAT;
    klvk_img_slot *slot = &g_spacewarp[stage][eye][depth ? 1 : 0];
    if (slot->img && (slot->w != w || slot->h != h ||
                      slot->layers != (uint32_t)layers || slot->fmt != fmt)) {
        // The eye table's rule, for the eye table's reason: the old image is not
        // freed under the guest, which may still hold a Unity render texture
        // wrapping it. It is retired and released later instead.
        VKI("spacewarp %s stage %d: was %ux%u (%u layer(s)), now %ux%u (%d) — "
            "allocating fresh; the old image (%.0f MiB) is RETIRED\n",
            depth ? "depth" : "motion vectors", stage,
            slot->w, slot->h, slot->layers, w, h, layers,
            (double)slot->bytes / (1024 * 1024));
        klvk_retire_slot(d, slot, KLVK_LEDGER_SPACEWARP);
    }
    if (slot->img) return (uint64_t)(uintptr_t)slot->img;

    VkImageUsageFlags usage = depth
        ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
        : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t bytes = 0;
    VkImage img = klvk_image_alloc(d, fmt, w, h, (uint32_t)layers, 1, usage, &mem,
                                   KLVK_LEDGER_SPACEWARP, &bytes);
    if (img == VK_NULL_HANDLE) return 0;
    slot->img    = img;
    slot->mem    = mem;
    slot->w      = w;
    slot->h      = h;
    slot->layers = (uint32_t)layers;
    slot->fmt    = fmt;
    slot->bytes  = bytes;
    VKI("spacewarp %s stage %d %s = VkImage %p (%ux%u %s)\n",
        depth ? "depth" : "motion vectors", stage,
        layers > 1 ? "both eyes (2 array layers)" : (eye ? "eye 1" : "eye 0"),
        (void *)img, w, h,
        depth ? "D32_SFLOAT_S8_UINT" : "R16G16B16A16_SFLOAT");
    return (uint64_t)(uintptr_t)img;
}

void *kl_vulkan_layer_mtl_texture(int layer_key, int stage, int eye,
                                  int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!kl_vulkan_guest_active()) return NULL;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return NULL;
    // Never the eye table: those go to a compositor through kl_glfb's eye table,
    // which is where every consumer already looks. This is only the OTHER
    // layers, which have no such table because nothing sampled them before.
    if (layer_key == KLVK_EYE_LAYER) return NULL;
    klvk_img_slot *slots = NULL;
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (g_layer_img[i].key == layer_key) { slots = &g_layer_img[i].s[0][0]; break; }
    if (!slots) return NULL;
    klvk_img_slot *slot = &slots[stage * 2 + eye];
    // A layer allocated for eye 0 only (the common case: one texture serving
    // both eyes, with the per-eye ViewportRect selecting the part) answers eye 1
    // from slot 0 rather than with nothing.
    if (!slot->img && eye == 1) slot = &slots[stage * 2];
    if (!slot->img) return NULL;
    if (w) *w = (int)slot->w;
    if (h) *h = (int)slot->h;
    return eye_mtl_texture(g_devs[0], slot->img);
}

unsigned long long kl_vulkan_eye_image(int stage, int eye, unsigned w, unsigned h,
                                       int srgb) {
    return kl_vulkan_eye_image_layers(stage, eye, w, h, srgb, 1);
}

// ---------------------------------------------------------------------------
// The OpenXR seam — XR_KHR_vulkan_enable. See kl_vulkan.h for why it exists.
// ---------------------------------------------------------------------------

// Whether the extension may be advertised. Deliberately NOT kl_vulkan_guest_active():
// that asks whether a device exists, and at xrCreateInstance time the guest has
// not created one yet — it cannot, because the whole point of the extension is
// that it asks US which physical device to create it on. So the question here
// is only whether MoltenVK is reachable at all.
int kl_vulkan_xr_supported(void) { return kl_vulkan_available(); }

// Empty, and true rather than lazy — klvk_CreateDevice already adds
// VK_EXT_metal_objects to every device the guest creates, so there is nothing
// left to require of the app. If something is ever genuinely required here,
// note that the spec wants a SPACE-delimited list in one buffer, not an array.
const char *kl_vulkan_xr_instance_extensions(void) { return ""; }
const char *kl_vulkan_xr_device_extensions(void)   { return ""; }

// The physical device the app must render with.
//
// The VkInstance is the APP's — it created one before asking, which is the
// ordering XR_KHR_vulkan_enable prescribes — so this enumerates on the handle it
// passes rather than on g_instance. Those are usually the same object here (the
// app's vkCreateInstance came through klvk_CreateInstance), but "usually" is not
// a thing to encode: the spec names the instance as a parameter precisely
// because the runtime is not entitled to assume it owns one.
//
// The FIRST device is the answer because MoltenVK exposes exactly one per Metal
// device, and picking by index rather than by score is honest about that. A
// machine that ever reports two would want a rule here, and would say so by
// this log line naming a count above 1.
void *kl_vulkan_xr_physical_device(void *vk_instance) {
    if (!kl_vulkan_available() || !vk_instance) return NULL;
    PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)
        real_gipa((VkInstance)vk_instance, "vkEnumeratePhysicalDevices");
    if (!enum_pd) { VKI("xr: no vkEnumeratePhysicalDevices on the app's instance\n"); return NULL; }
    uint32_t n = 0;
    if (enum_pd((VkInstance)vk_instance, &n, NULL) != VK_SUCCESS || n == 0) {
        // The trap BONELAB paid for, in the one place it can recur: without
        // VK_KHR_portability_enumeration this answers zero devices AND
        // VK_SUCCESS, so a bare count of 0 is not necessarily an empty machine.
        VKI("xr: vkEnumeratePhysicalDevices reports 0 devices — if the app's "
            "instance omitted VK_KHR_portability_enumeration, that is why\n");
        return NULL;
    }
    VkPhysicalDevice pd[8];
    if (n > 8) n = 8;
    if (enum_pd((VkInstance)vk_instance, &n, pd) != VK_SUCCESS || n == 0) return NULL;
    if (n > 1) VKI("xr: %u physical devices; taking the first\n", n);
    return (void *)pd[0];
}

// One OpenXR swapchain image.
//
// Deliberately NOT keyed on (stage, eye) the way the OVRPlugin eye images are:
// in OpenXR the runtime allocates a swapchain's images at xrCreateSwapchain,
// which knows a size and a format and nothing else — WHICH swapchain is an eye
// is only asserted later, at xrEndFrame, from the projection layer. That is a
// property of the API, not of a guest.
//
// So this hands back a bare image and keeps no table of its own; the eye
// association, and with it the MTLTexture publication, belongs at the assertion.
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned layers,
                                      unsigned mips, long long vk_format, int depth) {
    if (!kl_vulkan_guest_active()) return 0;
    klvk_device *d = xr_dev();
    if (!d || !w || !h) return 0;
    // A depth image is not a colour image with a different format: it needs the
    // depth/stencil attachment usage, and asking for COLOR_ATTACHMENT on a depth
    // format is a validation error rather than a harmless extra bit.
    VkImageUsageFlags usage = depth
        ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
        : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage img = klvk_image_alloc(d, (VkFormat)vk_format, w, h,
                                   layers ? layers : 1, mips ? mips : 1, usage, &mem,
                                   KLVK_LEDGER_XR, NULL);
    if (img == VK_NULL_HANDLE) return 0;
    if (!depth && kl_fove_on()) fove_xr_image(img, layers ? layers : 1, w, h);   // round G3
    return (uint64_t)(uintptr_t)img;
}

// The MTLTexture behind an image this file created, for the compositor seam.
// NULL when the export is unavailable — the caller reports it, because only the
// caller knows which eye it was about.
void *kl_vulkan_xr_image_mtl(unsigned long long image) {
    if (!kl_vulkan_guest_active() || !image) return NULL;
    return eye_mtl_texture(xr_dev(), (VkImage)(uintptr_t)image);
}

// The Vulkan version range a session may be created against.
//
// Measured off the physical device, not asserted: MoltenVK's instance-level and
// device-level versions differ, and the one that governs the app's device is the
// device's. The floor is 1.0 because nothing here needs more, and overstating a
// floor is the dangerous direction — the same reasoning, and the same trap, as
// the GLES range next door (an app told it needs 1.2 asks for 1.2).
void kl_vulkan_xr_api_range(unsigned *min_major, unsigned *min_minor,
                            unsigned *max_major, unsigned *max_minor) {
    if (min_major) *min_major = 1;
    if (min_minor) *min_minor = 0;
    // A conservative ceiling until a device answers, so a failure to measure
    // cannot present as "this runtime supports nothing".
    if (max_major) *max_major = 1;
    if (max_minor) *max_minor = 0;
    if (!kl_vulkan_available()) return;

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (g_ndev > 0 && g_devs[0]) phys = g_devs[0]->phys;
    if (phys == VK_NULL_HANDLE && g_instance) {
        PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)
            real_gipa(g_instance, "vkEnumeratePhysicalDevices");
        uint32_t n = 1;
        VkPhysicalDevice one = VK_NULL_HANDLE;
        if (enum_pd && enum_pd(g_instance, &n, &one) >= 0 && n) phys = one;
    }
    if (phys == VK_NULL_HANDLE) return;

    PFN_vkGetPhysicalDeviceProperties props_fn = (PFN_vkGetPhysicalDeviceProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    if (!props_fn) return;
    VkPhysicalDeviceProperties p;
    memset(&p, 0, sizeof p);
    props_fn(phys, &p);
    if (max_major) *max_major = VK_VERSION_MAJOR(p.apiVersion);
    if (max_minor) *max_minor = VK_VERSION_MINOR(p.apiVersion);
}

void kl_vulkan_capture_eyes(unsigned frame, int stage) {
    const char *dir = cap_dir();
    if (!dir || !kl_vulkan_guest_active()) return;
    int every = kl_env_int("KL_VK_OUT_EVERY", 1);
    if (every < 1) every = 1;
    if (frame % (unsigned)every) return;
    // The capture WINDOW. Without it the only way to see consecutive frames is
    // KL_VK_OUT_EVERY=1 from frame 0, and a full eye pair is ~21 MB — a run
    // long enough to reach a loaded level is then hundreds of gigabytes, which
    // is why every capture this port has taken so far has been a sparse sample
    // and no two of them were adjacent frames. A per-frame question ("is this
    // object in frame N and gone in N+1?") needs adjacent frames and does not
    // need many, so the window is the instrument for it.
    int from = kl_env_int("KL_VK_OUT_FROM", 0);
    int to   = kl_env_int("KL_VK_OUT_TO", 0);
    if (from > 0 && frame < (unsigned)from) return;
    if (to   > 0 && frame > (unsigned)to)   return;
    if (stage < 0 || stage >= KLVK_EYE_STAGES) return;

    klvk_device *d = g_devs[0];
    // Under the Array layout slot 0 holds the only image and the two eyes are
    // its layers, so the eye index selects a layer there and an image here. The
    // filename stays `eyeN` either way — it names what was drawn, not where it
    // was stored.
    int layered = g_eye[stage][0].img && g_eye[stage][0].layers > 1;
    for (int eye = 0; eye < 2; eye++) {
        int slot = layered ? 0 : eye;
        if (!g_eye[stage][slot].img) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/vk_f%05u_s%d_eye%d.png", dir, frame, stage, eye);
        // The guest submitted this to a compositor, so it left it readable by
        // one — SHADER_READ_ONLY_OPTIMAL. See cap_image on why a wrong guess
        // here is cheap on MoltenVK specifically.
        if (cap_image_layer(d, d->queue, g_eye[stage][slot].img, g_eye[stage][slot].w,
                            g_eye[stage][slot].h, g_eye[stage][slot].fmt,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            layered ? (uint32_t)eye : 0u, path, NULL, 0))
            g_presented++;
    }
}

// ---------------------------------------------------------------------------
// The frame seam — "this eye texture is finished, a compositor may sample it"
// ---------------------------------------------------------------------------
//
// The GL path answers this with an MTLSharedEvent queued into ANGLE's own
// command stream (kl_glfb's klfb_gpu_frame_now): the compositor's command
// buffer waits on a value the guest's queue signals, and nothing ever stalls on
// the CPU. There is no equivalent here, and the reason is not an oversight —
// **the guest owns the VkQueue and we do not add work to its submissions.** A
// cross-queue GPU wait needs a shared event signalled *after* the guest's
// rendering, and the only ways to get one are to signal a timeline semaphore on
// the guest's queue (which needs `VK_KHR_timeline_semaphore` enabled on a device
// the GUEST creates) or to export the MTLCommandQueue and encode the signal
// ourselves in Objective-C, which this plain-C file cannot do.
//
// So this waits. `vkQueueWaitIdle` on the guest's own queue, at the moment the
// guest asserts the eye textures are drawn (ovrp_EndFrame4), and then a serial
// the compositor reads: past it, the picture is not merely submitted but
// COMPLETE, so no compositor-side ordering is needed at all.
//
// It is a real stall on the guest's frame thread and it is named as one. The
// capture path (KL_VK_OUT) already pays exactly this cost on every captured
// frame, and no device run has happened, so it buys correctness at a price
// nothing has yet measured. `KL_VK_FRAME_SYNC=0` removes the wait and keeps the
// serial — the A/B for "is the compositor showing a torn frame?", and the shape
// the timeline-semaphore version would have. `g_frame_serial` itself is
// declared up beside g_presented — the graveyard needs it earlier.

unsigned long long kl_vulkan_frame_serial(void) {
    return __atomic_load_n(&g_frame_serial, __ATOMIC_ACQUIRE);
}

// What the GPU is holding, for the tripwire dump and for the once-a-second line
// the heartbeat prints. Two independent numbers, and the gap between them is
// the point:
//
//   OURS      — every image this file allocated, by kind, from the sizes
//               vkGetImageMemoryRequirements gave. Exact.
//   MTLDevice — currentAllocatedSize, the whole process's Metal footprint:
//               ours plus MoltenVK's plus every render target, texture, buffer
//               and heap the GUEST asked for through the synthetic loader.
//
// The second minus the first is the guest's own GPU appetite, which is the
// number that had never been measured when a 6153 MiB footprint showed only
// 1.5 GiB of mapped regions and 43 MiB of IOAccelerator. Neither is visible to
// a vm_region walk, so neither was visible at all.
// The top of the image ledger by BYTES. Eight rows, because the question is
// always "which handful is this" and a full list of four hundred is a file
// nobody reads. Printed only when the live image bytes have MOVED since the
// last report, for the same reason the format census is: once a second forever
// is how a useful instrument becomes one people filter out.
static uint64_t g_gimg_last_printed;

static void klvk_top_images(FILE *f) {
    klvk_gimg top[8];
    uint64_t total, tbytes, lazybytes = 0;
    uint32_t live, over, tn, lazyn = 0;
    pthread_mutex_lock(&g_gimg_lock);
    total = g_gimg_bytes; live = g_gimg_live; over = g_gimg_overflow;
    tbytes = g_gimg_transient_bytes; tn = g_gimg_transient_n;
    memset(top, 0, sizeof top);
    for (unsigned i = 0; i < KLVK_GIMG_N; i++) {
        if (!g_gimg[i].img || !g_gimg[i].bytes) continue;
        // Bound to a LAZILY_ALLOCATED type: the size below is what the image
        // WOULD have charged. Metal gives a memoryless texture tile storage for
        // the length of one render pass and no device memory at all, so these
        // bytes are counted here and are NOT in the footprint.
        if (g_gimg[i].memtype >= 0 && (g_lazy_mask >> g_gimg[i].memtype) & 1u) {
            lazybytes += g_gimg[i].bytes;
            lazyn++;
        }
        for (unsigned k = 0; k < 8; k++)
            if (g_gimg[i].bytes > top[k].bytes) {
                for (unsigned m = 7; m > k; m--) top[m] = top[m - 1];
                top[k] = g_gimg[i];
                break;
            }
    }
    pthread_mutex_unlock(&g_gimg_lock);
    if (!total) return;
    // A megabyte of churn is the noise floor of a streaming texture pool; a
    // change worth a report is bigger than that.
    uint64_t moved = total > g_gimg_last_printed ? total - g_gimg_last_printed
                                                 : g_gimg_last_printed - total;
    if (moved < 16ull * 1024 * 1024) return;
    g_gimg_last_printed = total;

    char lazy[128];
    lazy[0] = 0;
    if (lazyn)
        snprintf(lazy, sizeof lazy,
                 " | MEMORYLESS %u (%.0f MiB charged to nothing)",
                 lazyn, (double)lazybytes / (1024 * 1024));
    fprintf(f, "  [vk] guest images: %u live, %.0f MiB by vkGetImageMemoryRequirements"
               " | transient %u (%.0f MiB)%s%s%s\n",
            live, (double)total / (1024 * 1024), tn,
            (double)tbytes / (1024 * 1024), lazy,
            g_lazy_mask ? "" : ", and there is NO memoryless memory type here",
            over ? " | TABLE OVERFLOWED" : "");
    for (unsigned k = 0; k < 8 && top[k].bytes; k++) {
        char u[128];
        fprintf(f, "  [vk]   %7.0f MiB  %s %ux%u",
                (double)top[k].bytes / (1024 * 1024),
                kl_vulkan_format_name((int)top[k].fmt), top[k].w, top[k].h);
        if (top[k].d > 1) fprintf(f, "x%u", top[k].d);
        if (top[k].layers > 1) fprintf(f, " x%u layer", top[k].layers);
        if (top[k].mips > 1) fprintf(f, ", %u mip", top[k].mips);
        if (top[k].samples > 1) fprintf(f, ", %ux MSAA", top[k].samples);
        if (top[k].orig != top[k].fmt)
            fprintf(f, " (asked %s)", kl_vulkan_format_name((int)top[k].orig));
        fprintf(f, ", %s, type %d\n",
                usage_words((VkImageUsageFlags)top[k].usage, u, sizeof u),
                (int)top[k].memtype);
    }
}

static void idx_guard_report(FILE *f);
static void query_census_report(FILE *f);

void kl_vulkan_memory_report(FILE *f) {
    if (!f) return;
    if (!kl_vulkan_available()) return;
    idx_guard_report(f);
    query_census_report(f);

    // Round 18: the depth and draw censuses used to print on EVERY call of
    // this function, which the device heartbeat makes once a second. They ride
    // a 15 s clock now, with the substitution line beside them — see
    // klvk_census_riders().
    klvk_census_riders(f);

    uint64_t ours = 0;
    for (int i = 0; i < KLVK_LEDGER_N; i++) ours += g_ledger_bytes[i];
    // Silent until there is something to say. MoltenVK being reachable is not
    // the same as a guest having a device, and a line reading "ours 0 MiB"
    // once a second from process start is how a useful instrument becomes one
    // people filter out.
    if (!ours && !g_devs[0]) return;
    fprintf(f, "  [vk] memory: ours %.0f MiB", (double)ours / (1024 * 1024));
    for (int i = 0; i < KLVK_LEDGER_N; i++)
        if (g_ledger_bytes[i] || g_ledger_n[i])
            fprintf(f, " | %s %.0f (%u img)", k_ledger_name[i],
                    (double)g_ledger_bytes[i] / (1024 * 1024), g_ledger_n[i]);
    if (g_ledger_retired || g_ledger_freed)
        fprintf(f, " | retired %.0f, released %.0f",
                (double)g_ledger_retired / (1024 * 1024),
                (double)g_ledger_freed / (1024 * 1024));
    unsigned long long mtl = klvk_mtl_allocated();
    if (mtl)
        fprintf(f, " | MTLDevice %.0f MiB (guest+MoltenVK share %.0f)",
                (double)mtl / (1024 * 1024),
                (double)(mtl > ours ? mtl - ours : 0) / (1024 * 1024));
    fprintf(f, "\n");

    // ...and the guest's half, which round 10 could only ever report as one
    // subtracted number. Per memory TYPE, because that is the question: if
    // Unity's live bytes sit almost entirely in a type carrying
    // HOST_VISIBLE|HOST_COHERENT, it is keeping persistent staging copies of
    // things that should be device-private, and the type index is what names
    // it. The property flags are printed beside the index so the line can be
    // read without a second lookup.
    // The image census, printed only when the set of distinct formats has
    // GROWN since the last time. Once a second forever would be noise; once
    // per new format is the whole of what round 11's log could not say.
    if (g_img_fmt_seen != g_img_fmt_printed) {
        g_img_fmt_printed = g_img_fmt_seen;
        fprintf(f, "  [vk] guest images by format:");
        for (unsigned i = 0; i < KLVK_FMTC_N; i++)
            if (g_img_by_fmt[i])
                fprintf(f, " %s x%u", kl_vulkan_format_name((int)i), g_img_by_fmt[i]);
        if (g_subst_n || g_subst_overflow)
            fprintf(f, " | %u substituted live%s", g_subst_n,
                    g_subst_overflow ? ", TABLE OVERFLOWED" : "");
        fprintf(f, "\n");
    }

    // The top of the by-BYTES ledger, which is the line launch #13 needed and
    // did not have. Its final census said "3904 MiB, 390 alloc" and its peak
    // (3568 MiB) had been reached in SIXTY-SIX of them — so ~1757 MiB of it was
    // three allocations, and nothing in the log said which three. These rows
    // say it directly: format, extent, layers, usage, memory type, bytes.
    klvk_top_images(f);

    // ...and the pipeline line, for the OTHER report Austin filed: "it stutters
    // when I first point at the menu". A synchronous MSL compile inside
    // vkCreateGraphicsPipelines is the shape that produces exactly that, and
    // this says whether it is happening and how badly.
    if (g_pipe_n) {
        fprintf(f, "  [vk] pipelines: %u created in %u call(s), %.0f ms total, "
                   "worst %.1f ms, %u over one 120 Hz frame",
                g_pipe_n, g_pipe_calls, (double)g_pipe_ns / 1e6,
                (double)g_pipe_worst_ns / 1e6, g_pipe_slow);
        if (g_pipe_cache_seed_bytes || g_pipe_cache_save_bytes)
            fprintf(f, " | cache seeded %llu KiB, saved %llu KiB",
                    (unsigned long long)(g_pipe_cache_seed_bytes / 1024),
                    (unsigned long long)(g_pipe_cache_save_bytes / 1024));
        else if (pipe_cache_path())
            fprintf(f, " | cache file empty so far");
        else
            fprintf(f, " | NOT persisted (KL_VK_PIPELINE_CACHE unset) — every "
                       "launch recompiles all of these");
        fprintf(f, "\n");
    }

    klvk_pipe_cache_save();

    pthread_mutex_lock(&g_guest_alloc_lock);
    uint64_t live = g_guest_mem_live, peak = g_guest_mem_peak;
    uint64_t mapped = g_guest_mapped;
    uint32_t mapped_n = g_guest_mapped_n, over = g_guest_alloc_overflow;
    uint64_t by[KLVK_MEMTYPES]; uint32_t bn[KLVK_MEMTYPES];
    memcpy(by, g_guest_mem_bytes, sizeof by);
    memcpy(bn, g_guest_mem_n, sizeof bn);
    pthread_mutex_unlock(&g_guest_alloc_lock);
    if (!live && !peak) return;

    fprintf(f, "  [vk] guest memory: %.0f MiB live (peak %.0f)", 
            (double)live / (1024 * 1024), (double)peak / (1024 * 1024));
    // MAPPED is the double-count, and it is the number that makes the tripwire
    // dump add up. A mapped shared-storage allocation is charged to
    // phys_footprint once, but appears TWICE in the dump — as anon dirty in the
    // region walk and inside MTLDevice's total. Everything else is private and
    // invisible to the walk. So: anon + Metal - mapped ~= footprint, and any
    // residue is genuinely somewhere neither instrument is looking.
    fprintf(f, " | mapped %.0f MiB in %u (this is the anon<->Metal double count)",
            (double)mapped / (1024 * 1024), mapped_n);
    if (over) fprintf(f, " | %u allocation(s) past the table — totals high", over);
    fprintf(f, "\n");
    for (uint32_t t = 0; t < KLVK_MEMTYPES; t++) {
        if (!by[t] && !bn[t]) continue;
        VkMemoryPropertyFlags fl = (g_mem_props_read && t < g_mem_props.memoryTypeCount)
                                 ? g_mem_props.memoryTypes[t].propertyFlags : 0;
        fprintf(f, "  [vk]   type %2u  %8.0f MiB  %5u alloc  %s%s%s%s%s\n", t,
                (double)by[t] / (1024 * 1024), bn[t],
                (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)     ? "DEVICE_LOCAL "  : "",
                (fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)     ? "HOST_VISIBLE "  : "",
                (fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)    ? "HOST_COHERENT " : "",
                (fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)      ? "HOST_CACHED "   : "",
                (fl & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "LAZY "          : "");
    }
}

static uint64_t klvk_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// The frame seam's own census. Reported beside the occlusion-query one because
// they answer the same question from two ends: does anything in this runtime
// make an object come and go that the guest meant to draw.
void kl_vulkan_frame_sync_stats(unsigned long long *n, unsigned long long *total_ns,
                                unsigned long long *worst_ns,
                                unsigned long long *over_1ms) {
    if (n)        *n        = g_fsync_n;
    if (total_ns) *total_ns = g_fsync_ns;
    if (worst_ns) *worst_ns = g_fsync_worst;
    if (over_1ms) *over_1ms = g_fsync_over1ms;
}

static int g_frame_sync_external;   // see kl_vulkan_frame_sync_external
void kl_vulkan_frame_sync_external(int on) {
    int was = __atomic_exchange_n(&g_frame_sync_external, on ? 1 : 0, __ATOMIC_RELAXED);
    if (was != (on ? 1 : 0))
        VKI("frame end: the queue drain is %s (%s)\n", on ? "OFF" : "ON",
            on ? "a frontend fences its own eye pictures" : "kl_vulkan_frame_done waits");
}

void kl_vulkan_frame_done(int stage) {
    (void)stage;
    // The draw census's per-frame bucket closes here, at the ovrp frame seam,
    // and BEFORE the guest-active gate — a frame that drew nothing is a real
    // observation and dropping it would flatter the mean.
    klvk_draw_frame_close();
    if (!kl_vulkan_guest_active()) return;
    klvk_device *d = g_devs[0];
    // Retired storage is released HERE and nowhere else, because this is the
    // one point in the frame where the guest's queue has been made idle. See
    // klvk_retire_slot.
    klvk_grave_sweep(d, __atomic_load_n(&g_frame_serial, __ATOMIC_ACQUIRE));
    if (kl_env_on("KL_VK_FRAME_SYNC", 1) && d && d->queue &&
        !__atomic_load_n(&g_frame_sync_external, __ATOMIC_RELAXED)) {
        // How long the guest's GPU work is still outstanding when it declares
        // the frame finished. This is not a performance number, it is the
        // WIDTH OF THE WINDOW in which a compositor can sample an eye texture
        // the GPU is still writing — see klovrp_EndFrame4 for what sits inside
        // it. A wait of ~0 means the guest's queue was already drained and the
        // ordering hardly matters; a wait of milliseconds means a compositor
        // running at the panel's rate lands inside it several times a second.
        uint64_t t0 = klvk_mono_ns();
        VkResult r = d->QueueWaitIdle ? d->QueueWaitIdle(d->queue)
                   : d->DeviceWaitIdle ? d->DeviceWaitIdle(d->dev)
                                       : VK_SUCCESS;
        uint64_t dt = klvk_mono_ns() - t0;
        g_fsync_n++;
        g_fsync_ns += dt;
        if (dt > g_fsync_worst) g_fsync_worst = dt;
        if (dt > 1000000u) g_fsync_over1ms++;
        if (r != VK_SUCCESS) {
            static int said;
            if (!said++)
                VKI("waiting for the guest's queue at frame end answered %d — a "
                    "compositor may sample an eye the GPU is still writing\n", (int)r);
        }
    }
    __atomic_add_fetch(&g_frame_serial, 1, __ATOMIC_RELEASE);
    query_census_tick();
}

static VkResult klvk_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    klvk_device *d = dev_of_queue(q);
    unsigned this_frame = g_presented++;

    int every = kl_env_int("KL_VK_OUT_EVERY", 1);
    if (every < 1) every = 1;
    const char *dir = cap_dir();
    int want = dir && (this_frame % (unsigned)every) == 0;

    for (uint32_t i = 0; pi && i < pi->swapchainCount; i++) {
        klvk_swapchain *sc = swap_of(pi->pSwapchains[i]);
        if (!sc) continue;
        if (want && i == 0) {
            cap_frame(sc, q, pi->pImageIndices[i], pi->pWaitSemaphores,
                      pi->waitSemaphoreCount);
        } else if (pi->waitSemaphoreCount && d) {
            // Not capturing, but the wait semaphores still have to be consumed.
            VkPipelineStageFlags *stages =
                calloc(pi->waitSemaphoreCount, sizeof *stages);
            for (uint32_t k = 0; k < pi->waitSemaphoreCount; k++)
                stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = pi->waitSemaphoreCount,
                .pWaitSemaphores = pi->pWaitSemaphores,
        .pWaitDstStageMask = stages,
            };
            d->QueueSubmit(q, 1, &si, VK_NULL_HANDLE);
            free(stages);
        }
        if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
    }
    if (this_frame == 0) VKI("first vkQueuePresentKHR — the guest is rendering\n");
    return VK_SUCCESS;
}


// ---------------------------------------------------------------------------
// Dynamic-rendering attachment census (vkCmdBeginRendering)
// ---------------------------------------------------------------------------
// MoltenVK encodes a dynamic render pass at SUBMIT time from the MVKImageView
// pointers recorded at vkCmdBeginRendering. A view that was destroyed, whose
// image was destroyed, or that was never a view at all, is found only there —
// as a NULL/stale MVKImage deref in MVKRenderSubpass::populateMTLRender-
// PassDescriptor / MVKImageView::populateMTLRenderPassAttachmentDescriptor
// (the Alyx host hit it 1 run in ~3), with nothing naming the attachment. So
// every view is recorded (view -> image) and every attachment of every
// vkCmdBeginRendering is checked against the table when it is RECORDED:
//   - a resolve asked for with no resolve view (or no view to resolve) is
//     taken off (resolveMode NONE) — a no-op in Vulkan, a NULL view in MoltenVK;
//   - a view the table never saw, one already destroyed, or one whose image is
//     gone is named once each, and dropped from the pass (imageView NULL, the
//     attachment unused) rather than handed to MoltenVK to fault on.
// KL_VK_RENDERING_CENSUS=0 passes every call through untouched.
#define KLVK_VIEW_N 16384
typedef struct {
    VkImageView view; VkImage img; uint8_t dead_view, dead_img;
    uint16_t base_mip, base_layer, layers;   // the view's range (layers 0 = to the end)
} klvk_view;
static klvk_view g_views[KLVK_VIEW_N];
static pthread_mutex_t g_views_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_views_overflow, g_rc_calls, g_rc_fixed, g_rc_dropped;
static uint32_t g_rc_ss_resolve[3];      // single-sample resolves taken off: colour, depth, stencil
static uint32_t g_rc_ss_copy[3];         // ...of which emulated as a copy at vkCmdEndRendering
static uint32_t g_rc_ss_nocopy;          // ...asked to copy and could not (dropped instead)
static uint32_t g_rc_res_any[3];         // every resolve asked for, by aspect
static uint32_t g_rc_res_unknown;        // ...whose source the image ledger does not hold
// Where the copies land: the resolve TARGETS seen, and how many later passes
// bind one as their depth/stencil attachment (a depth-bounds or depth-tested
// pass over the copy — on the headset such a pass reads it for real).
static VkImage g_rc_ss_dst[8];
static uint32_t g_rc_ss_dst_n, g_rc_dst_as_depth;
static void rc_note_dst(VkImage img) {
    if (!img) return;
    for (uint32_t i = 0; i < g_rc_ss_dst_n; i++) if (g_rc_ss_dst[i] == img) return;
    uint32_t k = g_rc_ss_dst_n;
    if (k < 8) { g_rc_ss_dst[k] = img; g_rc_ss_dst_n = k + 1; }
    else g_rc_ss_dst[7] = img;
}
static int rc_is_dst(VkImage img) {
    for (uint32_t i = 0; img && i < g_rc_ss_dst_n; i++) if (g_rc_ss_dst[i] == img) return 1;
    return 0;
}

static int ss_resolve_copy(void);

// The periodic census line for vkCmdBeginRendering: passes seen, passes this
// layer rewrote, and how many single-sample resolves it took off per aspect
// (the per-call lines stop after 8; these counts do not).
static void klvk_rc_report(FILE *f) {
    if (!g_rc_calls) return;
    fprintf(f, "  [vk] dynamic rendering: %u pass(es), %u rewritten, %u dead attachment(s) "
               "dropped; single-sample resolves taken off: colour %u, depth %u, stencil %u "
               "(emulated as a copy: colour %u, depth %u, stencil %u; %u could not be, "
               "KL_VK_SS_RESOLVE=%s); %u later pass(es) bound a resolve target as "
               "their depth/stencil attachment; resolves asked for in all: colour %u, "
               "depth %u, stencil %u, %u from an image the ledger does not hold "
               "(ledger overflow %u)\n",
            g_rc_calls, g_rc_fixed, g_rc_dropped, g_rc_ss_resolve[0], g_rc_ss_resolve[1],
            g_rc_ss_resolve[2], g_rc_ss_copy[0], g_rc_ss_copy[1], g_rc_ss_copy[2],
            g_rc_ss_nocopy, ss_resolve_copy() ? "copy" : "drop", g_rc_dst_as_depth,
            g_rc_res_any[0], g_rc_res_any[1], g_rc_res_any[2], g_rc_res_unknown,
            g_gimg_overflow);
}

static int rc_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_RENDERING_CENSUS", 1);
    return on;
}

static unsigned view_slot(VkImageView v) {
    uint64_t h = (uint64_t)(uintptr_t)v;
    h ^= h >> 17; h *= 0x9e3779b97f4a7c15ull; h ^= h >> 29;
    return (unsigned)(h % KLVK_VIEW_N);
}

// Caller holds g_views_lock. Returns the row for v, or NULL.
static klvk_view *view_find_locked(VkImageView v) {
    unsigned i = view_slot(v);
    for (unsigned n = 0; n < KLVK_VIEW_N; n++, i = (i + 1) % KLVK_VIEW_N) {
        if (g_views[i].view == v) return &g_views[i];
        if (!g_views[i].view) return NULL;
    }
    return NULL;
}

static void view_add(VkImageView v, VkImage img, const VkImageSubresourceRange *range) {
    if (!v || !rc_on()) return;
    pthread_mutex_lock(&g_views_lock);
    klvk_view *r = view_find_locked(v);
    if (!r) {
        unsigned i = view_slot(v), n = 0;
        // A handle MoltenVK reused after a destroy lands on its old (dead) row
        // above; a fresh one takes the first empty slot of its probe chain.
        for (; n < KLVK_VIEW_N && g_views[i].view; n++, i = (i + 1) % KLVK_VIEW_N) {}
        if (n == KLVK_VIEW_N) { g_views_overflow++; pthread_mutex_unlock(&g_views_lock); return; }
        r = &g_views[i];
    }
    r->view = v; r->img = img; r->dead_view = 0; r->dead_img = 0;
    r->base_mip = range ? (uint16_t)range->baseMipLevel : 0;
    r->base_layer = range ? (uint16_t)range->baseArrayLayer : 0;
    r->layers = range && range->layerCount != VK_REMAINING_ARRAY_LAYERS
              ? (uint16_t)range->layerCount : 0;
    pthread_mutex_unlock(&g_views_lock);
}

static void views_image_gone(VkImage img) {
    if (!img || !rc_on()) return;
    pthread_mutex_lock(&g_views_lock);
    unsigned live = 0;
    for (unsigned i = 0; i < KLVK_VIEW_N; i++)
        if (g_views[i].view && g_views[i].img == img) {
            if (!g_views[i].dead_view && !g_views[i].dead_img) {
                live++;
                hold_killed(g_views[i].view, img, 0);
            }
            g_views[i].dead_img = 1;
        }
    static unsigned said;
    if (live && said++ < 16)
        VKI("vkDestroyImage %p with %u view(s) of it still alive (legal until one "
            "is used; every intermittent MVKImage NULL fault so far followed one)\n",
            (void *)img, live);
    pthread_mutex_unlock(&g_views_lock);
}

// 0 fine, 1 unknown, 2 view destroyed, 3 image destroyed. NULL is fine here.
static int view_state(VkImageView v, VkImage *img) {
    if (!v) return 0;
    pthread_mutex_lock(&g_views_lock);
    klvk_view *r = view_find_locked(v);
    int st = !r ? 1 : r->dead_view ? 2 : r->dead_img ? 3 : 0;
    if (img) *img = r ? r->img : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_views_lock);
    return st;
}

static void klvk_DestroyImageView(VkDevice dev, VkImageView v,
                                  const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyImageView real =
        (PFN_vkDestroyImageView)(real_gdpa ? real_gdpa(dev, "vkDestroyImageView") : NULL);
    if (v && rc_on()) {
        pthread_mutex_lock(&g_views_lock);
        klvk_view *r = view_find_locked(v);
        int live = r && !r->dead_view && !r->dead_img;
        VkImage img = r ? r->img : VK_NULL_HANDLE;
        if (r) r->dead_view = 1;
        pthread_mutex_unlock(&g_views_lock);
        if (live) hold_killed(v, img, 1);
    }
    if (real) real(dev, v, alloc);
}

static const char *const k_view_state[] = { "ok", "UNKNOWN (never created here)",
                                            "DESTROYED", "its IMAGE DESTROYED" };

// ---- single-sample resolves, emulated as a copy (round N12-B) -------------
// A resolve FROM a single-sample attachment is, in every resolve mode, a copy:
// the one sample is the average, the min, the max and sample zero. Source 2
// relies on it — it renders the eye's depth/stencil into one image (stored
// DONT_CARE) and "resolves" it into a second every frame, and the level's
// rendering depends on the second: taken off, it renders with black blobs over
// most of the world (headset launch 4, "VERY dark", 7716 depth+stencil
// resolves taken off in 7715 frames; reproduced on the Mac, round N12-B, by
// KL_VK_SS_RESOLVE=drop: mean luma 88 -> 68, 20% of the eye black). The Mac
// had never shown it only because the image ledger had overflowed before the
// level made its eye depth, so the resolve went to Metal untouched (which
// macOS tolerates and the headset does not). Metal
// cannot take the resolve itself (a resolveTexture on a 1-sample texture hangs
// the GPU, round N6), so the resolve is taken off the pass as before, the
// source attachment is made to STORE, and at the pass's vkCmdEndRendering the
// same pixels are copied into the resolve image with vkCmdCopyImage, bracketed
// by barriers that leave both images in the layouts the guest named.
// KL_VK_SS_RESOLVE=drop restores the round-N6 behaviour (taken off, nothing
// copied) for A/B.
static int ss_resolve_copy(void) {
    static int on = -1;
    if (on < 0) {
        const char *m = kl_env_str("KL_VK_SS_RESOLVE", "copy");
        on = !(m && !strcmp(m, "drop"));
    }
    return on;
}

typedef struct {
    VkImage src, dst;
    VkImageLayout src_layout, dst_layout;
    VkImageAspectFlags aspect, whole;   // aspects to copy; all aspects of the format
    uint32_t mip, dst_mip, src_layer, dst_layer, layers;
    VkOffset2D off; VkExtent2D ext;
} klvk_sscopy;
#define KLVK_SSC_CB 64
#define KLVK_SSC_PER 4
typedef struct { VkCommandBuffer cb; uint32_t n; klvk_sscopy c[KLVK_SSC_PER]; } klvk_ssc_cb;
static klvk_ssc_cb g_ssc[KLVK_SSC_CB];
static pthread_mutex_t g_ssc_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t g_ssc_pending;            // rows with n > 0

static VkImageAspectFlags fmt_aspects(uint32_t f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// Queue the copy for this attachment on cb; 0 if it cannot be done (the caller
// then only takes the resolve off, as before). Merges the depth and stencil
// halves of one image pair into one copy.
static int ssc_queue(VkCommandBuffer cb, const VkRenderingInfo *ri,
                     const VkRenderingAttachmentInfo *a, VkImage src, int what) {
    static unsigned said;
    const char *why = NULL;
    VkImage dst = VK_NULL_HANDLE;
    klvk_view sv = {0}, dv = {0};
    uint32_t sf = 0, sw = 0, sh = 0, sl = 0, ss = 0, df = 0, dw = 0, dh = 0, dl = 0, ds = 0;
    pthread_mutex_lock(&g_views_lock);
    klvk_view *r = view_find_locked(a->imageView);
    if (r) sv = *r;
    r = view_find_locked(a->resolveImageView);
    if (r) dv = *r;
    pthread_mutex_unlock(&g_views_lock);
    dst = dv.img;
    if (!sv.view || !dv.view || sv.img != src) why = "a view the table does not know";
    else if (ri->flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT))
        why = "a suspended/resumed pass";
    else if (!gimg_info(src, &sf, &sw, &sh, &sl, &ss) || !gimg_info(dst, &df, &dw, &dh, &dl, &ds))
        why = "an image the ledger does not know";
    else if (sf != df) why = "the two images differ in format";
    else if (ds != 1) why = "a multisampled resolve target";
    if (why) {
        g_rc_ss_nocopy++;
        if (said++ < 8)
            VKI("single-sample %s resolve %p -> %p NOT emulated (%s) — taken off only\n",
                what == 0 ? "colour" : what == 1 ? "depth" : "stencil",
                (void *)src, (void *)dst, why);
        return 0;
    }
    uint32_t mip = sv.base_mip;
    uint32_t mw = sw >> mip ? sw >> mip : 1, mh = sh >> mip ? sh >> mip : 1;
    uint32_t dmw = dw >> dv.base_mip ? dw >> dv.base_mip : 1;
    uint32_t dmh = dh >> dv.base_mip ? dh >> dv.base_mip : 1;
    if (dmw < mw) mw = dmw;                // the region has to fit both mips
    if (dmh < mh) mh = dmh;
    int32_t x0 = ri->renderArea.offset.x > 0 ? ri->renderArea.offset.x : 0;
    int32_t y0 = ri->renderArea.offset.y > 0 ? ri->renderArea.offset.y : 0;
    uint32_t ew = ri->renderArea.extent.width, eh = ri->renderArea.extent.height;
    if ((uint32_t)x0 >= mw || (uint32_t)y0 >= mh || !ew || !eh) {
        g_rc_ss_nocopy++;
        return 0;
    }
    if (x0 + ew > mw) ew = mw - (uint32_t)x0;
    if (y0 + eh > mh) eh = mh - (uint32_t)y0;
    // Layers: the view's own count, at least as many as the view mask names
    // (multiview renders layer i of the view for bit i), at least the pass's.
    uint32_t sv_l = sv.layers ? sv.layers : (sl > sv.base_layer ? sl - sv.base_layer : 1);
    uint32_t dv_l = dv.layers ? dv.layers : (dl > dv.base_layer ? dl - dv.base_layer : 1);
    uint32_t need = ri->layerCount ? ri->layerCount : 1;
    if (ri->viewMask) { need = 0; for (uint32_t m = ri->viewMask; m; m >>= 1) need++; }
    uint32_t layers = need;
    if (layers > sv_l) layers = sv_l;
    if (layers > dv_l) layers = dv_l;
    VkImageAspectFlags asp = what == 0 ? VK_IMAGE_ASPECT_COLOR_BIT
                           : what == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;

    pthread_mutex_lock(&g_ssc_lock);
    klvk_ssc_cb *row = NULL, *empty = NULL;
    for (unsigned i = 0; i < KLVK_SSC_CB; i++) {
        if (g_ssc[i].cb == cb) { row = &g_ssc[i]; break; }
        if (!empty && (!g_ssc[i].cb || !g_ssc[i].n)) empty = &g_ssc[i];
    }
    if (!row && empty) { row = empty; row->cb = cb; row->n = 0; }
    int ok = 0;
    if (row) {
        klvk_sscopy *c = NULL;
        for (uint32_t i = 0; i < row->n; i++)
            if (row->c[i].src == src && row->c[i].dst == dst) { c = &row->c[i]; break; }
        if (!c && row->n < KLVK_SSC_PER) {
            c = &row->c[row->n];
            if (!row->n++) g_ssc_pending++;
            memset(c, 0, sizeof *c);
            c->src = src; c->dst = dst;
            c->src_layout = a->imageLayout; c->dst_layout = a->resolveImageLayout;
            c->whole = fmt_aspects(sf);
            c->mip = mip; c->dst_mip = dv.base_mip; c->src_layer = sv.base_layer; c->dst_layer = dv.base_layer;
            c->layers = layers;
            c->off.x = x0; c->off.y = y0; c->ext.width = ew; c->ext.height = eh;
        }
        if (c) { c->aspect |= asp; ok = 1; }
    }
    pthread_mutex_unlock(&g_ssc_lock);
    if (!ok) {
        g_rc_ss_nocopy++;
        if (said++ < 8) VKI("single-sample resolve %p -> %p NOT emulated (copy table full)\n",
                            (void *)src, (void *)dst);
        return 0;
    }
    static unsigned said_ok;
    if (said_ok++ < 4)
        VKI("single-sample %s resolve %p -> %p (fmt %u %s, %ux%u+%d+%d, mip %u, layers %u "
            "from %u -> %u, view mask 0x%x) EMULATED as a copy at vkCmdEndRendering\n",
            what == 0 ? "colour" : what == 1 ? "depth" : "stencil", (void *)src, (void *)dst,
            sf, kl_vulkan_format_name((int)sf), ew, eh, x0, y0, mip, layers, sv.base_layer,
            dv.base_layer, ri->viewMask);
    return 1;
}

// After the pass: barrier both images to transfer layouts, copy, barrier back.
static void ssc_flush(VkCommandBuffer cb) {
    if (!g_ssc_pending) return;
    klvk_sscopy c[KLVK_SSC_PER];
    uint32_t n = 0;
    pthread_mutex_lock(&g_ssc_lock);
    for (unsigned i = 0; i < KLVK_SSC_CB; i++)
        if (g_ssc[i].cb == cb && g_ssc[i].n) {
            n = g_ssc[i].n;
            memcpy(c, g_ssc[i].c, n * sizeof *c);
            g_ssc[i].n = 0;
            g_ssc[i].cb = VK_NULL_HANDLE;
            g_ssc_pending--;
            break;
        }
    pthread_mutex_unlock(&g_ssc_lock);
    if (!n) return;
    static PFN_vkCmdPipelineBarrier bar;
    static PFN_vkCmdCopyImage cpy;
    if (!bar || !cpy) {
        klvk_device *d = g_devs[0];
        if (real_gdpa && d) {
            bar = (PFN_vkCmdPipelineBarrier)real_gdpa(d->dev, "vkCmdPipelineBarrier");
            cpy = (PFN_vkCmdCopyImage)real_gdpa(d->dev, "vkCmdCopyImage");
        }
        if (!bar || !cpy) return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const klvk_sscopy *k = &c[i];
        VkImageSubresourceRange rng = { k->whole, k->mip, 1, 0, 0 };
        VkImageMemoryBarrier b[2];
        memset(b, 0, sizeof b);
        for (int j = 0; j < 2; j++) {
            b[j].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b[j].srcQueueFamilyIndex = b[j].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[j].subresourceRange = rng;
            b[j].subresourceRange.layerCount = k->layers;
        }
        b[0].image = k->src; b[0].subresourceRange.baseArrayLayer = k->src_layer;
        b[0].oldLayout = k->src_layout; b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[1].image = k->dst; b[1].subresourceRange.baseArrayLayer = k->dst_layer;
        b[1].subresourceRange.baseMipLevel = k->dst_mip;
        b[1].oldLayout = k->dst_layout; b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, NULL, 0, NULL, 2, b);
        VkImageCopy r;
        memset(&r, 0, sizeof r);
        r.srcSubresource.aspectMask = k->aspect;
        r.srcSubresource.mipLevel = k->mip;
        r.srcSubresource.baseArrayLayer = k->src_layer;
        r.srcSubresource.layerCount = k->layers;
        r.dstSubresource = r.srcSubresource;
        r.dstSubresource.baseArrayLayer = k->dst_layer;
        r.dstSubresource.mipLevel = k->dst_mip;
        r.srcOffset.x = r.dstOffset.x = k->off.x;
        r.srcOffset.y = r.dstOffset.y = k->off.y;
        r.extent.width = k->ext.width; r.extent.height = k->ext.height; r.extent.depth = 1;
        cpy(cb, k->src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            k->dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        cbs_copy(cb);
        b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].newLayout = k->src_layout;
        b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].newLayout = k->dst_layout;
        b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        bar(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, NULL, 0, NULL, 2, b);
    }
}

static void klvk_CmdEndRendering(VkCommandBuffer cb) {
    static PFN_vkCmdEndRendering real;
    if (!real) {
        klvk_device *d = g_devs[0];
        if (real_gdpa && d) real = (PFN_vkCmdEndRendering)real_gdpa(d->dev, "vkCmdEndRendering");
        if (!real && real_gdpa && d)
            real = (PFN_vkCmdEndRendering)real_gdpa(d->dev, "vkCmdEndRenderingKHR");
        if (!real) return;
    }
    real(cb);
    ssc_flush(cb);
}

// One attachment: returns 1 if the copy was changed.
static int rc_fix_att(VkCommandBuffer cb, const VkRenderingInfo *ri,
                      VkRenderingAttachmentInfo *a, const char *what, unsigned idx) {
    int changed = 0;
    VkImage img = VK_NULL_HANDLE;
    int st = view_state(a->imageView, &img);
    if (st) {
        static unsigned said;
        if (said++ < 16)
            VKI("vkCmdBeginRendering: %s attachment %u view %p is %s (image %p) — "
                "dropped from the pass (MoltenVK would fault encoding it)\n",
                what, idx, (void *)a->imageView, k_view_state[st], (void *)img);
        a->imageView = VK_NULL_HANDLE;
        g_rc_dropped++;
        changed = 1;
    }
    if (a->resolveMode != VK_RESOLVE_MODE_NONE) {
        int rst = view_state(a->resolveImageView, &img);
        if (!a->imageView || !a->resolveImageView || rst) {
            static unsigned said;
            if (said++ < 16)
                VKI("vkCmdBeginRendering: %s attachment %u asks resolve mode 0x%x with "
                    "view %p -> resolve view %p (%s) — resolve taken off\n",
                    what, idx, (unsigned)a->resolveMode, (void *)a->imageView,
                    (void *)a->resolveImageView, a->resolveImageView ? k_view_state[rst] : "NULL");
            a->resolveMode = VK_RESOLVE_MODE_NONE;
            a->resolveImageView = VK_NULL_HANDLE;
            changed = 1;
        } else {
            // A resolve FROM a single-sample image: invalid Vulkan
            // (VUID-VkRenderingAttachmentInfo-imageView-06861) that a tiler
            // driver forgives and Metal does not — MoltenVK hands Metal a
            // resolveTexture on a 1-sample texture, which Metal validation
            // rejects and, without validation, hangs the GPU (the native Alyx
            // boot, round N6: kIOGPUCommandBufferCallbackErrorHang on its
            // second frame). Taken off; the source keeps what it rendered.
            VkImage src = VK_NULL_HANDLE;
            (void)view_state(a->imageView, &src);
            uint32_t nsamp = gimg_samples_of(src);
            g_rc_res_any[what[0] == 'c' ? 0 : what[0] == 'd' ? 1 : 2]++;
            if (!nsamp) {
                // Not in the ledger (it holds KLVK_GIMG_N images and a level
                // overflows it): the sample count is unknown, so nothing is
                // decided here — but it is counted, because an uncounted
                // resolve is how a census reads 0 while the pass runs.
                g_rc_res_unknown++;
                static unsigned said_u;
                if (said_u++ < 4)
                    VKI("vkCmdBeginRendering: %s attachment %u asks resolve mode 0x%x from image "
                        "%p the image ledger does NOT hold (overflowed: %u) — sample count "
                        "unknown, passed through\n", what, idx, (unsigned)a->resolveMode,
                        (void *)src, g_gimg_overflow);
            }
            if (nsamp == 1) {
                static unsigned said;
                int wi = what[0] == 'c' ? 0 : what[0] == 'd' ? 1 : 2;
                g_rc_ss_resolve[wi]++;
                {
                    VkImage dimg = VK_NULL_HANDLE;
                    (void)view_state(a->resolveImageView, &dimg);
                    rc_note_dst(dimg);
                }
                int copied = ss_resolve_copy() && ssc_queue(cb, ri, a, src, wi);
                if (copied) {
                    g_rc_ss_copy[wi]++;
                    // The copy reads the source after the pass: keep it.
                    a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                }
                if (said++ < 8)
                    VKI("vkCmdBeginRendering: %s attachment %u asks resolve mode 0x%x from "
                        "SINGLE-SAMPLE image %p (view %p -> resolve view %p) — resolve taken off "
                        "(Metal hangs on it)%s\n", what, idx, (unsigned)a->resolveMode,
                        (void *)src, (void *)a->imageView, (void *)a->resolveImageView,
                        copied ? "; emulated as a copy at vkCmdEndRendering" : "");
                a->resolveMode = VK_RESOLVE_MODE_NONE;
                a->resolveImageView = VK_NULL_HANDLE;
                changed = 1;
            }
        }
    }
    return changed;
}

// KL_VK_RENDERING_TRACE=N: print the first N passes whole — every attachment's
// view, image and resolve, and the pNext chain's sTypes (a density-map or
// shading-rate attachment rides there, not in the arrays).
static void rc_trace(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    // KL_VK_RENDERING_TRACE_FROM=K starts the window at pass K (a steady-state
    // frame, not the first boot passes).
    static int lim = -1, from = 0;
    if (lim < 0) {
        lim = kl_env_int("KL_VK_RENDERING_TRACE", 0);
        from = kl_env_int("KL_VK_RENDERING_TRACE_FROM", 0);
    }
    if ((int)g_rc_calls < from || (int)g_rc_calls > from + lim || !lim) return;
    char chain[160] = "";
    size_t o = 0;
    for (const VkBaseInStructure *b = (const VkBaseInStructure *)ri->pNext; b && o < sizeof chain - 16;
         b = b->pNext)
        o += (size_t)snprintf(chain + o, sizeof chain - o, " %d", (int)b->sType);
    VKI("rendering #%u cb %p flags 0x%x area %dx%d+%d+%d layers %u view-mask 0x%x colours %u depth %p stencil %p next[%s ]\n",
        g_rc_calls, (void *)cb, (unsigned)ri->flags, ri->renderArea.extent.width,
        ri->renderArea.extent.height, ri->renderArea.offset.x, ri->renderArea.offset.y,
        ri->layerCount, ri->viewMask, ri->colorAttachmentCount,
        ri->pDepthAttachment ? (void *)ri->pDepthAttachment->imageView : NULL,
        ri->pStencilAttachment ? (void *)ri->pStencilAttachment->imageView : NULL, chain);
    for (uint32_t i = 0; i < ri->colorAttachmentCount && ri->pColorAttachments; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        VkImage img = VK_NULL_HANDLE;
        int st = view_state(a->imageView, &img);
        VKI("    colour %u view %p (%s, image %p) layout %d resolve 0x%x -> %p load %d store %d\n",
            i, (void *)a->imageView, k_view_state[st], (void *)img, (int)a->imageLayout,
            (unsigned)a->resolveMode, (void *)a->resolveImageView, (int)a->loadOp, (int)a->storeOp);
    }
    for (int k = 0; k < 2; k++) {
        const VkRenderingAttachmentInfo *a = k ? ri->pStencilAttachment : ri->pDepthAttachment;
        if (!a) continue;
        VkImage img = VK_NULL_HANDLE, rimg = VK_NULL_HANDLE;
        int st = view_state(a->imageView, &img);
        (void)view_state(a->resolveImageView, &rimg);
        VKI("    %s view %p (%s, image %p) layout %d resolve 0x%x -> %p (image %p) load %d store %d\n",
            k ? "stencil" : "depth", (void *)a->imageView, k_view_state[st], (void *)img,
            (int)a->imageLayout, (unsigned)a->resolveMode, (void *)a->resolveImageView,
            (void *)rimg, (int)a->loadOp, (int)a->storeOp);
    }
}

// A copy still queued on cb when its next pass begins belongs to a pass whose
// vkCmdEndRendering never came (a reset buffer): it must not run after this one.
static void ssc_forget(VkCommandBuffer cb) {
    if (!g_ssc_pending) return;
    pthread_mutex_lock(&g_ssc_lock);
    for (unsigned i = 0; i < KLVK_SSC_CB; i++)
        if (g_ssc[i].cb == cb && g_ssc[i].n) {
            g_ssc[i].n = 0; g_ssc[i].cb = VK_NULL_HANDLE; g_ssc_pending--;
        }
    pthread_mutex_unlock(&g_ssc_lock);
}

// ---- fixed foveation (round F2, kl_fove.m) --------------------------------
// The eye images are recognised here, where their Vulkan identity is plain: a
// pass whose colour attachment 0 and depth attachment are both exactly the
// double-wide eye kl_openvr told the guest to draw, single-sample. (The final
// composite has the same extent but no depth; it is never an eye.) Their
// MTLTextures go to kl_fove, which sets the rate map on every Metal encoder that
// draws into them. Each new pass shape is logged once. Off: kl_fove_on() is 0
// and nothing below runs.
#define KLVK_FOVE_N 32
static struct { VkImage img; void *tex; } g_fove_img[KLVK_FOVE_N];
static pthread_mutex_t g_fove_lock = PTHREAD_MUTEX_INITIALIZER;

static void *fove_tex_locked(VkImage img) {
    for (int i = 0; i < KLVK_FOVE_N; i++) if (g_fove_img[i].img == img) return g_fove_img[i].tex;
    void *t = eye_mtl_texture(g_devs[0], img);
    if (!t) return NULL;
    for (int i = 0; i < KLVK_FOVE_N; i++)
        if (!g_fove_img[i].img) { g_fove_img[i].img = img; g_fove_img[i].tex = t; return t; }
    g_fove_img[0].img = img; g_fove_img[0].tex = t;
    return t;
}

static void fove_image_gone(VkImage img) {
    if (!img || !kl_fove_on()) return;
    void *t = NULL;
    pthread_mutex_lock(&g_fove_lock);
    for (int i = 0; i < KLVK_FOVE_N; i++)
        if (g_fove_img[i].img == img) { t = g_fove_img[i].tex; g_fove_img[i].img = VK_NULL_HANDLE; g_fove_img[i].tex = NULL; }
    pthread_mutex_unlock(&g_fove_lock);
    if (t) kl_fove_forget(t);
    kl_fove_inter_forget((uint64_t)(uintptr_t)img);   // round G6: a confirmed intermediate is sticky until here
}

static void fove_pass(const VkRenderingInfo *ri) {
    if (!ri || !kl_fove_on()) return;
    if (!ri->colorAttachmentCount || !ri->pColorAttachments || !ri->pDepthAttachment) return;
    VkImageView cv = ri->pColorAttachments[0].imageView, dv = ri->pDepthAttachment->imageView;
    if (!cv || !dv) return;
    VkImage ci = VK_NULL_HANDLE, di = VK_NULL_HANDLE;
    if (view_state(cv, &ci) || view_state(dv, &di) || !ci || !di) return;
    uint32_t cw = 0, ch = 0, cs = 0, dw = 0, dh = 0, ds = 0, cf = 0, df = 0;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(ci);
    if (g) { cw = g->w; ch = g->h; cs = g->samples; cf = g->fmt; }
    g = gimg_find_locked(di);
    if (g) { dw = g->w; dh = g->h; ds = g->samples; df = g->fmt; }
    pthread_mutex_unlock(&g_gimg_lock);
    if (!cw || cw != dw || ch != dh || !kl_fove_eye_extent(cw, ch)) return;
    // The composite Valve hands Submit has the same extent and is sometimes
    // drawn with the eye depth bound (depth-tested overlays): never an eye.
    if (kl_openvr_is_submitted_image((uint64_t)(uintptr_t)ci)) return;
    if ((cs > 1 || ds > 1) && (cs != ds || !kl_fove_msaa())) {
        static int said;
        if (!said++) VKI("[fove] the eye is MSAA (%u/%u samples): %s, this target stays unfoveated\n", cs, ds,
                         cs != ds ? "colour and depth sample counts differ"
                                  : "MSAA eyes are not foveated (KL_FOVE_MSAA=0)");
        return;
    }
    pthread_mutex_lock(&g_fove_lock);
    void *ct = fove_tex_locked(ci), *dt = fove_tex_locked(di);
    pthread_mutex_unlock(&g_fove_lock);
    if (!ct) return;
    kl_fove_note_eye(ct, dt, cw, ch, cs);
    // One line per new pass shape.
    static uint32_t seen[16], nseen;
    uint32_t key = (uint32_t)ri->pColorAttachments[0].loadOp | (uint32_t)ri->pColorAttachments[0].storeOp << 4 |
                   (uint32_t)ri->pDepthAttachment->loadOp << 8 | (uint32_t)ri->pDepthAttachment->storeOp << 12 |
                   (ri->renderArea.extent.width == cw ? 0u : 1u) << 16 | (uint32_t)(ri->flags & 7) << 17;
    for (uint32_t i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 16) seen[nseen++] = key;
    VKI("[fove] marks pass: colour img %p (%ux%u fmt %u, load %d store %d) + depth img %p (fmt %u, load %d "
        "store %d), area %d,%d %ux%u, flags 0x%x, %u colour attachment(s)\n",
        (void *)ci, cw, ch, cf, (int)ri->pColorAttachments[0].loadOp, (int)ri->pColorAttachments[0].storeOp,
        (void *)di, df, (int)ri->pDepthAttachment->loadOp, (int)ri->pDepthAttachment->storeOp,
        ri->renderArea.offset.x, ri->renderArea.offset.y, ri->renderArea.extent.width,
        ri->renderArea.extent.height, (unsigned)ri->flags, ri->colorAttachmentCount);
}

static void rc_begin(VkCommandBuffer cb, const VkRenderingInfo *ri, PFN_vkCmdBeginRendering real) {
    fove_pass(ri);              // kl_fove (off: returns at once)
    g_rc_calls++;
    if (ri && !(ri->flags & VK_RENDERING_RESUMING_BIT))
        cbs_pass(cb, ri->renderArea.extent.width, ri->renderArea.extent.height,
                 (uint32_t)__builtin_popcount(ri->viewMask));
    ssc_forget(cb);
    if (ri) rc_trace(cb, ri);
    if (!rc_on() || !ri) { real(cb, ri); return; }
    VkRenderingInfo mine = *ri;
    VkRenderingAttachmentInfo col[16], dep, sten;
    uint32_t nc = ri->colorAttachmentCount;
    int changed = 0;
    if (nc && ri->pColorAttachments) {
        if (nc > 16) { real(cb, ri); return; }
        memcpy(col, ri->pColorAttachments, nc * sizeof *col);
        for (uint32_t i = 0; i < nc; i++) changed |= rc_fix_att(cb, ri, &col[i], "colour", i);
        mine.pColorAttachments = col;
    }
    if (ri->pDepthAttachment) {
        dep = *ri->pDepthAttachment;
        changed |= rc_fix_att(cb, ri, &dep, "depth", 0);
        mine.pDepthAttachment = &dep;
    }
    if (ri->pStencilAttachment) {
        sten = *ri->pStencilAttachment;
        changed |= rc_fix_att(cb, ri, &sten, "stencil", 0);
        mine.pStencilAttachment = &sten;
    }
    if (changed) g_rc_fixed++;
    if (g_rc_ss_dst_n && mine.pDepthAttachment && mine.pDepthAttachment->imageView) {
        VkImage di = VK_NULL_HANDLE;
        (void)view_state(mine.pDepthAttachment->imageView, &di);
        if (rc_is_dst(di)) {
            g_rc_dst_as_depth++;
            static unsigned said;
            if (said++ < 4)
                VKI("vkCmdBeginRendering: a pass binds single-sample resolve TARGET %p as its "
                    "depth attachment (load %d, %u colour attachment(s), area %ux%u)\n",
                    (void *)di, (int)mine.pDepthAttachment->loadOp, nc,
                    ri->renderArea.extent.width, ri->renderArea.extent.height);
        }
    }
    {
        VkImageView hv[36]; uint32_t hn = 0;
        for (uint32_t i = 0; i < nc && hn < 32; i++) {
            hv[hn++] = mine.pColorAttachments[i].imageView;
            hv[hn++] = mine.pColorAttachments[i].resolveImageView;
        }
        if (mine.pDepthAttachment) { hv[hn++] = mine.pDepthAttachment->imageView;
                                     hv[hn++] = mine.pDepthAttachment->resolveImageView; }
        if (mine.pStencilAttachment) { hv[hn++] = mine.pStencilAttachment->imageView;
                                       hv[hn++] = mine.pStencilAttachment->resolveImageView; }
        cbh_note_views(cb, hv, hn);
    }
    real(cb, changed ? &mine : ri);
}

static void klvk_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    static PFN_vkCmdBeginRendering real;
    if (!real) {
        klvk_device *d = g_devs[0];
        if (real_gdpa && d) real = (PFN_vkCmdBeginRendering)real_gdpa(d->dev, "vkCmdBeginRendering");
        if (!real && real_gdpa && d)
            real = (PFN_vkCmdBeginRendering)real_gdpa(d->dev, "vkCmdBeginRenderingKHR");
        if (!real) return;
    }
    rc_begin(cb, ri, real);
}


// ---------------------------------------------------------------------------
// Command-buffer hold census (the MVKImage NULL fault)
// ---------------------------------------------------------------------------
// MoltenVK does not encode Metal work when a command buffer is RECORDED; it
// replays the recorded MVKImage/MVKImageView pointers at vkQueueSubmit. A
// driver that encodes at record time (turnip, on the Quest) forgives an image
// destroyed between record and submit; MoltenVK reads freed memory (Darwin
// zeroes or reuses it: the NULL MVKImage in encodeStoreActions, the garbage
// one in applyImageMemoryBarrier). So every image a command buffer names (a
// barrier, a copy) and every view (a render pass, dynamic rendering) is held
// against it; every destroy is recorded with WHO destroyed it (thread + frame
// walk) in a dead set; every submit checks its buffers against the dead set
// and names the destroy; a destroy of something a buffer being encoded right
// now names is named too. KL_VK_HOLD_CENSUS=1 turns it on (off by default:
// it takes a global lock on every barrier, render pass and image create; the
// Alyx cause it found is fixed in kl_alyx.c, "the host's flush-wait
// experiment"). KL_VK_SUBMIT_DELAY_MS=N[:count] stretches record->submit so a
// destroy that does not wait shows up every run.
#define KLVK_CBH_N 16384
typedef struct {
    VkCommandBuffer cb;
    uint8_t submitted, in_submit;
    uint32_t n, cap;
    uint64_t tid;
    uint64_t *o;             // VkImageView / VkImage handles it names
} klvk_cbh;
static klvk_cbh g_cbh[KLVK_CBH_N];
#define KLVK_FBH_N 4096
typedef struct { VkFramebuffer fb; uint32_t nv; VkImageView v[16]; } klvk_fbh;
static klvk_fbh g_fbh[KLVK_FBH_N];
#define KLVK_DEAD_N 65536
typedef struct { uint64_t obj; int32_t kill; } klvk_dead;   // kill -1: alive again
static klvk_dead g_dead[KLVK_DEAD_N];
#define KLVK_KILL_N 4096
typedef struct { uint64_t obj, img, tid, t_ns; int kind; void *bt[10]; } klvk_kill;
static klvk_kill g_kill[KLVK_KILL_N];
static unsigned g_kill_i;
#define KLVK_ENC_N 64
static VkCommandBuffer g_enc[KLVK_ENC_N];                     // inside vkQueueSubmit now
static pthread_mutex_t g_cbh_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_cbh_submits, g_cbh_bad_submits, g_cbh_overflow, g_cbh_racing;

static void hold_report(FILE *f);
const char *kl_addr_image(const void *addr, size_t *offset);   // klepton.h
static int hold_on(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_VK_HOLD_CENSUS", 0);
        if (on) kl_fault_add_reporter(hold_report);
    }
    return on;
}

static uint64_t hold_tid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return t; }
static uint64_t hold_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static unsigned ptr_slot(uint64_t h, unsigned n) {
    h ^= h >> 17; h *= 0x9e3779b97f4a7c15ull; h ^= h >> 29;
    return (unsigned)(h % n);
}

// Caller holds g_cbh_lock.
static klvk_cbh *cbh_get_locked(VkCommandBuffer cb, int make) {
    unsigned i = ptr_slot((uint64_t)(uintptr_t)cb, KLVK_CBH_N);
    for (unsigned n = 0; n < KLVK_CBH_N; n++, i = (i + 1) % KLVK_CBH_N) {
        if (g_cbh[i].cb == cb) return &g_cbh[i];
        if (!g_cbh[i].cb) {
            if (!make) return NULL;
            g_cbh[i].cb = cb;
            return &g_cbh[i];
        }
    }
    g_cbh_overflow++;
    return NULL;
}

static klvk_fbh *fbh_get_locked(VkFramebuffer fb, int make) {
    unsigned i = ptr_slot((uint64_t)(uintptr_t)fb, KLVK_FBH_N);
    for (unsigned n = 0; n < KLVK_FBH_N; n++, i = (i + 1) % KLVK_FBH_N) {
        if (g_fbh[i].fb == fb) return &g_fbh[i];
        if (!g_fbh[i].fb) {
            if (!make) return NULL;
            memset(&g_fbh[i], 0, sizeof g_fbh[i]);
            g_fbh[i].fb = fb;
            return &g_fbh[i];
        }
    }
    g_cbh_overflow++;
    return NULL;
}

static klvk_dead *dead_get_locked(uint64_t obj, int make) {
    unsigned i = ptr_slot(obj, KLVK_DEAD_N);
    for (unsigned n = 0; n < KLVK_DEAD_N; n++, i = (i + 1) % KLVK_DEAD_N) {
        if (g_dead[i].obj == obj) return &g_dead[i];
        if (!g_dead[i].obj) {
            if (!make) return NULL;
            g_dead[i].obj = obj; g_dead[i].kill = -1;
            return &g_dead[i];
        }
    }
    g_cbh_overflow++;
    return NULL;
}

// vkCreateImage / vkCreateImageView handed this handle out (again): alive.
static void hold_born(uint64_t obj) {
    if (!obj || !hold_on()) return;
    pthread_mutex_lock(&g_cbh_lock);
    klvk_dead *d = dead_get_locked(obj, 1);
    if (d) d->kill = -1;
    pthread_mutex_unlock(&g_cbh_lock);
}

// vkBeginCommandBuffer: whatever the buffer held before is gone.
static void cbh_begin(VkCommandBuffer cb) {
    if (!cb || !hold_on()) return;
    pthread_mutex_lock(&g_cbh_lock);
    klvk_cbh *h = cbh_get_locked(cb, 0);
    if (h) { h->n = 0; h->submitted = 0; h->tid = hold_tid(); }
    pthread_mutex_unlock(&g_cbh_lock);
}

static void cbh_note_objs(VkCommandBuffer cb, const uint64_t *v, uint32_t n) {
    if (!cb || !n || !hold_on()) return;
    pthread_mutex_lock(&g_cbh_lock);
    klvk_cbh *h = cbh_get_locked(cb, 1);
    if (h) {
        // Submitted and recording again without a vkBeginCommandBuffer we saw
        // (implicit reset through the pool): start over.
        if (h->submitted) { h->submitted = 0; h->n = 0; }
        h->tid = hold_tid();
        for (uint32_t i = 0; i < n; i++) {
            if (!v[i]) continue;
            if (h->n && h->o[h->n - 1] == v[i]) continue;
            if (h->n == h->cap) {
                uint32_t nc = h->cap ? h->cap * 2 : 32;
                uint64_t *no = realloc(h->o, nc * sizeof *no);
                if (!no) { g_cbh_overflow++; continue; }
                h->o = no; h->cap = nc;
            }
            h->o[h->n++] = v[i];
        }
    }
    pthread_mutex_unlock(&g_cbh_lock);
}

static void hold_backtrace(void **bt, int n);
static void hold_print_bt(void *const *bt, int n);

// Record-time check of a barrier/copy image: born through vkCreateImage here
// and not destroyed? An unknown handle is one MoltenVK never saw through this
// seam (or garbage); a dead one is a use after destroy at RECORD time.
static void hold_check_record(VkCommandBuffer cb, const uint64_t *v, uint32_t n, const char *what) {
    if (!hold_on()) return;
    for (uint32_t i = 0; i < n; i++) {
        if (!v[i]) continue;
        pthread_mutex_lock(&g_cbh_lock);
        klvk_dead *d = dead_get_locked(v[i], 0);
        int st = !d ? 1 : d->kill >= 0 ? 2 : 0;
        klvk_kill k; if (st == 2) k = g_kill[d->kill];
        pthread_mutex_unlock(&g_cbh_lock);
        if (!st) continue;
        static unsigned said;
        if (said++ >= 24) continue;
        void *bt[10]; hold_backtrace(bt, 10);
        VKI("HOLD: %s on cb %p (thread %llu) names image 0x%llx that is %s. Recorder:\n",
            what, (void *)cb, (unsigned long long)hold_tid(), (unsigned long long)v[i],
            st == 1 ? "UNKNOWN (never created through vkCreateImage here)" : "DESTROYED");
        hold_print_bt(bt, 10);
        if (st == 2) {
            VKI("    destroyed %.3f ms earlier on thread %llu. Destroyer:\n",
                (double)(hold_ns() - k.t_ns) / 1e6, (unsigned long long)k.tid);
            hold_print_bt(k.bt, 10);
        }
    }
}

static void cbh_note_views(VkCommandBuffer cb, const VkImageView *v, uint32_t n) {
    uint64_t o[40];
    if (n > 40) n = 40;
    for (uint32_t i = 0; i < n; i++) o[i] = (uint64_t)(uintptr_t)v[i];
    cbh_note_objs(cb, o, n);
}

static void hold_backtrace(void **bt, int n) {
    void **fp = (void **)__builtin_frame_address(0);
    pthread_t self = pthread_self();
    char *hi = (char *)pthread_get_stackaddr_np(self);
    char *lo = hi - pthread_get_stacksize_np(self);
    int k = 0;
    for (; k < n && fp && (char *)fp >= lo && (char *)fp + 16 <= hi; k++) {
        bt[k] = fp[1];
        void **next = (void **)fp[0];
        if (next <= fp) { k++; break; }
        fp = next;
    }
    for (; k < n; k++) bt[k] = NULL;
}

static void hold_print_bt(void *const *bt, int n) {
    for (int i = 0; i < n && bt[i]; i++) {
        size_t off = 0;
        const char *img = kl_addr_image(bt[i], &off);
        Dl_info di;
        if (img) VKI("      #%d %s+0x%zx\n", i, img, off);
        else if (dladdr(bt[i], &di) && di.dli_sname)
            VKI("      #%d %s+0x%tx\n", i, di.dli_sname,
                (const char *)bt[i] - (const char *)di.dli_saddr);
        else VKI("      #%d %p\n", i, bt[i]);
    }
}

static const char *const k_kill_what[] = { "vkDestroyImage (the image of a view)",
                                           "vkDestroyImageView", "vkDestroyImage" };

// kind 0: a view's image destroyed (obj = the view), 1: the view destroyed,
// 2: an image destroyed (obj = the image).
static void hold_kill_obj(uint64_t obj, VkImage img, int kind) {
    if (!obj || !hold_on()) return;
    klvk_kill k = { obj, (uint64_t)(uintptr_t)img, hold_tid(), hold_ns(), kind, {0} };
    hold_backtrace(k.bt, 10);
    unsigned racing = 0; VkCommandBuffer rcb = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_cbh_lock);
    int ix = (int)(g_kill_i % KLVK_KILL_N);
    g_kill[ix] = k; g_kill_i++;
    klvk_dead *d = dead_get_locked(obj, 1);
    if (d) d->kill = ix;
    for (unsigned e = 0; e < KLVK_ENC_N; e++) {
        if (!g_enc[e]) continue;
        klvk_cbh *h = cbh_get_locked(g_enc[e], 0);
        if (!h) continue;
        for (uint32_t j = 0; j < h->n; j++) if (h->o[j] == obj) { racing++; rcb = h->cb; break; }
    }
    if (racing) g_cbh_racing++;
    pthread_mutex_unlock(&g_cbh_lock);
    static unsigned said;
    if (racing && said++ < 24) {
        VKI("HOLD: %s 0x%llx on thread %llu WHILE command buffer %p naming it is inside "
            "vkQueueSubmit (MoltenVK is encoding it now). Destroyer:\n",
            k_kill_what[kind], (unsigned long long)obj, (unsigned long long)k.tid, (void *)rcb);
        hold_print_bt(k.bt, 10);
    }
}

static void hold_killed(VkImageView view, VkImage img, int kind) {
    hold_kill_obj((uint64_t)(uintptr_t)view, img, kind);
}

static void hold_killed_image(VkImage img) {
    hold_kill_obj((uint64_t)(uintptr_t)img, img, 2);
}

static void hold_check_submit(const VkCommandBuffer *cbs, uint32_t n) {
    if (!hold_on() || !cbs) return;
    for (uint32_t c = 0; c < n; c++) {
        klvk_kill k; int bad = 0; uint64_t rtid = 0, obj = 0;
        pthread_mutex_lock(&g_cbh_lock);
        g_cbh_submits++;
        klvk_cbh *h = cbh_get_locked(cbs[c], 0);
        if (h) {
            rtid = h->tid;
            for (uint32_t j = 0; j < h->n && !bad; j++) {
                klvk_dead *d = dead_get_locked(h->o[j], 0);
                if (d && d->kill >= 0) { k = g_kill[d->kill]; obj = h->o[j]; bad = 1; }
            }
            h->submitted = 1;
        }
        pthread_mutex_unlock(&g_cbh_lock);
        if (!bad) continue;
        g_cbh_bad_submits++;
        static unsigned said;
        if (said++ >= 24) continue;
        VKI("HOLD: SUBMIT on thread %llu of cb %p (recorded on thread %llu) names 0x%llx, "
            "destroyed %.3f ms earlier on thread %llu (%s) — MoltenVK will encode a freed "
            "object. Destroyer:\n",
            (unsigned long long)hold_tid(), (void *)cbs[c], (unsigned long long)rtid,
            (unsigned long long)obj, (double)(hold_ns() - k.t_ns) / 1e6,
            (unsigned long long)k.tid, k_kill_what[k.kind]);
        hold_print_bt(k.bt, 10);
    }
}

static void hold_mark_submit(const VkCommandBuffer *cbs, uint32_t n, int on) {
    if (!cbs || !n || !hold_on()) return;
    pthread_mutex_lock(&g_cbh_lock);
    for (uint32_t i = 0; i < n; i++) {
        if (on) {
            for (unsigned e = 0; e < KLVK_ENC_N; e++) if (!g_enc[e]) { g_enc[e] = cbs[i]; break; }
        } else {
            for (unsigned e = 0; e < KLVK_ENC_N; e++) if (g_enc[e] == cbs[i]) { g_enc[e] = VK_NULL_HANDLE; break; }
        }
    }
    pthread_mutex_unlock(&g_cbh_lock);
}

// KL_VK_SUBMIT_DELAY_MS=N[:count]: sleep N ms before each of the first `count`
// (default 200) vkQueueSubmits — widens the record->submit window so a destroy
// that does not wait for the submit shows up in the hold census.
static void hold_submit_delay(void) {
    static int ms = -1, count = 200;
    static unsigned done;
    if (ms < 0) {
        const char *e = getenv("KL_VK_SUBMIT_DELAY_MS");
        ms = e ? atoi(e) : 0;
        const char *c = e ? strchr(e, ':') : NULL;
        if (c) count = atoi(c + 1);
    }
    if (ms > 0 && done++ < (unsigned)count) usleep((useconds_t)ms * 1000);
}

static void hold_report(FILE *f) {
    if (!f || !hold_on()) return;
    fprintf(f, "  [vk] hold census: %llu command buffer submit(s) checked, %llu named a destroyed "
               "image/view, %llu destroy(s) raced an encode%s\n",
            (unsigned long long)g_cbh_submits, (unsigned long long)g_cbh_bad_submits,
            (unsigned long long)g_cbh_racing, g_cbh_overflow ? " (a table overflowed)" : "");
}

static PFN_vkVoidFunction hold_real(const char *a, const char *b) {
    klvk_device *d = g_devs[0];
    if (!real_gdpa || !d) return NULL;
    PFN_vkVoidFunction f = real_gdpa(d->dev, a);
    if (!f && b) f = real_gdpa(d->dev, b);
    return f;
}

// ---- compute -> later work: barriers that forget the compute stage
// (round G4 on hla, promoted from hla dc3f16d0)
//
// A guest may record a compute dispatch and then a pipeline barrier whose
// srcStageMask leaves COMPUTE_SHADER out (Half-Life: Alyx's MSAA resolve: ONE
// dispatch, then barriers naming 0x1600 = TRANSFER | COLOR_ATTACHMENT_OUTPUT |
// LATE_FRAGMENT_TESTS — every barrier it records). By the letter of Vulkan that
// orders nothing after the dispatch; a driver that serialises everything hides
// it, MoltenVK does not: the later passes read the dispatch's output while it
// still runs and pick up the PREVIOUS frame's values in whole threadgroup
// blocks ("vertical rectangles from another picture", hla launches 7 and 21).
//
// So: a command buffer that recorded a dispatch gets COMPUTE_SHADER (stage) and
// SHADER_WRITE (access) added to the source scope of its next pipeline barrier
// that does not already name COMPUTE_SHADER / ALL_COMMANDS. Never a wrong
// barrier, at worst a wider one. KL_VK_COMPUTE_BARRIER=0 passes the guest's
// masks through unchanged (default 1, every Vulkan guest).
static int cb_fix_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_COMPUTE_BARRIER", 1);
    return on;
}
#define KLVK_CD_N 1024
static VkCommandBuffer g_cd[KLVK_CD_N];
static pthread_mutex_t g_cd_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_cd_dispatches, g_cd_widened;
static unsigned cd_slot(VkCommandBuffer cb) {
    uint64_t h = (uint64_t)(uintptr_t)cb;
    h ^= h >> 17; h *= 0x9e3779b97f4a7c15ull; h ^= h >> 29;
    return (unsigned)(h % KLVK_CD_N);
}
// Direct-mapped (a collision evicts: at worst one barrier is not widened, or a
// stale mark widens one barrier needlessly — never a wrong barrier).
static void cd_mark(VkCommandBuffer cb) {       // a dispatch was recorded on cb
    pthread_mutex_lock(&g_cd_lock);
    g_cd_dispatches++;
    g_cd[cd_slot(cb)] = cb;
    pthread_mutex_unlock(&g_cd_lock);
}
static int cd_take(VkCommandBuffer cb) {        // 1 = cb has a dispatch no barrier covered yet
    unsigned i = cd_slot(cb);
    int hit = 0;
    pthread_mutex_lock(&g_cd_lock);
    if (g_cd[i] == cb) { g_cd[i] = VK_NULL_HANDLE; hit = 1; }
    if (hit) g_cd_widened++;
    uint64_t w = g_cd_widened, nd = g_cd_dispatches;
    pthread_mutex_unlock(&g_cd_lock);
    if (hit && (w == 1 || w == 1000 || w == 100000))
        VKI("compute barrier: %llu pipeline barrier(s) after a dispatch widened to wait for "
            "COMPUTE_SHADER writes (the guest's own srcStageMask left it out; %llu dispatch(es) so far; "
            "KL_VK_COMPUTE_BARRIER=0 keeps the guest's masks)\n", (unsigned long long)w,
            (unsigned long long)nd);
    return hit;
}

// Dispatches and barriers inside KL_VK_RENDERING_TRACE's window: one
// self-contained line each (cb named), so a multi-threaded recording can be put
// back in order by command buffer. Debug only; costs nothing outside it.
static int cdt_in_window(void) {
    static int lim = -1, from = 0;
    if (lim < 0) {
        lim = kl_env_int("KL_VK_RENDERING_TRACE", 0);
        from = kl_env_int("KL_VK_RENDERING_TRACE_FROM", 0);
    }
    uint32_t now = __atomic_load_n(&g_rc_calls, __ATOMIC_RELAXED);
    return lim && (int)now >= from && (int)now <= from + lim;
}
static void cdt_barrier_note(VkCommandBuffer cb, const char *kind, uint64_t src, uint64_t dst,
                             uint32_t nm, uint32_t nb, uint32_t ni, const VkImage *imgs) {
    if (!cdt_in_window()) return;
    char b[256]; size_t o = 0; b[0] = 0;
    for (uint32_t i = 0; i < ni && i < 6 && o < sizeof b - 24; i++)
        o += (size_t)snprintf(b + o, sizeof b - o, " %p", (void *)imgs[i]);
    VKI("%s cb %p after pass #%u: stages 0x%llx -> 0x%llx, %u memory %u buffer %u image:%s\n", kind,
        (void *)cb, g_rc_calls, (unsigned long long)src, (unsigned long long)dst, nm, nb, ni, b);
}
static void klvk_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    static PFN_vkCmdDispatch real;
    if (!real) real = (PFN_vkCmdDispatch)hold_real("vkCmdDispatch", NULL);
    if (cdt_in_window()) VKI("dispatch cb %p after pass #%u: %ux%ux%u groups\n", (void *)cb, g_rc_calls, x, y, z);
    if (cb_fix_on()) cd_mark(cb);
    if (real) real(cb, x, y, z);
}
static void klvk_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off) {
    static PFN_vkCmdDispatchIndirect real;
    if (!real) real = (PFN_vkCmdDispatchIndirect)hold_real("vkCmdDispatchIndirect", NULL);
    if (cdt_in_window())
        VKI("dispatch-indirect cb %p after pass #%u: buffer %p+%llu\n", (void *)cb, g_rc_calls, (void *)b,
            (unsigned long long)off);
    if (cb_fix_on()) cd_mark(cb);
    if (real) real(cb, b, off);
}
static void klvk_CmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz,
                                 uint32_t x, uint32_t y, uint32_t z) {
    static PFN_vkCmdDispatchBase real;
    if (!real) real = (PFN_vkCmdDispatchBase)hold_real("vkCmdDispatchBase", "vkCmdDispatchBaseKHR");
    if (cdt_in_window()) VKI("dispatch-base cb %p after pass #%u: %ux%ux%u groups\n", (void *)cb, g_rc_calls, x, y, z);
    if (cb_fix_on()) cd_mark(cb);
    if (real) real(cb, bx, by, bz, x, y, z);
}

static void klvk_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src,
                                    VkPipelineStageFlags dst, VkDependencyFlags dep,
                                    uint32_t nm, const VkMemoryBarrier *m,
                                    uint32_t nb, const VkBufferMemoryBarrier *bb,
                                    uint32_t ni, const VkImageMemoryBarrier *ib) {
    static PFN_vkCmdPipelineBarrier real;
    if (!real) real = (PFN_vkCmdPipelineBarrier)hold_real("vkCmdPipelineBarrier", NULL);
    if (!real) return;
    if (ni && ib && hold_on()) {
        uint64_t o[64]; uint32_t k = 0;
        for (uint32_t i = 0; i < ni && k < 64; i++) o[k++] = (uint64_t)(uintptr_t)ib[i].image;
        hold_check_record(cb, o, k, "vkCmdPipelineBarrier");
        cbh_note_objs(cb, o, k);
    }
    if (cdt_in_window()) {
        VkImage im[6]; uint32_t k = 0;
        for (uint32_t i = 0; ib && i < ni && k < 6; i++) im[k++] = ib[i].image;
        cdt_barrier_note(cb, "barrier", src, dst, nm, nb, ni, im);
    }
    if (cb_fix_on() && !(src & (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) &&
        cd_take(cb)) {
        // Source scope += the dispatch's writes: stage, and the access on every
        // barrier (copies — the arrays are the caller's).
        VkMemoryBarrier mm[16]; VkBufferMemoryBarrier bm[16]; VkImageMemoryBarrier im[64];
        if (nm <= 16 && nb <= 16 && ni <= 64) {
            for (uint32_t i = 0; i < nm; i++) { mm[i] = m[i]; mm[i].srcAccessMask |= VK_ACCESS_SHADER_WRITE_BIT; }
            for (uint32_t i = 0; i < nb; i++) { bm[i] = bb[i]; bm[i].srcAccessMask |= VK_ACCESS_SHADER_WRITE_BIT; }
            for (uint32_t i = 0; i < ni; i++) { im[i] = ib[i]; im[i].srcAccessMask |= VK_ACCESS_SHADER_WRITE_BIT; }
            real(cb, src | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst, dep, nm, nm ? mm : m, nb, nb ? bm : bb,
                 ni, ni ? im : ib);
        } else {
            real(cb, src | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst, dep, nm, m, nb, bb, ni, ib);
        }
        return;
    }
    real(cb, src, dst, dep, nm, m, nb, bb, ni, ib);
}

static void klvk_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    static PFN_vkCmdPipelineBarrier2 real;
    if (!real) real = (PFN_vkCmdPipelineBarrier2)hold_real("vkCmdPipelineBarrier2",
                                                          "vkCmdPipelineBarrier2KHR");
    if (!real) return;
    if (di && di->imageMemoryBarrierCount && di->pImageMemoryBarriers && hold_on()) {
        uint64_t o[64]; uint32_t k = 0;
        for (uint32_t i = 0; i < di->imageMemoryBarrierCount && k < 64; i++)
            o[k++] = (uint64_t)(uintptr_t)di->pImageMemoryBarriers[i].image;
        hold_check_record(cb, o, k, "vkCmdPipelineBarrier2");
        cbh_note_objs(cb, o, k);
    }
    if (di && cdt_in_window()) {
        VkImage im[6]; uint32_t k = 0; uint64_t sm = 0, dm = 0;
        for (uint32_t i = 0; i < di->imageMemoryBarrierCount && di->pImageMemoryBarriers; i++) {
            if (k < 6) im[k++] = di->pImageMemoryBarriers[i].image;
            sm |= di->pImageMemoryBarriers[i].srcStageMask; dm |= di->pImageMemoryBarriers[i].dstStageMask;
        }
        for (uint32_t i = 0; i < di->memoryBarrierCount && di->pMemoryBarriers; i++) {
            sm |= di->pMemoryBarriers[i].srcStageMask; dm |= di->pMemoryBarriers[i].dstStageMask;
        }
        for (uint32_t i = 0; i < di->bufferMemoryBarrierCount && di->pBufferMemoryBarriers; i++) {
            sm |= di->pBufferMemoryBarriers[i].srcStageMask; dm |= di->pBufferMemoryBarriers[i].dstStageMask;
        }
        cdt_barrier_note(cb, "barrier2", sm, dm, di->memoryBarrierCount, di->bufferMemoryBarrierCount,
                         di->imageMemoryBarrierCount, im);
    }
    if (di && cb_fix_on() && di->memoryBarrierCount <= 16 && di->bufferMemoryBarrierCount <= 16 &&
        di->imageMemoryBarrierCount <= 64 && cd_take(cb)) {
        const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const VkAccessFlags2 sw = VK_ACCESS_2_SHADER_WRITE_BIT;
        VkMemoryBarrier2 mm[16]; VkBufferMemoryBarrier2 bm[16]; VkImageMemoryBarrier2 im2[64];
        VkDependencyInfo d2 = *di;
        for (uint32_t i = 0; i < di->memoryBarrierCount; i++) {
            mm[i] = di->pMemoryBarriers[i]; mm[i].srcStageMask |= cs; mm[i].srcAccessMask |= sw; }
        for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; i++) {
            bm[i] = di->pBufferMemoryBarriers[i]; bm[i].srcStageMask |= cs; bm[i].srcAccessMask |= sw; }
        for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) {
            im2[i] = di->pImageMemoryBarriers[i]; im2[i].srcStageMask |= cs; im2[i].srcAccessMask |= sw; }
        if (di->memoryBarrierCount) d2.pMemoryBarriers = mm;
        if (di->bufferMemoryBarrierCount) d2.pBufferMemoryBarriers = bm;
        if (di->imageMemoryBarrierCount) d2.pImageMemoryBarriers = im2;
        real(cb, &d2);
        return;
    }
    real(cb, di);
}

static VkResult klvk_CreateFramebuffer(VkDevice dev, const VkFramebufferCreateInfo *ci,
                                       const VkAllocationCallbacks *alloc, VkFramebuffer *out) {
    PFN_vkCreateFramebuffer real =
        (PFN_vkCreateFramebuffer)(real_gdpa ? real_gdpa(dev, "vkCreateFramebuffer") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(dev, ci, alloc, out);
    if (r == VK_SUCCESS && out && ci && (hold_on() || kl_fove_on()) &&
        !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments) {
        pthread_mutex_lock(&g_cbh_lock);
        klvk_fbh *f = fbh_get_locked(*out, 1);
        if (f) {
            f->nv = ci->attachmentCount < 16 ? ci->attachmentCount : 16;
            memcpy(f->v, ci->pAttachments, f->nv * sizeof *f->v);
        }
        pthread_mutex_unlock(&g_cbh_lock);
    }
    return r;
}

static void klvk_DestroyFramebuffer(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyFramebuffer real =
        (PFN_vkDestroyFramebuffer)(real_gdpa ? real_gdpa(dev, "vkDestroyFramebuffer") : NULL);
    if (fb && (hold_on() || kl_fove_on())) {
        pthread_mutex_lock(&g_cbh_lock);
        klvk_fbh *f = fbh_get_locked(fb, 0);
        if (f) f->nv = 0;   // the row stays (a probe chain must not break); a reused handle refills it
        pthread_mutex_unlock(&g_cbh_lock);
    }
    if (real) real(dev, fb, alloc);
}

static void hold_note_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi) {
    if (!bi || !hold_on()) return;
    VkImageView v[16]; uint32_t n = 0;
    for (const VkBaseInStructure *b = (const VkBaseInStructure *)bi->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            const VkRenderPassAttachmentBeginInfo *ab = (const VkRenderPassAttachmentBeginInfo *)b;
            n = ab->attachmentCount < 16 ? ab->attachmentCount : 16;
            if (ab->pAttachments) memcpy(v, ab->pAttachments, n * sizeof *v); else n = 0;
        }
    if (!n) {
        pthread_mutex_lock(&g_cbh_lock);
        klvk_fbh *f = fbh_get_locked(bi->framebuffer, 0);
        if (f) { n = f->nv; memcpy(v, f->v, n * sizeof *v); }
        pthread_mutex_unlock(&g_cbh_lock);
    }
    cbh_note_views(cb, v, n);
}

// ---- fixed foveation of a guest that renders into the runtime's OWN eye images
// (round G3, kl_fove.m). Every Unity/Unreal Vulkan guest here draws its eyes
// with vkCmdBeginRenderPass into the swapchain kl_ovrp (or kl_openxr) handed
// it — usually a 2-layer ARRAY image, both eyes as two slices (multiview) — not
// into an OpenVR double-wide texture, so fove_pass above never sees them. This
// path identifies the eye pass by the images: a render pass whose colour
// attachment 0 IS one of our eye images, or whose resolve attachment 0 is
// (an eye-size MSAA target resolved into the eye inside the pass), with a
// depth attachment of the same extent and sample count. Its MTLTextures go to
// kl_fove_note_eye_array; kl_fove does the rest on the Metal side. Every new
// pass shape — eye pass or a near miss — is logged once with its full
// attachment table. Off: kl_fove_on() is 0, none of these entry points is
// handed out, and the record functions return at once.
#define KLVK_FRP_N 2048
typedef struct {
    VkRenderPass rp;
    uint8_t natt, nsub, ncol;
    int8_t col0, dep, res0;
    uint32_t viewmask;
    // round G6: attachments any subpass writes as colour or resolve (bit per
    // attachment < 8; col0 excluded), and whether the subpasses' view masks differ
    uint8_t outs, vm_mixed;
    // round G7 (Deadpool, 2026-10-09): the attachment a LATER subpass (k >= 1)
    // resolves col0 into (col0 as its colour c, pResolveAttachments[c] set), or
    // -1. UE4's depth-read subpass: subpass 0 draws the MSAA scene, subpass 1
    // LOADs it, reads depth as an input and resolves into the single-sample
    // scene colour on its way out.
    int8_t res_late;
    struct { uint32_t fmt; uint8_t samples, load, store, sload; } a[8];
} klvk_frp;
static klvk_frp g_frp[KLVK_FRP_N];
static pthread_mutex_t g_frp_lock = PTHREAD_MUTEX_INITIALIZER;

static klvk_frp *frp_get_locked(VkRenderPass rp, int make) {
    unsigned i = ptr_slot((uint64_t)(uintptr_t)rp, KLVK_FRP_N);
    for (unsigned n = 0; n < KLVK_FRP_N; n++, i = (i + 1) % KLVK_FRP_N) {
        if (g_frp[i].rp == rp) return &g_frp[i];
        if (!g_frp[i].rp) {
            if (!make) return NULL;
            memset(&g_frp[i], 0, sizeof g_frp[i]);
            g_frp[i].rp = rp;
            return &g_frp[i];
        }
    }
    return NULL;
}

static int8_t frp_ref(uint32_t a) { return a < 8 ? (int8_t)a : (int8_t)-1; }   // VK_ATTACHMENT_UNUSED too

static void fove_rp_note1(const VkRenderPassCreateInfo *ci, VkRenderPass rp) {
    if (!ci || !rp || !kl_fove_on()) return;
    pthread_mutex_lock(&g_frp_lock);
    klvk_frp *r = frp_get_locked(rp, 1);
    if (r) {
        r->natt = (uint8_t)(ci->attachmentCount < 8 ? ci->attachmentCount : 8);
        for (unsigned i = 0; i < r->natt && ci->pAttachments; i++) {
            const VkAttachmentDescription *a = &ci->pAttachments[i];
            r->a[i].fmt = (uint32_t)a->format; r->a[i].samples = (uint8_t)a->samples;
            r->a[i].load = (uint8_t)a->loadOp; r->a[i].store = (uint8_t)a->storeOp;
            r->a[i].sload = (uint8_t)a->stencilLoadOp;
        }
        r->nsub = (uint8_t)(ci->subpassCount < 255 ? ci->subpassCount : 255);
        r->col0 = r->dep = r->res0 = -1; r->ncol = 0; r->viewmask = 0;
        r->outs = r->vm_mixed = 0;
        r->res_late = -1;
        if (ci->subpassCount && ci->pSubpasses) {
            const VkSubpassDescription *s = &ci->pSubpasses[0];
            r->ncol = (uint8_t)s->colorAttachmentCount;
            if (s->colorAttachmentCount && s->pColorAttachments) r->col0 = frp_ref(s->pColorAttachments[0].attachment);
            if (s->colorAttachmentCount && s->pResolveAttachments) r->res0 = frp_ref(s->pResolveAttachments[0].attachment);
            if (s->pDepthStencilAttachment) r->dep = frp_ref(s->pDepthStencilAttachment->attachment);
            for (uint32_t k = 0; k < ci->subpassCount; k++) {
                const VkSubpassDescription *t = &ci->pSubpasses[k];
                for (uint32_t c = 0; c < t->colorAttachmentCount; c++) {
                    int8_t a = t->pColorAttachments ? frp_ref(t->pColorAttachments[c].attachment) : -1;
                    int8_t z = t->pResolveAttachments ? frp_ref(t->pResolveAttachments[c].attachment) : -1;
                    if (a >= 0) r->outs |= (uint8_t)(1u << a);
                    if (z >= 0) r->outs |= (uint8_t)(1u << z);
                    if (k >= 1 && a >= 0 && a == r->col0 && z >= 0) r->res_late = z;
                }
            }
            if (r->col0 >= 0) r->outs &= (uint8_t)~(1u << r->col0);
        }
        for (const VkBaseInStructure *b = (const VkBaseInStructure *)ci->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
                const VkRenderPassMultiviewCreateInfo *mv = (const VkRenderPassMultiviewCreateInfo *)b;
                if (mv->subpassCount && mv->pViewMasks) r->viewmask = mv->pViewMasks[0];
                for (uint32_t k = 1; mv->pViewMasks && k < mv->subpassCount; k++)
                    if (mv->pViewMasks[k] != r->viewmask) r->vm_mixed = 1;
            }
    }
    pthread_mutex_unlock(&g_frp_lock);
}

static void fove_rp_note2(const VkRenderPassCreateInfo2 *ci, VkRenderPass rp) {
    if (!ci || !rp || !kl_fove_on()) return;
    pthread_mutex_lock(&g_frp_lock);
    klvk_frp *r = frp_get_locked(rp, 1);
    if (r) {
        r->natt = (uint8_t)(ci->attachmentCount < 8 ? ci->attachmentCount : 8);
        for (unsigned i = 0; i < r->natt && ci->pAttachments; i++) {
            const VkAttachmentDescription2 *a = &ci->pAttachments[i];
            r->a[i].fmt = (uint32_t)a->format; r->a[i].samples = (uint8_t)a->samples;
            r->a[i].load = (uint8_t)a->loadOp; r->a[i].store = (uint8_t)a->storeOp;
            r->a[i].sload = (uint8_t)a->stencilLoadOp;
        }
        r->nsub = (uint8_t)(ci->subpassCount < 255 ? ci->subpassCount : 255);
        r->col0 = r->dep = r->res0 = -1; r->ncol = 0; r->viewmask = 0;
        r->outs = r->vm_mixed = 0;
        r->res_late = -1;
        if (ci->subpassCount && ci->pSubpasses) {
            const VkSubpassDescription2 *s = &ci->pSubpasses[0];
            r->ncol = (uint8_t)s->colorAttachmentCount;
            if (s->colorAttachmentCount && s->pColorAttachments) r->col0 = frp_ref(s->pColorAttachments[0].attachment);
            if (s->colorAttachmentCount && s->pResolveAttachments) r->res0 = frp_ref(s->pResolveAttachments[0].attachment);
            if (s->pDepthStencilAttachment) r->dep = frp_ref(s->pDepthStencilAttachment->attachment);
            r->viewmask = s->viewMask;
            for (uint32_t k = 0; k < ci->subpassCount; k++) {
                const VkSubpassDescription2 *t = &ci->pSubpasses[k];
                for (uint32_t c = 0; c < t->colorAttachmentCount; c++) {
                    int8_t a = t->pColorAttachments ? frp_ref(t->pColorAttachments[c].attachment) : -1;
                    int8_t z = t->pResolveAttachments ? frp_ref(t->pResolveAttachments[c].attachment) : -1;
                    if (a >= 0) r->outs |= (uint8_t)(1u << a);
                    if (z >= 0) r->outs |= (uint8_t)(1u << z);
                    if (k >= 1 && a >= 0 && a == r->col0 && z >= 0) r->res_late = z;
                }
                if (t->viewMask != r->viewmask) r->vm_mixed = 1;
            }
            if (r->col0 >= 0) r->outs &= (uint8_t)~(1u << r->col0);
        }
    }
    pthread_mutex_unlock(&g_frp_lock);
}

// The OpenXR swapchain colour images this file created (kl_vulkan_xr_image).
#define KLVK_XR_EYE_N 64
static struct { VkImage img; uint32_t layers, w, h; } g_xr_col[KLVK_XR_EYE_N];
static pthread_mutex_t g_xr_col_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_xr_col_n;            // live entries (destroy paths skip the lock at 0)
static void fove_xr_image(VkImage img, uint32_t layers, uint32_t w, uint32_t h) {
    if (!img) return;
    int put = 0;
    pthread_mutex_lock(&g_xr_col_lock);
    for (int i = 0; i < KLVK_XR_EYE_N; i++)
        if (!g_xr_col[i].img) {
            g_xr_col[i].img = img; g_xr_col[i].layers = layers ? layers : 1; g_xr_col[i].w = w; g_xr_col[i].h = h;
            __atomic_add_fetch(&g_xr_col_n, 1, __ATOMIC_RELAXED);
            put = 1;
            break;
        }
    pthread_mutex_unlock(&g_xr_col_lock);
    static int said;
    if (!put && !said++)
        VKI("[fove] OpenXR swapchain image table full (%d colour images live): image %p %ux%u is NOT "
            "recognised as an eye (drawn unfoveated)\n", KLVK_XR_EYE_N, (void *)img, w, h);
}
// An image going away (the guest's vkDestroyImage, or klvk_image_free for the
// runtime's own) drops its entry: a guest that recreates its swapchains must
// not fill the table, and a VkImage handle reused for an unrelated image must
// not be taken for an eye. Runs whether or not foveation is (still) on.
static void fove_xr_image_gone(VkImage img) {
    if (!img || !__atomic_load_n(&g_xr_col_n, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&g_xr_col_lock);
    for (int i = 0; i < KLVK_XR_EYE_N; i++)
        if (g_xr_col[i].img == img) {
            memset(&g_xr_col[i], 0, sizeof g_xr_col[i]);
            __atomic_sub_fetch(&g_xr_col_n, 1, __ATOMIC_RELAXED);
        }
    pthread_mutex_unlock(&g_xr_col_lock);
}

// Is img an eye image the runtime handed the guest? *layers = its array size,
// *eye = 0/1 for a one-eye image (kl_ovrp Stereo layout), -1 for a pair or
// when the owner does not say (an OpenXR swapchain: eye 0 is assumed).
// The runtime's own images are allocated here (klvk_image_alloc), not through
// the guest's vkCreateImage, so the image ledger does not hold them: their
// extent comes from the eye table (*w/*h; 0 when the owner does not record it).
static int fove_runtime_eye_wh(VkImage img, uint32_t *layers, int *eye, uint32_t *w, uint32_t *h) {
    if (!img) return 0;
    for (int s = 0; s < KLVK_EYE_STAGES; s++)
        for (int e = 0; e < 2; e++)
            if (g_eye[s][e].img == img) {
                *layers = g_eye[s][e].layers ? g_eye[s][e].layers : 1;
                *eye = *layers >= 2 ? -1 : e;
                if (w) *w = g_eye[s][e].w;
                if (h) *h = g_eye[s][e].h;
                return 1;
            }
    int hit = 0;
    pthread_mutex_lock(&g_xr_col_lock);
    for (int i = 0; i < KLVK_XR_EYE_N && !hit; i++)
        if (g_xr_col[i].img == img) {
            *layers = g_xr_col[i].layers; *eye = -1; hit = 1;
            if (w) *w = g_xr_col[i].w;
            if (h) *h = g_xr_col[i].h;
        }
    pthread_mutex_unlock(&g_xr_col_lock);
    return hit;
}
static int fove_runtime_eye(VkImage img, uint32_t *layers, int *eye) {
    return fove_runtime_eye_wh(img, layers, eye, NULL, NULL);
}

// Any runtime eye image of this extent? (for the near-miss line)
// Round G6: a target the size of a runtime eye image — or a few px larger, as
// Unreal rounds its scene target up (Deadpool: 5088x3236 scene for a 5088x3234
// eye, which an exact match left silently unfoveated, launch 2 2026-10-09).
// The map is then sized to the INTERMEDIATE (kl_fove_note_eye_scene gets its
// extent) and the expand runs on the intermediate itself, so the eye being the
// smaller of the two changes nothing downstream.
static int fove_eye_extent_known(uint32_t w, uint32_t h) {
    for (int s = 0; s < KLVK_EYE_STAGES; s++)
        for (int e = 0; e < 2; e++)
            if (g_eye[s][e].img && kl_fove_inter_extent_ok(g_eye[s][e].w, g_eye[s][e].h, w, h)) return 1;
    return 0;
}

static void fove_view_range(VkImageView v, uint32_t *base, uint32_t *count) {
    *base = 0; *count = 1;
    pthread_mutex_lock(&g_views_lock);
    klvk_view *r = view_find_locked(v);
    if (r) { *base = r->base_layer; *count = r->layers; }   // layers 0 = to the end
    pthread_mutex_unlock(&g_views_lock);
}

// Round G6: the guest's own eye-size scene target (an "intermediate"). The last
// candidate's command buffer, for the "sampled" confirmation line.
static VkCommandBuffer g_inter_last_cb;
static int fove_probe_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_FOVE_PROBE", 0);
    return on;
}

static void fove_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi) {
    if (!bi || !kl_fove_on()) return;
    klvk_frp r;
    pthread_mutex_lock(&g_frp_lock);
    klvk_frp *rr = frp_get_locked(bi->renderPass, 0);
    if (rr) r = *rr;
    pthread_mutex_unlock(&g_frp_lock);
    if (!rr || r.col0 < 0) return;
    VkImageView v[16]; uint32_t nv = 0;
    for (const VkBaseInStructure *b = (const VkBaseInStructure *)bi->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            const VkRenderPassAttachmentBeginInfo *ab = (const VkRenderPassAttachmentBeginInfo *)b;
            nv = ab->attachmentCount < 16 ? ab->attachmentCount : 16;
            if (ab->pAttachments) memcpy(v, ab->pAttachments, nv * sizeof *v); else nv = 0;
        }
    if (!nv) {
        pthread_mutex_lock(&g_cbh_lock);
        klvk_fbh *f = fbh_get_locked(bi->framebuffer, 0);
        if (f) { nv = f->nv; memcpy(v, f->v, nv * sizeof *v); }
        pthread_mutex_unlock(&g_cbh_lock);
    }
    if ((uint32_t)r.col0 >= nv) return;
    VkImage ci = VK_NULL_HANDLE, di = VK_NULL_HANDLE, ri = VK_NULL_HANDLE;
    if (view_state(v[r.col0], &ci) || !ci) return;
    if (r.dep >= 0 && (uint32_t)r.dep < nv && view_state(v[r.dep], &di)) di = VK_NULL_HANDLE;
    if (r.res0 >= 0 && (uint32_t)r.res0 < nv && view_state(v[r.res0], &ri)) ri = VK_NULL_HANDLE;
    uint32_t cw = 0, ch = 0, cs = 0, cl = 0, dw = 0, dh = 0, ds = 0, rw = 0, rh = 0;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(ci);
    if (g) { cw = g->w; ch = g->h; cs = g->samples; cl = g->layers; }
    if ((g = gimg_find_locked(di))) { dw = g->w; dh = g->h; ds = g->samples; }
    if ((g = gimg_find_locked(ri))) { rw = g->w; rh = g->h; }
    pthread_mutex_unlock(&g_gimg_lock);
    uint32_t el = 0, ew = 0, eh = 0; int ee = -1;
    int direct = fove_runtime_eye_wh(ci, &el, &ee, &ew, &eh);
    if (direct && !cw) { cw = ew; ch = eh; cs = 1; cl = el; }
    // An eye image attached at ANOTHER extent than the colour (resolve, or a
    // later subpass's output): named on the near-miss line instead of silence.
    // RE4 (UE4 4.25) quantized its scene to 4032x3236 over a 4032x3234 eye and
    // launch 1 printed nothing at all; KL_OVRP_EYE_ALIGN=4 is that fix.
    uint32_t mis_w = 0, mis_h = 0;
    int via_res = 0;
    if (!direct && ri && fove_runtime_eye_wh(ri, &el, &ee, &rw, &rh)) {
        if (rw == cw && rh == ch) via_res = 1;
        else { mis_w = rw; mis_h = rh; }
    }
    uint32_t vb = 0, vc = 1;
    fove_view_range(v[r.col0], &vb, &vc);
    uint32_t L = r.viewmask ? (uint32_t)__builtin_popcount(r.viewmask) : (vc ? vc : (cl > vb ? cl - vb : 1));
    // Round G6: the colour is the guest's OWN eye-size target. (a) In-pass: a
    // later subpass of this render pass writes (or resolves into) a runtime eye
    // image — Batman's Manta SRP: 2-sample scene + depth, the post subpass reads
    // the scene as an input attachment and writes the eye. (b) A candidate the
    // guest copies/blits/samples into the eye later: confirmed by that
    // transfer or pass (the confirming frame stays unfoveated), sticky until
    // vkDestroyImage, then marked like an eye on every later frame.
    VkImage oi = VK_NULL_HANDLE; int oj = -1;          // (a): the eye image and its attachment index
    int conf = 0, conf_via = 0, cand = 0;              // (b)
    int inter_on = !direct && !via_res && kl_fove_intermediate() && cw && ch && r.dep >= 0;
    if (inter_on) {
        for (unsigned j = 0; j < r.natt && j < nv && !oi; j++) {
            if (!(r.outs >> j & 1)) continue;
            VkImage im = VK_NULL_HANDLE; uint32_t ol = 0, ow = 0, oh = 0; int oe = -1;
            if (view_state(v[j], &im) || !im) continue;
            if (fove_runtime_eye_wh(im, &ol, &oe, &ow, &oh)) {
                if (!ow || (ow == cw && oh == ch)) { oi = im; oj = (int)j; el = ol; ee = oe; }
                else if (!mis_w) { mis_w = ow; mis_h = oh; }
            }
        }
        if (!oi && fove_eye_extent_known(cw, ch)) {
            int st = kl_fove_inter_note((uint64_t)(uintptr_t)ci, &conf_via);
            if (st == 2) { conf = 1; el = cl; ee = -1; }
            else { cand = 1; g_inter_last_cb = cb; }
        }
    }
    // Why this pass is (not) an eye pass, for the once-per-shape line.
    const char *why = NULL;
    static __thread char mis_why[256];
    if (!direct && !via_res && !oi && !conf && mis_w) {
        snprintf(mis_why, sizeof mis_why, "a runtime eye image (%ux%u) is attached as the resolve or a later "
                 "subpass's output at ANOTHER extent than the colour (%ux%u) — the guest quantized its scene buffer; "
                 "KL_OVRP_EYE_ALIGN=4 sizes the eye to match an Unreal guest", mis_w, mis_h, cw, ch);
        why = mis_why;
    } else if (!direct && !via_res && !oi && !conf)
        why = r.dep >= 0 && fove_eye_extent_known(cw, ch)
            ? (cand ? "eye-size pass that does not draw into (or resolve into) a runtime eye image — CANDIDATE "
                      "intermediate: marked from the next frame on once a copy/blit/resolve into the eye, or a pass "
                      "drawing into the eye after it, confirms it"
                    : "eye-size pass that does not draw into (or resolve into) a runtime eye image — an intermediate "
                      "the guest copies/blits later is not foveated by this path (KL_FOVE_INTERMEDIATE=0)")
            : NULL;
    else if (!di) why = "no depth attachment (a post/blit pass, not the scene)";
    else if (dw != cw || dh != ch) why = "depth extent differs from colour";
    else if (cs != ds) why = "colour and depth sample counts differ";
    else if (cs > 1 && !kl_fove_msaa()) why = "MSAA and KL_FOVE_MSAA=0";
    else if (L >= 2 && (r.viewmask ? (r.viewmask & 3u) != 3u || L != 2 : vb != 0 || L != 2))
        why = "a multiview pass over other than views/slices 0 and 1";
    else if (L >= 2 && el < 2) why = "multiview pass over a one-layer image";
    else if (oi && r.a[r.col0].store != (uint8_t)VK_ATTACHMENT_STORE_OP_DONT_CARE)
        why = "in-pass intermediate whose scene colour is STORED (it outlives the pass in physical layout)";
    else if (oi && (uint32_t)oj < r.natt && r.a[oj].load == (uint8_t)VK_ATTACHMENT_LOAD_OP_LOAD)
        why = "in-pass intermediate whose eye attachment is LOADed";
    else if (oi && r.vm_mixed) why = "in-pass intermediate whose subpasses have different view masks";
    int eye_pass = (direct || via_res || oi || conf) && !why;
    if (eye_pass) {
        pthread_mutex_lock(&g_fove_lock);
        void *ct = fove_tex_locked(ci), *dt = fove_tex_locked(di), *ot = oi ? fove_tex_locked(oi) : NULL;
        pthread_mutex_unlock(&g_fove_lock);
        int lay = L >= 2 ? 2 : 1, one = L >= 2 ? -1 : (ee == 1 ? 1 : 0);
        if (!ct || (oi && !ot)) why = "no MTLTexture behind the colour (or eye) image", eye_pass = 0;
        else if (oi) {
            kl_fove_note_eye_scene(ct, dt, ot, cw, ch, cs, (uint32_t)lay, one, KL_FOVE_VIA_SUBPASS);
            kl_fove_inter_note((uint64_t)(uintptr_t)ci, NULL);
            if (kl_fove_inter_confirm((uint64_t)(uintptr_t)ci, KL_FOVE_VIA_SUBPASS) == 1)
                VKI("[fove] intermediate CONFIRMED: VkImage %p -> eye image %p via subpass (attachment #%d of render "
                    "pass %p, written by a later subpass; %u subpass(es), scene %ux%u x%u)\n", (void *)ci, (void *)oi,
                    oj, (void *)bi->renderPass, r.nsub, cw, ch, cs);
        } else if (conf) {
            kl_fove_note_eye_scene(ct, dt, NULL, cw, ch, cs, (uint32_t)lay, one, conf_via);
            // Round G7 (Deadpool): an MSAA scene the guest does NOT store
            // (storeOp DONT_CARE) and a later subpass of this pass resolves into
            // a single-sample attachment. Its samples die with the pass, so kl_fove
            // may leave them in physical layout across the subpass encoders and
            // expand only what the resolve writes (no per-sample expand).
            int tr = cs > 1 && r.a[r.col0].store == (uint8_t)VK_ATTACHMENT_STORE_OP_DONT_CARE && r.res_late >= 0 &&
                     r.res_late < (int8_t)r.natt && r.a[r.res_late].samples == 1;
            kl_fove_note_ms_transient(ct, tr);
            static int said_tr;
            if (tr && !said_tr++)
                VKI("[fove] intermediate %p: MSAA x%u scene with storeOp DONT_CARE, resolved by a later subpass into "
                    "attachment #%d (x1): its samples stay in physical layout through the pass, the resolve target "
                    "is what gets expanded\n", (void *)ci, cs, r.res_late);
        } else kl_fove_note_eye_array(ct, dt, cw, ch, cs, (uint32_t)lay, one);
    }
    // A post pass drawing into an eye image (no depth): it may be the one that
    // samples the last candidate intermediate into the eye.
    if (direct && !di && kl_fove_intermediate()) {
        uint64_t last = kl_fove_inter_last();
        static int probed;
        if (fove_probe_on() && probed < 4) {
            probed++;
            VKI("[fove] probe: pass into eye image %p with no depth (render pass %p, %u subpass(es), area %ux%u); last "
                "candidate intermediate %p (same command buffer: %s)\n", (void *)ci, (void *)bi->renderPass, r.nsub,
                bi->renderArea.extent.width, bi->renderArea.extent.height, (void *)(uintptr_t)last,
                last && g_inter_last_cb == cb ? "yes" : "no");
        }
        if (last && kl_fove_inter_confirm(last, KL_FOVE_VIA_SAMPLED) == 1)
            VKI("[fove] intermediate CONFIRMED: VkImage %p -> eye image %p via sampled (the next pass drawing into "
                "the eye after its pass; same command buffer: %s)\n", (void *)(uintptr_t)last, (void *)ci,
                g_inter_last_cb == cb ? "yes" : "no");
    }
    if (!eye_pass && !why) return;   // an unrelated pass: silent
    // One line per new shape (render pass + which images).
    static struct { VkRenderPass rp; int eye; } seen[64];
    static unsigned nseen;
    static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
    int fresh = 1;
    pthread_mutex_lock(&seen_lock);
    for (unsigned i = 0; i < nseen; i++) if (seen[i].rp == bi->renderPass && seen[i].eye == eye_pass) fresh = 0;
    if (fresh && nseen < 64) { seen[nseen].rp = bi->renderPass; seen[nseen].eye = eye_pass; nseen++; }
    else if (fresh) fresh = 0;
    pthread_mutex_unlock(&seen_lock);
    if (!fresh) return;
    char att[512]; int o = 0;
    for (unsigned i = 0; i < r.natt && o < (int)sizeof att - 64; i++)
        o += snprintf(att + o, sizeof att - (size_t)o, "%s#%u fmt %u x%u load %u store %u%s", i ? "; " : "", i,
                      r.a[i].fmt, r.a[i].samples, r.a[i].load, r.a[i].store,
                      (int)i == r.col0 ? " =colour" : (int)i == r.dep ? " =depth" : (int)i == r.res0 ? " =resolve" : "");
    if (!eye_pass) {
        char eyes[256]; int eo = 0;
        for (int s2 = 0; s2 < KLVK_EYE_STAGES && eo < (int)sizeof eyes - 40; s2++)
            for (int e2 = 0; e2 < 2; e2++)
                if (g_eye[s2][e2].img) eo += snprintf(eyes + eo, sizeof eyes - (size_t)eo, " s%d/e%d=%p", s2, e2, (void *)g_eye[s2][e2].img);
        VKI("[fove] near miss images: colour %p depth %p resolve %p (resolve %ux%u); runtime eye images:%s\n",
            (void *)ci, (void *)di, (void *)ri, rw, rh, eo ? eyes : " none");
    }
    VKI("[fove] %s render pass %p: %s %ux%u x%u (%u layer(s), view base %u count %u, viewMask 0x%x)%s, depth %ux%u x%u, "
        "%u subpass(es), %u colour attachment(s), area %d,%d %ux%u | %s%s%s\n",
        eye_pass ? "EYE PASS (marked)" : "near miss, NOT marked:", (void *)bi->renderPass,
        direct ? "colour = runtime eye image" : via_res ? "MSAA colour resolved into a runtime eye image"
        : oi ? "eye-size intermediate, a later subpass writes a runtime eye image"
        : conf ? "confirmed eye-size intermediate (the guest transfers/samples it into the eye)" : "colour",
        cw, ch, cs, cl, vb, vc, r.viewmask, via_res ? "" : "", dw, dh, ds, r.nsub, r.ncol,
        bi->renderArea.offset.x, bi->renderArea.offset.y, bi->renderArea.extent.width, bi->renderArea.extent.height,
        att, why ? " | why: " : "", why ? why : "");
}


// Where an eye-size intermediate reaches the runtime's eye image (round G3
// census, fove only): the first transfer of each kind into one is named once,
// with its first region's shape (KL_FOVE_PROBE=1: the first 4 of each kind).
// Round G6: a transfer from a CANDIDATE intermediate confirms it.
static void fove_xfer_note(const char *what, const char *via, VkImage src, VkImage dst, uint32_t n,
                           const VkImageSubresourceLayers *ss, const VkImageSubresourceLayers *ds,
                           int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h) {
    uint32_t el = 0; int ee = -1;
    if (!kl_fove_on() || !fove_runtime_eye(dst, &el, &ee)) return;
    if (kl_fove_intermediate() && kl_fove_inter_confirm((uint64_t)(uintptr_t)src, KL_FOVE_VIA_XFER) == 1)
        VKI("[fove] intermediate CONFIRMED: VkImage %p -> eye image %p via %s (%s)\n", (void *)src, (void *)dst, via, what);
    static struct { const char *what; int n; } said[8]; static unsigned nsaid;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    int k = -1, show = 0;
    pthread_mutex_lock(&lk);
    for (unsigned i = 0; i < nsaid; i++) if (said[i].what == what) k = (int)i;
    if (k < 0 && nsaid < 8) { k = (int)nsaid++; said[k].what = what; said[k].n = 0; }
    if (k >= 0 && said[k].n < (fove_probe_on() ? 4 : 1)) { said[k].n++; show = 1; }
    pthread_mutex_unlock(&lk);
    if (!show) return;
    uint32_t sw = 0, sh = 0, sm = 0, sl = 0;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(src);
    if (g) { sw = g->w; sh = g->h; sm = g->samples; sl = g->layers; }
    pthread_mutex_unlock(&g_gimg_lock);
    int ist = kl_fove_inter_state((uint64_t)(uintptr_t)src, NULL);
    VKI("[fove] %s INTO a runtime eye image %p (%u layer(s)) from %p %ux%u x%u (%u layer(s)), %u region(s) — "
        "the eye content is drawn elsewhere and transferred | region 0: src layers %u+%u at %d,%d -> dst layers "
        "%u+%u at %d,%d, %ux%u; source is %s\n", what, (void *)dst, el, (void *)src, sw, sh, sm, sl, n,
        ss ? ss->baseArrayLayer : 0, ss ? ss->layerCount : 0, sx, sy, ds ? ds->baseArrayLayer : 0,
        ds ? ds->layerCount : 0, dx, dy, w, h,
        ist == 2 ? "a CONFIRMED intermediate" : ist == 1 ? "a candidate intermediate"
        : "not a recorded eye-size pass target");
}
static void klvk_CmdCopyImageF(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                               uint32_t n, const VkImageCopy *r) {
    static PFN_vkCmdCopyImage real;
    if (!real) real = (PFN_vkCmdCopyImage)hold_real("vkCmdCopyImage", NULL);
    if (n && r) fove_xfer_note("vkCmdCopyImage", "copy", src, dst, n, &r->srcSubresource, &r->dstSubresource,
                               r->srcOffset.x, r->srcOffset.y, r->dstOffset.x, r->dstOffset.y, r->extent.width, r->extent.height);
    if (real) real(cb, src, sl, dst, dl, n, r);
}
static void klvk_CmdBlitImageF(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                               uint32_t n, const VkImageBlit *r, VkFilter f) {
    static PFN_vkCmdBlitImage real;
    if (!real) real = (PFN_vkCmdBlitImage)hold_real("vkCmdBlitImage", NULL);
    if (n && r) fove_xfer_note("vkCmdBlitImage", "blit", src, dst, n, &r->srcSubresource, &r->dstSubresource,
                               r->srcOffsets[0].x, r->srcOffsets[0].y, r->dstOffsets[0].x, r->dstOffsets[0].y,
                               (uint32_t)(r->dstOffsets[1].x - r->dstOffsets[0].x),
                               (uint32_t)(r->dstOffsets[1].y - r->dstOffsets[0].y));
    if (real) real(cb, src, sl, dst, dl, n, r, f);
}
static void klvk_CmdResolveImageF(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                  uint32_t n, const VkImageResolve *r) {
    static PFN_vkCmdResolveImage real;
    if (!real) real = (PFN_vkCmdResolveImage)hold_real("vkCmdResolveImage", NULL);
    if (n && r) fove_xfer_note("vkCmdResolveImage", "resolve", src, dst, n, &r->srcSubresource, &r->dstSubresource,
                               r->srcOffset.x, r->srcOffset.y, r->dstOffset.x, r->dstOffset.y, r->extent.width, r->extent.height);
    if (real) real(cb, src, sl, dst, dl, n, r);
}
// The Vulkan 1.3 / KHR_copy_commands2 spellings (round G6: Unity and Unreal
// may use either; the census above only saw the 1.0 ones).
static void klvk_CmdCopyImage2F(VkCommandBuffer cb, const VkCopyImageInfo2 *i) {
    static PFN_vkCmdCopyImage2 real;
    if (!real) real = (PFN_vkCmdCopyImage2)hold_real("vkCmdCopyImage2", "vkCmdCopyImage2KHR");
    if (i && i->regionCount && i->pRegions) {
        const VkImageCopy2 *r = i->pRegions;
        fove_xfer_note("vkCmdCopyImage2", "copy", i->srcImage, i->dstImage, i->regionCount, &r->srcSubresource,
                       &r->dstSubresource, r->srcOffset.x, r->srcOffset.y, r->dstOffset.x, r->dstOffset.y,
                       r->extent.width, r->extent.height);
    }
    if (real) real(cb, i);
}
static void klvk_CmdBlitImage2F(VkCommandBuffer cb, const VkBlitImageInfo2 *i) {
    static PFN_vkCmdBlitImage2 real;
    if (!real) real = (PFN_vkCmdBlitImage2)hold_real("vkCmdBlitImage2", "vkCmdBlitImage2KHR");
    if (i && i->regionCount && i->pRegions) {
        const VkImageBlit2 *r = i->pRegions;
        fove_xfer_note("vkCmdBlitImage2", "blit", i->srcImage, i->dstImage, i->regionCount, &r->srcSubresource,
                       &r->dstSubresource, r->srcOffsets[0].x, r->srcOffsets[0].y, r->dstOffsets[0].x, r->dstOffsets[0].y,
                       (uint32_t)(r->dstOffsets[1].x - r->dstOffsets[0].x), (uint32_t)(r->dstOffsets[1].y - r->dstOffsets[0].y));
    }
    if (real) real(cb, i);
}
static void klvk_CmdResolveImage2F(VkCommandBuffer cb, const VkResolveImageInfo2 *i) {
    static PFN_vkCmdResolveImage2 real;
    if (!real) real = (PFN_vkCmdResolveImage2)hold_real("vkCmdResolveImage2", "vkCmdResolveImage2KHR");
    if (i && i->regionCount && i->pRegions) {
        const VkImageResolve2 *r = i->pRegions;
        fove_xfer_note("vkCmdResolveImage2", "resolve", i->srcImage, i->dstImage, i->regionCount, &r->srcSubresource,
                       &r->dstSubresource, r->srcOffset.x, r->srcOffset.y, r->dstOffset.x, r->dstOffset.y,
                       r->extent.width, r->extent.height);
    }
    if (real) real(cb, i);
}

static void klvk_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                    VkSubpassContents c) {
    static PFN_vkCmdBeginRenderPass real;
    if (!real) real = (PFN_vkCmdBeginRenderPass)hold_real("vkCmdBeginRenderPass", NULL);
    if (!real) return;
    if (bi) cbs_pass(cb, bi->renderArea.extent.width, bi->renderArea.extent.height, 1);
    hold_note_rp(cb, bi);
    fove_rp(cb, bi);            // KL_FOVEATION (off: returns at once)
    real(cb, bi, c);
}

static void klvk_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                     const VkSubpassBeginInfo *si) {
    static PFN_vkCmdBeginRenderPass2 real;
    if (!real) real = (PFN_vkCmdBeginRenderPass2)hold_real("vkCmdBeginRenderPass2",
                                                          "vkCmdBeginRenderPass2KHR");
    if (!real) return;
    hold_note_rp(cb, bi);
    fove_rp(cb, bi);            // KL_FOVEATION (off: returns at once)
    real(cb, bi, si);
}

// ---------------------------------------------------------------------------
// Who reads a single-sample resolve's TARGET (KL_VK_SS_CONSUMERS=1, round N12-B)
// ---------------------------------------------------------------------------
// The census above counts the resolves; this says whether the guest's own
// shaders ever see the image they land in (a sampled/input/storage descriptor
// naming a view of it), or whether it only leaves through IVRCompositor::
// Submit (the depth the compositor may reproject with, which kl_openvr
// ignores). Both write paths are covered: vkUpdateDescriptorSets and the
// descriptor-update templates Source 2 uses. Out of the dispatch when off.
static int ssc_cons_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_SS_CONSUMERS", 0);
    return on;
}
static uint32_t g_cons_writes, g_cons_tmpl;

#define KLVK_CONS_N 16384
static struct { VkImage img; uint32_t n; int type; } g_cons[KLVK_CONS_N];
static pthread_mutex_t g_cons_lock = PTHREAD_MUTEX_INITIALIZER;

// Every image any image descriptor ever names, with a count — a resolve target
// is often bound once, before its first resolve, so it is looked up later.
static void cons_note(VkImageView v, int type, const char *how) {
    if (!v) return;
    VkImage img = VK_NULL_HANDLE;
    (void)view_state(v, &img);
    if (!img) return;
    __atomic_add_fetch(how[0] == 't' ? &g_cons_tmpl : &g_cons_writes, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_cons_lock);
    unsigned h = (unsigned)((((uintptr_t)img) >> 4) * 2654435761u % KLVK_CONS_N);
    for (unsigned n = 0; n < KLVK_CONS_N; n++, h = (h + 1) % KLVK_CONS_N)
        if (!g_cons[h].img || g_cons[h].img == img) {
            g_cons[h].img = img; g_cons[h].n++; g_cons[h].type = type;
            break;
        }
    pthread_mutex_unlock(&g_cons_lock);
}

static void cons_report(FILE *f) {
    if (!ssc_cons_on() || !f) return;
    fprintf(f, "  [vk] ss-consumers: %u descriptor write(s), %u template write(s) of image "
               "descriptors seen; single-sample resolve targets:", g_cons_writes, g_cons_tmpl);
    pthread_mutex_lock(&g_cons_lock);
    for (uint32_t i = 0; i < g_rc_ss_dst_n; i++) {
        uint32_t n = 0; int t = -1;
        unsigned h = (unsigned)((((uintptr_t)g_rc_ss_dst[i]) >> 4) * 2654435761u % KLVK_CONS_N);
        for (unsigned k = 0; k < KLVK_CONS_N && g_cons[h].img; k++, h = (h + 1) % KLVK_CONS_N)
            if (g_cons[h].img == g_rc_ss_dst[i]) { n = g_cons[h].n; t = g_cons[h].type; break; }
        fprintf(f, " %p named by %u descriptor(s) (type %d)", (void *)g_rc_ss_dst[i], n, t);
    }
    pthread_mutex_unlock(&g_cons_lock);
    fprintf(f, "%s\n", g_rc_ss_dst_n ? "" : " none yet");
}

static int desc_is_image(int t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

static void klvk_UpdateDescriptorSets(VkDevice dev, uint32_t nw, const VkWriteDescriptorSet *w,
                                      uint32_t nc, const VkCopyDescriptorSet *c) {
    static PFN_vkUpdateDescriptorSets real;
    if (!real) real = (PFN_vkUpdateDescriptorSets)(real_gdpa ? real_gdpa(dev, "vkUpdateDescriptorSets") : NULL);
    if (!real) return;
    for (uint32_t i = 0; w && i < nw; i++)
        if (desc_is_image((int)w[i].descriptorType) && w[i].pImageInfo)
            for (uint32_t k = 0; k < w[i].descriptorCount; k++)
                cons_note(w[i].pImageInfo[k].imageView, (int)w[i].descriptorType, "write");
    real(dev, nw, w, nc, c);
}

#define KLVK_TMPL_N 1024
typedef struct { VkDescriptorUpdateTemplate t; uint32_t n; VkDescriptorUpdateTemplateEntry *e; } klvk_tmpl;
static klvk_tmpl g_tmpl[KLVK_TMPL_N];
static pthread_mutex_t g_tmpl_lock = PTHREAD_MUTEX_INITIALIZER;

static VkResult klvk_CreateDescriptorUpdateTemplate(VkDevice dev,
        const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkDescriptorUpdateTemplate *out) {
    PFN_vkCreateDescriptorUpdateTemplate real = (PFN_vkCreateDescriptorUpdateTemplate)
        (real_gdpa ? real_gdpa(dev, "vkCreateDescriptorUpdateTemplate") : NULL);
    if (!real) real = (PFN_vkCreateDescriptorUpdateTemplate)
        (real_gdpa ? real_gdpa(dev, "vkCreateDescriptorUpdateTemplateKHR") : NULL);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(dev, ci, alloc, out);
    if (r != VK_SUCCESS || !ci || !out) return r;
    uint32_t ni = 0;
    for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; i++)
        ni += desc_is_image((int)ci->pDescriptorUpdateEntries[i].descriptorType);
    if (!ni) return r;
    VkDescriptorUpdateTemplateEntry *e = calloc(ni, sizeof *e);
    if (!e) return r;
    for (uint32_t i = 0, k = 0; i < ci->descriptorUpdateEntryCount; i++)
        if (desc_is_image((int)ci->pDescriptorUpdateEntries[i].descriptorType))
            e[k++] = ci->pDescriptorUpdateEntries[i];
    pthread_mutex_lock(&g_tmpl_lock);
    unsigned h = (unsigned)(((uintptr_t)*out >> 4) % KLVK_TMPL_N);
    for (unsigned n = 0; n < KLVK_TMPL_N; n++, h = (h + 1) % KLVK_TMPL_N)
        if (!g_tmpl[h].t || g_tmpl[h].t == *out) {
            free(g_tmpl[h].e);
            g_tmpl[h].t = *out; g_tmpl[h].n = ni; g_tmpl[h].e = e; e = NULL;
            break;
        }
    pthread_mutex_unlock(&g_tmpl_lock);
    free(e);
    return r;
}

static void klvk_UpdateDescriptorSetWithTemplate(VkDevice dev, VkDescriptorSet set,
        VkDescriptorUpdateTemplate t, const void *data) {
    static PFN_vkUpdateDescriptorSetWithTemplate real;
    if (!real) real = (PFN_vkUpdateDescriptorSetWithTemplate)
        (real_gdpa ? real_gdpa(dev, "vkUpdateDescriptorSetWithTemplate") : NULL);
    if (!real) real = (PFN_vkUpdateDescriptorSetWithTemplate)
        (real_gdpa ? real_gdpa(dev, "vkUpdateDescriptorSetWithTemplateKHR") : NULL);
    if (!real) return;
    if (data) {
        pthread_mutex_lock(&g_tmpl_lock);
        unsigned h = (unsigned)(((uintptr_t)t >> 4) % KLVK_TMPL_N);
        klvk_tmpl *row = NULL;
        for (unsigned n = 0; n < KLVK_TMPL_N && g_tmpl[h].t; n++, h = (h + 1) % KLVK_TMPL_N)
            if (g_tmpl[h].t == t) { row = &g_tmpl[h]; break; }
        if (row)
            for (uint32_t i = 0; i < row->n; i++)
                for (uint32_t k = 0; k < row->e[i].descriptorCount; k++) {
                    const VkDescriptorImageInfo *ii = (const VkDescriptorImageInfo *)
                        ((const uint8_t *)data + row->e[i].offset + k * row->e[i].stride);
                    cons_note(ii->imageView, (int)row->e[i].descriptorType, "template");
                }
        pthread_mutex_unlock(&g_tmpl_lock);
    }
    real(dev, set, t, data);
}

// ---------------------------------------------------------------------------
// The depthBounds feature as the guest sees it (KL_VK_DEPTH_BOUNDS)
// ---------------------------------------------------------------------------
// MoltenVK reports VkPhysicalDeviceFeatures.depthBounds on the headset (Apple
// GPU family, visionOS) and NOT on the Mac, and Alyx enables it there. Round
// N12-B suspected it of the headset's dark level and ruled it out: faked ON on
// the Mac, no pipeline the guest creates turns the depth-bounds test on (the
// depth census counts them) and the level renders unchanged. Kept as the A/B
// switch for the next guest that does use it:
//   KL_VK_DEPTH_BOUNDS=real  (default) what MoltenVK reports
//   KL_VK_DEPTH_BOUNDS=fake  report it ON (the Mac sees the headset's feature
//                            set; vkCreateDevice's feature audit strips the
//                            request again, and MoltenVK ignores the test)
//   KL_VK_DEPTH_BOUNDS=off   report it OFF (the headset sees the Mac's)
static int depth_bounds_mode(void) {          // 0 real, 1 fake on, 2 off
    static int m = -1;
    if (m < 0) {
        const char *s = kl_env_str("KL_VK_DEPTH_BOUNDS", NULL);
        m = !s || !*s || !strcmp(s, "real") ? 0 : !strcmp(s, "fake") || !strcmp(s, "on") ? 1
          : !strcmp(s, "off") || !strcmp(s, "0") ? 2 : 0;
    }
    return m;
}

static void depth_bounds_apply(VkPhysicalDeviceFeatures *f) {
    if (!f) return;
    int m = depth_bounds_mode();
    VkBool32 was = f->depthBounds;
    if (m == 1) f->depthBounds = VK_TRUE;
    else if (m == 2) f->depthBounds = VK_FALSE;
    static int said;
    if (!said++)
        VKI("guest reads VkPhysicalDeviceFeatures.depthBounds = %u (MoltenVK reports %u; "
            "KL_VK_DEPTH_BOUNDS=%s)\n", (unsigned)f->depthBounds, (unsigned)was,
            m == 1 ? "fake" : m == 2 ? "off" : "real");
}

static void klvk_GetPhysicalDeviceFeatures(VkPhysicalDevice phys, VkPhysicalDeviceFeatures *out) {
    PFN_vkGetPhysicalDeviceFeatures real = (PFN_vkGetPhysicalDeviceFeatures)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures");
    if (!real || !out) return;
    real(phys, out);
    depth_bounds_apply(out);
}

static void klvk_GetPhysicalDeviceFeatures2(VkPhysicalDevice phys, VkPhysicalDeviceFeatures2 *out) {
    PFN_vkGetPhysicalDeviceFeatures2 real = (PFN_vkGetPhysicalDeviceFeatures2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
    if (!real) real = (PFN_vkGetPhysicalDeviceFeatures2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!real || !out) return;
    real(phys, out);
    depth_bounds_apply(&out->features);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
typedef struct { const char *name; PFN_vkVoidFunction fn; } entry;

static const entry g_ours[] = {
#define E(n) { "vk" #n, (PFN_vkVoidFunction)klvk_##n }
    E(CreateInstance), E(EnumerateInstanceExtensionProperties),
    E(CreateDevice), E(DestroyDevice),
    E(CreateAndroidSurfaceKHR), E(DestroySurfaceKHR),
    E(GetPhysicalDeviceSurfaceSupportKHR), E(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    E(GetPhysicalDeviceSurfaceFormatsKHR), E(GetPhysicalDeviceSurfacePresentModesKHR),
    E(CreateSwapchainKHR), E(DestroySwapchainKHR), E(GetSwapchainImagesKHR),
    E(AcquireNextImageKHR), E(QueuePresentKHR),
    // The memory seam. Four of these five are pure PASS-THROUGH accounting —
    // they call MoltenVK's own entry point and add to a counter — and the fifth
    // (the memory-properties pair) clamps a heap size and nothing else. Nothing
    // here copies, shadows or reallocates a guest resource; see the block that
    // defines them for why that distinction is the whole point.
    E(GetPhysicalDeviceMemoryProperties), E(GetPhysicalDeviceMemoryProperties2),
    E(AllocateMemory), E(FreeMemory), E(MapMemory), E(UnmapMemory),
    // The format seam. vkCreateImage was NOT on this list before round 12, and
    // that is exactly why launch #11's unmappable VkFormat reached MoltenVK and
    // aborted the process through Metal's validation instead of coming back as
    // an error the guest could see. The other four are here because they have
    // to AGREE with whatever vkCreateImage did: a view's format, an upload's
    // texel size, and the slot that has to be released when the image is.
    E(CreateImage), E(DestroyImage), E(CreateImageView),
    // ...and the dynamic-rendering attachment census (see its block).
    E(DestroyImageView), E(CmdBeginRendering), E(CmdEndRendering),
    // ...and the two seams that say what an image COSTS. Round 13's census
    // counted images by format; a count is not a size, and the by-bytes ledger
    // needs the driver's own answer (vkGetImageMemoryRequirements) and the type
    // the guest bound it to (vkBindImageMemory). Both are pure pass-through
    // unless KL_VK_LAZY_TRANSIENT is on.
    E(GetImageMemoryRequirements), E(GetImageMemoryRequirements2),
    E(BindImageMemory), E(BindImageMemory2),
    // The pipeline seam: MoltenVK compiles MSL inside these, synchronously, on
    // the calling thread. Counting and timing them is what turns "it stutters
    // when I first point at the menu" into a number; vkCreatePipelineCache is
    // what stops the second launch paying it again.
    E(CreateGraphicsPipelines), E(CreateComputePipelines), E(CreatePipelineCache),
    E(CmdCopyBufferToImage), E(CmdCopyBufferToImage2),
    E(GetPhysicalDeviceFormatProperties),
    E(GetPhysicalDeviceFeatures), E(GetPhysicalDeviceFeatures2),
    E(GetPhysicalDeviceImageFormatProperties),
    // ...and the render pass, which names the format of every attachment and
    // has to name the SUBSTITUTED one or the framebuffer's view will not match.
    E(CreateRenderPass), E(CreateRenderPass2),
    // The queue seam. Pure pass-through plus counters, and the ONLY reason it
    // exists: kl_vulkan_frame_done drains exactly one VkQueue, and until round
    // 13 nothing in this tree could say whether that is all of them. See the
    // queue census for the instrument's own failure modes.
    E(GetDeviceQueue), E(GetDeviceQueue2), E(QueueSubmit), E(QueueSubmit2),
#undef E
    { "vkGetDeviceQueue2KHR", (PFN_vkVoidFunction)klvk_GetDeviceQueue2 },
    { "vkCmdBeginRenderingKHR", (PFN_vkVoidFunction)klvk_CmdBeginRendering },
    { "vkCmdEndRenderingKHR",   (PFN_vkVoidFunction)klvk_CmdEndRendering },
    { "vkGetPhysicalDeviceFeatures2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceFeatures2 },
    { "vkQueueSubmit2KHR",    (PFN_vkVoidFunction)klvk_QueueSubmit2 },
    // The KHR alias of the ...2 form, which cannot go through E() because the
    // suffix differs. A guest that enabled VK_KHR_get_physical_device_properties2
    // asks by this name, and missing it would route straight to MoltenVK and
    // silently undo the clamp.
    { "vkGetPhysicalDeviceMemoryProperties2KHR",
      (PFN_vkVoidFunction)klvk_GetPhysicalDeviceMemoryProperties2 },
    // ...and the KHR alias of the copy-2 form, for the same reason: a guest
    // that enabled VK_KHR_copy_commands2 asks by this name, and missing it
    // would route the upload straight past the substitution check.
    { "vkCmdCopyBufferToImage2KHR",
      (PFN_vkVoidFunction)klvk_CmdCopyBufferToImage2 },
    // ...and the KHR alias of vkCreateRenderPass2, which is how a Vulkan 1.1
    // guest with VK_KHR_create_renderpass2 spells it.
    { "vkCreateRenderPass2KHR",
      (PFN_vkVoidFunction)klvk_CreateRenderPass2 },
    // The KHR aliases of the ...2 memory forms. A Vulkan 1.0 guest with
    // VK_KHR_get_memory_requirements2 / VK_KHR_bind_memory2 asks by these
    // names, and missing them would route the image straight past the ledger.
    { "vkGetImageMemoryRequirements2KHR",
      (PFN_vkVoidFunction)klvk_GetImageMemoryRequirements2 },
    { "vkBindImageMemory2KHR",
      (PFN_vkVoidFunction)klvk_BindImageMemory2 },
};

static PFN_vkVoidFunction ours(const char *name) {
    // The index-buffer guard is NOT in the table: it sits on three of the
    // hottest calls in the API, and KL_VK_IDX_GUARD=0 must take it out of the
    // dispatch entirely rather than leave a pass-through hop on the frame path.
    // The occlusion-query census is not in the table either, and for the same
    // reason: vkCmdBeginQuery is a per-object call on the frame path, so
    // KL_VK_QUERY_CENSUS=0 (the default) must leave no hop there at all.
    if (queryc_on()) {
        if (!strcmp(name, "vkCreateQueryPool"))
            return (PFN_vkVoidFunction)klvk_CreateQueryPool;
        if (!strcmp(name, "vkCmdBeginQuery"))
            return (PFN_vkVoidFunction)klvk_CmdBeginQuery;
        if (!strcmp(name, "vkGetQueryPoolResults"))
            return (PFN_vkVoidFunction)klvk_GetQueryPoolResults;
    }
    // Round 21's draw diff needs the per-command-buffer bound state, so when
    // it is on it claims the two bind names and vkBeginCommandBuffer even if
    // the index guard is off. vkCmdDrawIndexed stays the guard's name and
    // charges the diff itself, exactly as it charges the other two censuses.
    if (ddiff_on()) {
        if (!strcmp(name, "vkCmdBindPipeline"))
            return (PFN_vkVoidFunction)klvk_CmdBindPipeline;
        if (!strcmp(name, "vkCmdBindIndexBuffer"))
            return (PFN_vkVoidFunction)klvk_CmdBindIndexBuffer;
        if (!strcmp(name, "vkBeginCommandBuffer"))
            return (PFN_vkVoidFunction)klvk_BeginCommandBuffer;
        if (!strcmp(name, "vkCmdDrawIndexed"))
            return (PFN_vkVoidFunction)klvk_CmdDrawIndexed;
    }
    // The texel-buffer view census, out of the dispatch entirely when off —
    // vkCreateBufferView is not on the frame path for most guests, but the
    // rule in this file is that an OFF instrument costs nothing.
    if (ssc_cons_on()) {
        if (!strcmp(name, "vkUpdateDescriptorSets"))
            return (PFN_vkVoidFunction)klvk_UpdateDescriptorSets;
        if (!strcmp(name, "vkCreateDescriptorUpdateTemplate") ||
            !strcmp(name, "vkCreateDescriptorUpdateTemplateKHR"))
            return (PFN_vkVoidFunction)klvk_CreateDescriptorUpdateTemplate;
        if (!strcmp(name, "vkUpdateDescriptorSetWithTemplate") ||
            !strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR"))
            return (PFN_vkVoidFunction)klvk_UpdateDescriptorSetWithTemplate;
    }
    if (bufviewc_on()) {
        if (!strcmp(name, "vkCreateBufferView"))
            return (PFN_vkVoidFunction)klvk_CreateBufferView;
        if (!strcmp(name, "vkDestroyBufferView"))
            return (PFN_vkVoidFunction)klvk_DestroyBufferView;
    }
    if (idxguard_on()) {
        if (!strcmp(name, "vkCmdBindIndexBuffer"))
            return (PFN_vkVoidFunction)klvk_CmdBindIndexBuffer;
        if (!strcmp(name, "vkCmdDrawIndexed"))
            return (PFN_vkVoidFunction)klvk_CmdDrawIndexed;
        if (!strcmp(name, "vkBeginCommandBuffer"))
            return (PFN_vkVoidFunction)klvk_BeginCommandBuffer;
    }
    // The draw census, out of the dispatch entirely when off — same reason as
    // the two families above. vkCmdDrawIndexed is deliberately NOT here: the
    // index guard already owns that name, and two hooks on one entry point is
    // how a count gets doubled. klvk_CmdDrawIndexed charges the census itself.
    if (drawc_on()) {
        if (!strcmp(name, "vkCmdDraw"))
            return (PFN_vkVoidFunction)klvk_CmdDraw;
        if (!strcmp(name, "vkCmdDrawIndirect"))
            return (PFN_vkVoidFunction)klvk_CmdDrawIndirect;
        if (!strcmp(name, "vkCmdDrawIndexedIndirect"))
            return (PFN_vkVoidFunction)klvk_CmdDrawIndexedIndirect;
        if (!strcmp(name, "vkCmdDrawIndirectCount") ||
            !strcmp(name, "vkCmdDrawIndirectCountKHR") ||
            !strcmp(name, "vkCmdDrawIndirectCountAMD"))
            return (PFN_vkVoidFunction)klvk_CmdDrawIndirectCount;
        if (!strcmp(name, "vkCmdDrawIndexedIndirectCount") ||
            !strcmp(name, "vkCmdDrawIndexedIndirectCountKHR") ||
            !strcmp(name, "vkCmdDrawIndexedIndirectCountAMD"))
            return (PFN_vkVoidFunction)klvk_CmdDrawIndexedIndirectCount;
        if (!strcmp(name, "vkCmdExecuteCommands"))
            return (PFN_vkVoidFunction)klvk_CmdExecuteCommands;
    }
    // The hold census needs to see every vkBeginCommandBuffer (a re-recorded
    // buffer holds nothing of its last recording). klvk_BeginCommandBuffer is
    // shared with the two families above and resets their state as well.
    // The compute barrier (KL_VK_COMPUTE_BARRIER, default on) sees every
    // dispatch and every pipeline barrier; so does the rendering trace's window.
    // klvk_CmdPipelineBarrier* are shared with the hold census below.
    if (cb_fix_on() || kl_env_int("KL_VK_RENDERING_TRACE", 0) > 0) {
        static const entry cd[] = {
            { "vkCmdDispatch",            (PFN_vkVoidFunction)klvk_CmdDispatch },
            { "vkCmdDispatchIndirect",    (PFN_vkVoidFunction)klvk_CmdDispatchIndirect },
            { "vkCmdDispatchBase",        (PFN_vkVoidFunction)klvk_CmdDispatchBase },
            { "vkCmdDispatchBaseKHR",     (PFN_vkVoidFunction)klvk_CmdDispatchBase },
            { "vkCmdPipelineBarrier",     (PFN_vkVoidFunction)klvk_CmdPipelineBarrier },
            { "vkCmdPipelineBarrier2",    (PFN_vkVoidFunction)klvk_CmdPipelineBarrier2 },
            { "vkCmdPipelineBarrier2KHR", (PFN_vkVoidFunction)klvk_CmdPipelineBarrier2 },
        };
        for (size_t i = 0; i < sizeof cd / sizeof *cd; i++)
            if (!strcmp(name, cd[i].name)) return cd[i].fn;
    }
    if (hold_on()) {
        static const entry hold[] = {
            { "vkBeginCommandBuffer",     (PFN_vkVoidFunction)klvk_BeginCommandBuffer },
            { "vkCreateFramebuffer",      (PFN_vkVoidFunction)klvk_CreateFramebuffer },
            { "vkDestroyFramebuffer",     (PFN_vkVoidFunction)klvk_DestroyFramebuffer },
            { "vkCmdBeginRenderPass",     (PFN_vkVoidFunction)klvk_CmdBeginRenderPass },
            { "vkCmdBeginRenderPass2",    (PFN_vkVoidFunction)klvk_CmdBeginRenderPass2 },
            { "vkCmdBeginRenderPass2KHR", (PFN_vkVoidFunction)klvk_CmdBeginRenderPass2 },
            { "vkCmdPipelineBarrier",     (PFN_vkVoidFunction)klvk_CmdPipelineBarrier },
            { "vkCmdPipelineBarrier2",    (PFN_vkVoidFunction)klvk_CmdPipelineBarrier2 },
            { "vkCmdPipelineBarrier2KHR", (PFN_vkVoidFunction)klvk_CmdPipelineBarrier2 },
        };
        for (size_t i = 0; i < sizeof hold / sizeof *hold; i++)
            if (!strcmp(name, hold[i].name)) return hold[i].fn;
    }
    // Foveation (round G3) needs every render pass's framebuffer and begin:
    // the same four entry points the hold census uses, out of the dispatch
    // entirely when KL_FOVEATION is off.
    if (kl_fove_on()) {
        static const entry fove[] = {
            { "vkCreateFramebuffer",      (PFN_vkVoidFunction)klvk_CreateFramebuffer },
            { "vkDestroyFramebuffer",     (PFN_vkVoidFunction)klvk_DestroyFramebuffer },
            { "vkCmdBeginRenderPass",     (PFN_vkVoidFunction)klvk_CmdBeginRenderPass },
            { "vkCmdBeginRenderPass2",    (PFN_vkVoidFunction)klvk_CmdBeginRenderPass2 },
            { "vkCmdBeginRenderPass2KHR", (PFN_vkVoidFunction)klvk_CmdBeginRenderPass2 },
            { "vkCmdCopyImage",           (PFN_vkVoidFunction)klvk_CmdCopyImageF },
            { "vkCmdBlitImage",           (PFN_vkVoidFunction)klvk_CmdBlitImageF },
            { "vkCmdResolveImage",        (PFN_vkVoidFunction)klvk_CmdResolveImageF },
            { "vkCmdCopyImage2",          (PFN_vkVoidFunction)klvk_CmdCopyImage2F },
            { "vkCmdCopyImage2KHR",       (PFN_vkVoidFunction)klvk_CmdCopyImage2F },
            { "vkCmdBlitImage2",          (PFN_vkVoidFunction)klvk_CmdBlitImage2F },
            { "vkCmdBlitImage2KHR",       (PFN_vkVoidFunction)klvk_CmdBlitImage2F },
            { "vkCmdResolveImage2",       (PFN_vkVoidFunction)klvk_CmdResolveImage2F },
            { "vkCmdResolveImage2KHR",    (PFN_vkVoidFunction)klvk_CmdResolveImage2F },
        };
        for (size_t i = 0; i < sizeof fove / sizeof *fove; i++)
            if (!strcmp(name, fove[i].name)) {
                // The copy_commands2 spellings only where the driver has them
                // (ours would otherwise drop the command).
                if (strstr(name, "Image2") && !hold_real(name, NULL)) return NULL;
                return fove[i].fn;
            }
    }
    for (size_t i = 0; i < sizeof g_ours / sizeof *g_ours; i++)
        if (!strcmp(g_ours[i].name, name)) return g_ours[i].fn;
    return NULL;
}

// Round 24, upstream PR #3's rank 4: they found UE4 sizing a
// VK_KHR_fragment_shading_rate array from a *count a generic no-op stub never
// wrote. We hook nothing of that family, so the FIRST question is whether this
// guest resolves the name at all — a name it never asks for cannot be the
// source of anything. Printed once per distinct name, from whichever door it
// came through.
static void klvk_note_shading_rate_ask(const char *name, const char *door) {
    if (!name || !strstr(name, "ShadingRate")) return;
    static char seen[4][64];
    static unsigned seen_n;
    for (unsigned i = 0; i < seen_n; i++)
        if (!strcmp(seen[i], name)) return;
    if (seen_n < 4) { snprintf(seen[seen_n], sizeof seen[0], "%s", name); seen_n++; }
    VKI("the guest RESOLVED %s through %s — and this layer hooks no member of "
        "that family, so the call lands on MoltenVK's own entry point\n",
        name, door);
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetInstanceProcAddr(VkInstance inst,
                                                              const char *name);
static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev,
                                                            const char *name);

static PFN_vkVoidFunction VKAPI_CALL klvk_GetInstanceProcAddr(VkInstance inst,
                                                              const char *name) {
    vk_init();
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetDeviceProcAddr;
    klvk_note_shading_rate_ask(name, "vkGetInstanceProcAddr");
    PFN_vkVoidFunction m = ours(name);
    if (m) return m;
    if (!g_avail) return NULL;
    return real_gipa(inst ? inst : g_instance, name);
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev,
                                                            const char *name) {
    vk_init();
    if (!name) return NULL;
    // The draw census's blindness detector. Every name the guest resolves comes
    // through here, so a draw entry point we do not hook is recorded BY NAME
    // rather than silently passed to MoltenVK. This is the half of the census
    // that cannot flatter itself: the reassuring answer is an EMPTY table it
    // never writes to, not a number it accumulates.
    if (drawc_on() && !strncmp(name, "vkCmdDraw", 9) && !ours(name))
        klvk_draw_note_unhooked(name);
    klvk_note_shading_rate_ask(name, "vkGetDeviceProcAddr");
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    if (m) return m;
    if (!g_avail || !real_gdpa) return NULL;
    return real_gdpa(dev, name);
}

// ---------------------------------------------------------------------------
// The synthetic-library face
// ---------------------------------------------------------------------------
static const int g_handle_tag = 0;
#define KLVK_HANDLE ((void *)&g_handle_tag)

int kl_vulkan_claims(const char *soname) {
    if (!name_is_vulkan(soname)) return 0;
    vk_init();
    return g_avail;
}

void *kl_vulkan_dlopen(const char *soname) {
    if (!name_is_vulkan(soname)) return NULL;
    vk_init();
    if (!g_avail) {
        VKI("guest dlopen(\"%s\") refused — MoltenVK is not vendored ('make mvk')\n",
            soname);
        return NULL;
    }
    VKI("guest dlopen(\"%s\") -> synthetic Vulkan loader over MoltenVK\n", soname);
    return KLVK_HANDLE;
}

int kl_vulkan_is_handle(const void *h) { return h == KLVK_HANDLE; }

void *kl_vulkan_sym(const char *name) {
    vk_init();
    if (!name || !g_avail) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (void *)klvk_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (void *)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    if (m) return (void *)m;
    // MoltenVK exports the whole API statically, so a guest that resolves entry
    // points by dlsym rather than through vkGetInstanceProcAddr — which is legal
    // and which libSLZQuestNative does — gets a working pointer either way.
    return mvk_sym(name);
}

void *kl_vulkan_lookup(const char *name) {
    if (!name || strncmp(name, "vk", 2)) return NULL;
    return kl_vulkan_sym(name);
}

#endif /* __has_include(<vulkan/vulkan.h>) */

// ---- adrenotools_open_libvulkan (the Alyx jailbreak host, hla) -------------
//
// libadrenotools' entry: "dlopen a Vulkan loader, optionally with a custom
// Adreno/turnip driver and hooks". The jailbreak host calls it to get its
// Vulkan, with the Quest's turnip build as the custom driver. Here the driver
// IS MoltenVK, and there is exactly one loader to hand out — the synthetic
// libvulkan.so the dlopen door above already serves — so the answer is that
// handle, whatever driver was asked for; the request is logged so a run shows
// what the host wanted in its place. Every dlsym on the handle then goes
// through kl_vulkan_sym as it does for a guest that dlopened libvulkan.so by
// name. `userMappingHandle` is adrenotools' own bookkeeping for a mapped
// custom driver; none was mapped, so it is answered NULL.
//
// Outside the MoltenVK split on purpose: without MoltenVK kl_vulkan_dlopen
// refuses by name and this returns that refusal (NULL), which is exactly what
// adrenotools returns when it cannot open a loader.
void *kl_vulkan_adrenotools_open(int dlopen_mode, int feature_flags,
                                 const char *tmp_lib_dir, const char *hook_lib_dir,
                                 const char *custom_driver_dir,
                                 const char *custom_driver_name,
                                 const char *file_redirect_dir,
                                 void **user_mapping_handle) {
    fprintf(stderr, "  [hla] adrenotools_open_libvulkan(mode=%#x, features=%#x, "
                    "driver=%s%s%s, hooks=%s, redirect=%s) -> the runtime's "
                    "libvulkan.so (MoltenVK); the custom driver is not loaded\n",
            dlopen_mode, feature_flags,
            custom_driver_dir ? custom_driver_dir : "(none)",
            custom_driver_name ? "/" : "", custom_driver_name ? custom_driver_name : "",
            hook_lib_dir ? hook_lib_dir : "(none)",
            file_redirect_dir ? file_redirect_dir : "(none)");
    (void)tmp_lib_dir;
    if (user_mapping_handle) *user_mapping_handle = NULL;
    return kl_vulkan_dlopen("libvulkan.so");
}
