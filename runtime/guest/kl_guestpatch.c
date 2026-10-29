// Measured guest-image patches — the table, and the guard that makes it safe.
//
// Everything here exists because of one class of gate: a branch inside the
// guest that this host cannot answer from outside. The project's rule is to
// implement what the guest FORCES and to answer honestly; a patch is the
// admission that there is no question being asked of us at all — the decision
// is already made inside the binary, and the only way to observe what is behind
// it is to take the other edge.
//
// So the bar for a row here is high, and every row carries how it was MEASURED:
//
//   * the address is a guest vaddr in a named library, not an offset in a file;
//   * the `expect` word is the instruction that was disassembled there, which
//     doubles as a build fingerprint — a different APK almost never has the
//     same word at the same address, so a stale row is skipped and NAMED rather
//     than corrupting an image;
//   * a group is atomic: either every word matches and all are written, or none
//     are. Half of a two-instruction replacement is a crash, not a no-op.
//
// Knobs (DEBUG_ENV_VARS.md): `KL_GUEST_PATCH=0` turns all of this off, which is
// the A/B for every row; `KL_GUEST_PATCH_OFF=<name>[,<name>...]` turns off one.
//
// DEVICE: a klepton-ld dylib is signed, so its text cannot be written at run
// time — which is why the entry point takes a RESOLVER rather than a base
// pointer, so the same table can be applied offline. tools/klepton_ld.c does
// exactly that (see its `gp_at`), so a row here reaches the headset as well as
// the host, and there is no host/device divergence to remember.
#include "kl_guestpatch.h"
#include "kl_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t va; uint32_t expect, to; const char *what; } kl_gp_word;

typedef struct {
    const char       *name;      // the knob name, and what a log line calls it
    const char       *lib;       // basename of the guest library
    const char       *why;       // one line, printed when it is applied
    const kl_gp_word *words;
    unsigned          nwords;
} kl_gp_patch;

// ---------------------------------------------------------------------------
// VRChat (Steam Frame build, `Assembly-CSharp.dll` inside libil2cpp.so).
//
// Both rows were reached the same way and the route is worth keeping, because
// nothing in this target has a symbol: the metadata is decrypted with
// tools/vrc_metadata.py, an `Il2CppCodeGenModule`'s methodPointers array turns
// a method's token RID into an address, and a v29 metadata-usage slot holds
// `(kind << 29) | (index << 1) | 1` in the FILE until it is initialised — so a
// string literal decodes back to the `adrp`+`ldr` that reads it, and from there
// to the method.

// (1) The "Under Construction" wall.
//
// `VRCFlowManager`'s ManageGameFlow coroutine does, in effect:
//
//     while (VRCUiManager.Instance == null) yield return null;   // 'waiting for UI Manager'
//     yield return this.<wall>();                                // <-- here
//
// and the wall coroutine it yields is:
//
//     while (VRCUiPopupManager.Instance == null) yield return null;
//     VRCUiPopupManager.Instance.<hide>(null);
//     VRCUiManager.Instance.ShowScreen("MenuContent/Screens/Authentication/UnderConstruction");
//     for (;;) yield return null;                                // never completes
//
// The last line is what makes this a wall rather than a screen: the outer
// coroutine is waiting on an inner one that never returns false, so the whole
// game flow stops there forever. It is also why the obvious patches do NOT
// work, and both were tried: aliasing the screen-path string literal to
// `.../LoginPrompt` shows a different screen and still never advances, and
// NOPing the one conditional branch that reaches the yield leaves the other
// predecessor — the paths converge, so in this build the wall is effectively
// unconditional.
//
// What works is to make the inner coroutine COMPLETE: `MoveNext` returns false
// on its first call, `yield return` finishes immediately, and the flow carries
// on to what was behind it. The screen is never shown, and nothing else in the
// coroutine has a side effect worth keeping.
//
//   0x698d634  str x30, [sp, #-0x30]!   -> mov w0, #0   ; "the enumerator is done"
//   0x698d638  stp x22, x21, [sp, #0x10] -> ret
//
// The method was confirmed to RUN before it was patched, by writing a `brk #0`
// at its ShowScreen call and watching the process die on signal 5 — worth the
// extra run, because the two failed patches above look exactly like dead code.
static const kl_gp_word k_vrc_wall[] = {
    { 0x698d634, 0xf81d0ffe, 0x52800000, "mov w0, #0  (MoveNext -> false)" },
    { 0x698d638, 0xa90157f6, 0xd65f03c0, "ret" },
};

// (2) The minimum-client-version screen, which is what is immediately behind
// the wall: "Update Required — Current: 1862, Required: 1865".
//
// This one is a real check against a real answer — the required build number
// comes from VRChat's own API — and the honest fix is a newer APK: the tree
// here is three builds old. It is patched anyway because otherwise the target
// stops at a second dead screen and nothing downstream of it can be brought up
// at all, and because the number it compares is not a measurement this host can
// make. Turn it off with `KL_GUEST_PATCH_OFF=vrchat-min-client-version` to see
// the screen again.
//
//   0x696828c  ldp w8, w9, [x0, #0x10]  ; current, required
//   0x6968290  cmp w8, w9
//   0x6968294  b.ge 0x6968350           -> b 0x6968350   ; always "new enough"
static const kl_gp_word k_vrc_minver[] = {
    { 0x6968294, 0x540005ea, 0x1400002f, "b.ge -> b  (skip VerifyMinClientVersion)" },
};

// (3) The "Going to Error World" transition.
//
// The trigger that brought this row up: the APK genuinely does not ship
// AudioPluginOculusSpatializer, so every
// world load raises DllNotFoundException, and the managed response is:
//
//     E/Unity: Exception: Going to Error World
//
// The route to the code, since nothing here has a symbol:
// tools/vrc_code.py --literals 'Going to Error World' finds
// StringLiteral[13074]; --uses-literal names its one real reader,
// VRCFlowManager::ÌÌÎÌÍÌÌÍÌÏÍÍÌÌÌÍÎÍÏÍÏÎÌ at 0x6944074 (196 bytes). That
// method IS the error-world action, not a branch near it: it builds a
// System.Exception with that message, hands it to the shared LogException
// helper at 0x68acbd0 (the VRC.Core.Logger user — this is what prints the
// E/Unity line above), then starts the coroutine stored in [this + 0x148]
// through the generic coroutine starter at 0x6940030, and returns the
// Coroutine to its caller, which yields on it. The starter has nine other
// callers and is innocent; the literal's only other reader, a <>c lambda at
// 0x6aed654, just string.Formats the message. So the whole transition is
// exactly this one small method.
//
// --callers shows all six call sites live in two coroutines' MoveNext and
// every one is a failure path ('Failed to load default world {0} times,
// going to error world', "Couldn't validate world {0}", and their untagged
// siblings): the method is never reached from a healthy flow, so making it
// a no-op disables ONLY the error-driven transition. What the host loses is
// the Error World itself — a fallback that exists to give the user somewhere
// to stand when the real world failed, which is precisely the thrash when
// the failure is this host's missing spatializer. The DllNotFoundException
// is still logged by Unity; only the reaction is skipped.
//
// The replacement mirrors the wall row above: return before the prologue,
// with x0 = null so the caller's `yield return <coroutine>` degenerates to
// the benign `yield return null` (verified in the caller at 0x69701c0: the
// returned pair is stored, never dereferenced, before MoveNext continues).
//
//   0x6944074  sub sp, sp, #0x30        -> mov x0, xzr  ; "no transition"
//   0x6944078  stp x30, x21, [sp, #0x10] -> ret
static const kl_gp_word k_vrc_errorworld[] = {
    { 0x6944074, 0xd100c3ff, 0xaa1f03e0, "mov x0, xzr  (return null coroutine)" },
    { 0x6944078, 0xa90157fe, 0xd65f03c0, "ret" },
};

// (4) The stereo rendering mode, in libUnityOpenXR.so rather than in the
// managed image — the one row here that is about pixels.
//
// This build asks for a swapchain with `arraySize 2`, which is the Single Pass
// Instanced layout, and single-pass instanced on GLES is `GL_OVR_multiview2`.
// ANGLE's Metal backend has no multiview at all (nothing in
// src/libANGLE/renderer/metal mentions it), so we advertise none, libunity
// resolves no multiview entry point, and its fall-back attaches ONE array
// layer — layer 0 — and never re-attaches. The right eye is therefore never
// drawn: measured as tex 29 layer 0 at 3,493,465 lit and layer 1 at 0 lit, in
// every frame of every run, with no GL error anywhere.
//
// The mode is not a question asked of this host. Unity's OpenXR C# pushes it
// down once, from the serialized `OpenXRSettings.renderMode`, through
// `NativeConfig_SetRenderMode(int)` — which is the whole of the decision:
//
//   0x76e08  stp x19, x30, [sp, #-0x10]!
//   0x76e0c  mov w19, w0                 -> mov w19, #0     ; MultiPass
//   0x76e10  bl  <singleton>
//   0x76e14  cbz x0, +0x18
//   0x76e18  bl  <singleton>
//   0x76e1c  str w19, [x0, #0x70]        ; the field every consumer reads
//
// `MultiPass = 0, SinglePassInstanced = 1` is the package's own enum, and the
// binary agrees: the consumer at +0x55120 tests the field against exactly 0 and
// 1 and treats anything else as neither.
//
// MultiPass is the mode that needs no extension: two passes, one array slice
// each, the plain (non-stereo) shader variant that every build ships. It costs
// twice the draw calls, which is the honest price of a display without
// multiview. `KL_GUEST_PATCH_OFF=vrchat-multipass` restores the failing
// configuration exactly.
static const kl_gp_word k_vrc_multipass[] = {
    { 0x76e0c, 0x2a0003f3, 0x52800013, "mov w19, w0 -> mov w19, #0  (MultiPass)" },
};


// (5) Unity's cross-context guard on every framebuffer bind.
//
// Framebuffer objects are container objects and are NOT shared between GL
// contexts, so Unity records the EGLContext each binding was made under and
// re-checks it before every bind. On a mismatch it binds `(GLuint)-1` rather
// than a name it believes is invalid here. The idiom is inlined at 35 sites,
// always the same three instructions, always calling GL table slot 1240:
//
//     cmp   x8, x20                 ; recorded context vs the live one
//     ccmp  wN, #0, #4, ne          ; ...or the name is 0
//     csinv w1, wN, wzr, eq         ; mismatch -> ~0
//
// `(GLuint)-1` is not a sentinel ANGLE invents and not a refused bind: the
// guest passes it, ANGLE accepts it, and the result is a framebuffer with no
// attachments — so the clear and every draw of that pass raise
// GL_INVALID_FRAMEBUFFER_OPERATION and go nowhere. Measured on VRChat: 322 such
// errors in a 1500-frame run, 0 with this row applied.
//
// Neutralising the guard is truthful about what this runtime implements, and
// that is the whole justification: kl_glfb's default migration mode backs EVERY
// guest EGLContext with ONE real ANGLE context (see kl_glfb_make_current), so
// the framebuffer really is valid whichever guest context is nominally current.
// The distinction Unity is guarding against does not exist here.
//
// Three things about this row that must not be forgotten:
//
//   * WHAT TRIGGERS THE MISMATCH IS NOT EXPLAINED. Every eglGetCurrentContext
//     in the run answers the same context, and every framebuffer is created
//     under it, so the recorded value must be captured somewhere this
//     instrumentation did not see. Beat Saber never binds -1 at all; AVPro's
//     third context was suspected and MEASURED INNOCENT (refusing it leaves two
//     contexts and the -1 binds remain).
//   * IT DOES NOT FIX THE WORLD LOAD. VRChat still fails to instantiate a
//     scene with this applied — that is a separate cause, and the adjacency of
//     the two in the log is a coincidence.
//   * On device it is applied OFFLINE by tools/klepton_ld.c, like every row
//     here, so the host and the headset agree.
static const kl_gp_word k_vrc_fbo_ctx[] = {
    { 0x1432f04, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x1458650, 0x5a9f02a1, 0x2a1503e1, "w1 = w21 unconditionally" },
    { 0x145888c, 0x5a9f03a1, 0x2a1d03e1, "w1 = w29 unconditionally" },
    { 0x145899c, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x1458a54, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x1458ac4, 0x5a9f0321, 0x2a1903e1, "w1 = w25 unconditionally" },
    { 0x1458b34, 0x5a9f0321, 0x2a1903e1, "w1 = w25 unconditionally" },
    { 0x1458bdc, 0x5a9f0281, 0x2a1403e1, "w1 = w20 unconditionally" },
    { 0x1458c70, 0x5a9f02a1, 0x2a1503e1, "w1 = w21 unconditionally" },
    { 0x14590a4, 0x5a9f0141, 0x2a0a03e1, "w1 = w10 unconditionally" },
    { 0x1459240, 0x5a9f0301, 0x2a1803e1, "w1 = w24 unconditionally" },
    { 0x145beb8, 0x5a9f0101, 0x2a0803e1, "w1 = w8 unconditionally" },
    { 0x14688dc, 0x5a9f0141, 0x2a0a03e1, "w1 = w10 unconditionally" },
    { 0x1468cdc, 0x5a9f0121, 0x2a0903e1, "w1 = w9 unconditionally" },
    { 0x1469ff8, 0x5a9f0121, 0x2a0903e1, "w1 = w9 unconditionally" },
    { 0x146a2e0, 0x5a9f0321, 0x2a1903e1, "w1 = w25 unconditionally" },
    { 0x146aff0, 0x5a9f0121, 0x2a0903e1, "w1 = w9 unconditionally" },
    { 0x146b3bc, 0x5a9f0121, 0x2a0903e1, "w1 = w9 unconditionally" },
    { 0x146bc68, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146c10c, 0x5a9f0341, 0x2a1a03e1, "w1 = w26 unconditionally" },
    { 0x146c190, 0x5a9f0341, 0x2a1a03e1, "w1 = w26 unconditionally" },
    { 0x146c24c, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146c2b0, 0x5a9f02e1, 0x2a1703e1, "w1 = w23 unconditionally" },
    { 0x146c328, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146c51c, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146c84c, 0x5a9f02c1, 0x2a1603e1, "w1 = w22 unconditionally" },
    { 0x146c94c, 0x5a9f02c1, 0x2a1603e1, "w1 = w22 unconditionally" },
    { 0x146ca9c, 0x5a9f0161, 0x2a0b03e1, "w1 = w11 unconditionally" },
    { 0x146cc24, 0x5a9f02c1, 0x2a1603e1, "w1 = w22 unconditionally" },
    { 0x146ccd8, 0x5a9f0301, 0x2a1803e1, "w1 = w24 unconditionally" },
    { 0x146cd4c, 0x5a9f0381, 0x2a1c03e1, "w1 = w28 unconditionally" },
    { 0x146d260, 0x5a9f02e1, 0x2a1703e1, "w1 = w23 unconditionally" },
    { 0x146d61c, 0x5a9f02e1, 0x2a1703e1, "w1 = w23 unconditionally" },
    { 0x146d6bc, 0x5a9f0301, 0x2a1803e1, "w1 = w24 unconditionally" },
    { 0x146d7ec, 0x5a9f02c1, 0x2a1603e1, "w1 = w22 unconditionally" },
};


// (6) The AudioPluginOculusSpatializer per-frame exception storm.
//
// The Steam Frame APK ships the Oculus spatializer's MANAGED code but not its
// native module, so every P/Invoke into it throws DllNotFoundException — on
// real hardware too, not just here. Measured: ~1 throw per frame, every frame
// (587-609 in 600-frame headless runs), and each one costs a full IL2CPP
// exception raise (our dlopen miss is cached; the throw is not). The call
// site evaded four static hunts because the
// module name lives only in P/Invoke metadata and the resolver is reached
// indirectly.
//
// The route that worked: `vrc_code.py --type '^OculusSpatializerUnity$'`
// lists the component's 27 methods; the small ones (124-136 bytes, no
// metadata usages) are IL2CPP's P/Invoke thunks, and each embeds its
// resolver data in plain sight — an adrp+add pair naming the module string
// ('AudioPluginOculusSpatializer', 28 chars) and another naming the native
// entry point. Reading those strings out of the binary attributes every
// thunk: OSP_Unity_AssignRaycastCallback (two variants), SetDynamicRoom*,
// UpdateRoomModel, GetRoomDimensions, GetRaycastHits.
//
// The per-frame caller is OculusSpatializerUnity::Update (0x67a0c80). Its
// prologue reads a bool at [this+0x38]: if set it builds a delegate and
// calls the marshaling AssignRaycastCallback variant (0x679dab4) at
// 0x67a0d88; either way the paths merge at 0x67a0e50 and call the plain
// AssignRaycastCallback thunk (0x679f1e0) at 0x67a0e68, followed by the
// SetDynamicRoom* chain. The FIRST of these to run throws, and exactly one
// throw per frame says the exception unwinds clean out of Update — so on
// real hardware everything in Update after that call is dead code, every
// frame, forever.
//
// The patch therefore does by branch what hardware does by exception: at
// each of the two throwing call sites, jump to Update's own epilogue
// (0x67a13e8, the register-restore ladder ending in ret at 0x67a1414). No
// managed side effect is skipped that hardware would have performed — on
// hardware the throw unwinds past the same instructions. The spatializer's
// dynamic-room feature is off either way; what changes is only the cost and
// the log. One-time throws from Start (same thunks, other call sites) are
// left alone: they are once, not per frame.
//
//   0x67a0d88  bl 0x679dab4  -> b 0x67a13e8   ; [this+0x38] path
//   0x67a0e68  bl 0x679f1e0  -> b 0x67a13e8   ; merged per-frame path
//
// Measured: objdump + a raw PT_LOAD read agree on both expect words; the
// epilogue at 0x67a13e8 begins `ldp x20, x19, [sp, #0xa0]` (0xa94a4ff4),
// matching the prologue's stores at 0x67a0ca0.
static const kl_gp_word k_vrc_ospstorm[] = {
    { 0x67a0d88, 0x97fff34b, 0x14000198, "bl AssignRaycastCallback(marshaled) -> b Update.epilogue" },
    { 0x67a0e68, 0x97fff8de, 0x14000160, "bl AssignRaycastCallback -> b Update.epilogue" },
};

// ---------------------------------------------------------------------------
// superhot-menu-scale — SUPERHOT VR's pause menu stops doubling the render size
//
// The game's own code, read out of the APK (libil2cpp.so, Assembly-CSharp):
// PauseMenuSystem::Update -> SetActive -> DoSetActive's MoveNext (0xdd45fc),
// whose opening arm is
//
//   dd4774  bl   0xdd3420           // PauseMenuSystem::get_BaseResolution
//   dd4778  fadd s0, s0, s0         // <-- the doubling, and only this
//
// while the closing arm sets the base resolution undoubled. Nop the fadd and
// the menu press stops reconfiguring the eye textures at all, which is what
// lets the eye cap stay at 4032 through the transition (the phase-14 ladder
// was measuring this doubling, not the steady state).
//
// Measured against superhot/lib/arm64-v8a/libil2cpp.so from the QA APK
// (51,398,456 bytes, Unity 2018.4.26f1); the expect word is the guard — any
// other build's libil2cpp is skipped and named, not patched. SUPERHOT's is
// the only libil2cpp in this tree with fadd s0,s0,s0 at 0xdd4778, so the row
// is inert for every other target.
static const kl_gp_word k_sh_menuscale[] = {
    { 0xdd4778, 0x1e202800, 0xd503201f, "fadd s0, s0, s0 -> nop  (menu opens at gameplay resolution)" },
};

// ---------------------------------------------------------------------------
// Half-Life 2 VR (HL2VR-Standalone-0.92, libsourcevr.so). The eye buffer is
// sized as `vr_eye_scale` percent of a hardcoded Quest 3 2064x2208, read from
// SharedPreferences "mod" — and then WHITELISTED: 50, 75, 100 or 114, anything
// else becomes 100 (launch #14, 2026-08-26: mod.xml said 200, the log said
// `100% of Quest 3 native`). The runtime max it prints is 10240x8214, so the
// clamp is the launcher's idea of a Quest's budget, not the runtime's. On this
// device the game peaks at 2.2 GB of 8.2 and is smooth at 100%, so take the
// value as written:
//
//   0xecc8  mov w8, #50
//   0xeccc  cmp w0, #75
//   0xecd8  ccmp w0, w8, #4, ne          ; == 50 ?
//   0xece0  ccmp w0, w23, #4, ne         ; == 100 ?
//   0xecec  ccmp w0, w8(=114), #4, ne    ; == 114 ?
//   0xecf0  csel w8, w0, w23, eq         -> mov w8, w0   ; any percent
//   0xecf8  str w8, [x19, #0x1e4]
//
// KL_SRCENG_EYE_SCALE (kl_sdl.c) picks the number; this row lets it through.
static const kl_gp_word k_hl2vr_eyescale[] = {
    { 0xecf0, 0x1a970008, 0x2a0003e8, "csel w8, w0, w23, eq -> mov w8, w0 (vr_eye_scale unclamped)" },
};

// ---------------------------------------------------------------------------
// hl2vr-togl-maxtexture — the eye render target stops being halved to 2064x2208
//
// Round 14 unclamped `vr_eye_scale` and launch #15 sized everything downstream
// of libsourcevr correctly: `Eye resolution: 200% of Quest 3 native 2064x2208
// -> 4128x4416`, `[xr] swapchain 4128x4416` twice, `Source render targets
// retained: ... size=4128x4416`. And Austin saw the game as a small doubled
// SQUARE in the bottom-left of each eye, because the picture inside that
// swapchain was still 2064x2208. The log says so in libsourcevr's own words:
//
//   Source pixels sample 0: fbo=42 ... viewport=0,0 2064x2208
//   Eye 0 ... source target bound=1 texture=114 ... 2064x2208 active=2064x2208
//
// libsourcevr creates its eye colour/depth targets itself, at 0xcfd0/0xd00c,
// with RenderTargetSizeMode RT_SIZE_LITERAL (8) and w/h read from its own
// fields 0x1d4/0x1d8 — the 4128x4416 it just computed. So the request was
// right and the ANSWER was halved, which is Source's material system doing
// what it always does with a texture bigger than the device can hold:
//
//   while (w > maxTextureWidth || h > maxTextureHeight) { w >>= 1; h >>= 1; }
//
// and togl reports the device's limit as a hardcoded 4096. 4128 is 32 pixels
// over it, so one halving takes 4128x4416 to exactly 2064x2208 — the number in
// the log, to the pixel, on both axes.
//
// The limit is not a computed value and not a GL query: it is three DWORDs of
// .rodata that IDirect3D9::GetDeviceCaps copies straight into D3DCAPS9 at
// libtogl.so 0x35e00 (`ldr q0, [x8, #0xe00]` at 0x79db4, the only reference to
// the blob in the image):
//
//   0x35e00  0x04000400                 ; the field before MaxTextureWidth
//   0x35e04  0x00001000  = 4096         ; MaxTextureWidth   -> 8192
//   0x35e08  0x00001000  = 4096         ; MaxTextureHeight  -> 8192
//   0x35e0c  0x00000400  = 1024         ; MaxVolumeExtent, left alone
//
// (The first three DWORDs of the struct are 1, 0, 0x20000000 = D3DDEVTYPE_HAL,
// adapter 0, D3DCAPS2_DYNAMICTEXTURES, which is how the layout is anchored.)
// 8192 is the smallest power of two that admits 4128x4416, and it is well
// inside what ANGLE-Metal actually allows (16384). Two rows are the change;
// the third and fourth are fingerprint only — they rewrite the words they
// already hold, so a togl built with different caps is skipped by name rather
// than corrupted.
//
// The cost to watch: the cap is global, so any ART texture over 4096 that was
// being halved now uploads whole. HL2's own art is 2048 or less and the remix
// pack is 4096, so nothing should move; `[hb] peak` in launch #16 is the check.
// `KL_GUEST_PATCH_OFF=hl2vr-togl-maxtexture` is the A/B.
static const kl_gp_word k_hl2vr_maxtex[] = {
    { 0x35e00, 0x04000400, 0x04000400, "D3DCAPS9 field before MaxTextureWidth (fingerprint only)" },
    { 0x35e04, 0x00001000, 0x00002000, "D3DCAPS9.MaxTextureWidth  4096 -> 8192" },
    { 0x35e08, 0x00001000, 0x00002000, "D3DCAPS9.MaxTextureHeight 4096 -> 8192" },
    { 0x35e0c, 0x00000400, 0x00000400, "D3DCAPS9.MaxVolumeExtent 1024 (fingerprint only)" },
};

static const kl_gp_patch k_patches[] = {
    { "hl2vr-togl-maxtexture", "libtogl.so",
      "the eye render target keeps the size it was created with instead of being halved past togl's 4096 cap",
      k_hl2vr_maxtex, sizeof k_hl2vr_maxtex / sizeof *k_hl2vr_maxtex },
    { "hl2vr-eye-scale", "libsourcevr.so",
      "vr_eye_scale is taken as written instead of whitelisted to 50/75/100/114",
      k_hl2vr_eyescale, sizeof k_hl2vr_eyescale / sizeof *k_hl2vr_eyescale },
    { "superhot-menu-scale", "libil2cpp.so",
      "the pause menu stops doubling XRSettings.eyeTextureResolutionScale",
      k_sh_menuscale, sizeof k_sh_menuscale / sizeof *k_sh_menuscale },
    { "vrchat-multipass", "libUnityOpenXR.so",
      "the stereo rendering mode is MultiPass, because there is no multiview",
      k_vrc_multipass, sizeof k_vrc_multipass / sizeof *k_vrc_multipass },
    { "vrchat-under-construction", "libil2cpp.so",
      "the VRCFlowManager wall coroutine completes instead of hanging",
      k_vrc_wall, sizeof k_vrc_wall / sizeof *k_vrc_wall },
    { "vrchat-fbo-context-guard", "libunity.so",
      "EXPERIMENT: framebuffer binds ignore Unity's cross-context guard",
      k_vrc_fbo_ctx, sizeof k_vrc_fbo_ctx / sizeof *k_vrc_fbo_ctx },
    { "vrchat-min-client-version", "libil2cpp.so",
      "the client build number always compares new enough",
      k_vrc_minver, sizeof k_vrc_minver / sizeof *k_vrc_minver },
    { "vrchat-error-world", "libil2cpp.so",
      "the exception-driven Error World transition does not start",
      k_vrc_errorworld, sizeof k_vrc_errorworld / sizeof *k_vrc_errorworld },
    { "vrchat-spatializer-storm", "libil2cpp.so",
      "the absent Oculus spatializer is skipped, not thrown on, per frame",
      k_vrc_ospstorm, sizeof k_vrc_ospstorm / sizeof *k_vrc_ospstorm },
};
#define KL_GP_N (sizeof k_patches / sizeof k_patches[0])

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Is `name` listed in KL_GUEST_PATCH_OFF? Compared whole, against a comma or
// space separated list, so one name is never a prefix of another's answer.
static int turned_off(const char *name) {
    const char *list = getenv("KL_GUEST_PATCH_OFF");
    if (!list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == n && strncmp(p, name, n) == 0) return 1;
        p = e;
    }
    return 0;
}

unsigned kl_guest_patch_apply(const char *path, kl_gp_at at, void *ctx) {
    if (!kl_env_on("KL_GUEST_PATCH", 1)) return 0;
    const char *base = basename_of(path);
    unsigned applied = 0;

    for (size_t i = 0; i < KL_GP_N; i++) {
        const kl_gp_patch *p = &k_patches[i];
        if (strcmp(base, p->lib) != 0) continue;
        if (turned_off(p->name)) {
            fprintf(stderr, "  [patch] %s: OFF (KL_GUEST_PATCH_OFF)\n", p->name);
            continue;
        }

        // Pass 1 — every word must be where it was measured. The two ways that
        // fails are NOT the same thing and are reported differently, which the
        // first run of this table got wrong: every guest with a `libil2cpp.so`
        // matches the basename, so Beat Saber's `make check` printed two lines
        // about VRChat's rows. An address past the end of the image says this
        // is a different LIBRARY that happens to share a name — no information,
        // so it is silent unless asked. A word that is present and different
        // says this is the right library and the wrong BUILD, which is worth a
        // line: it is the difference between "not for this APK" and "wrong".
        unsigned ok = 1;
        for (unsigned k = 0; k < p->nwords && ok; k++) {
            uint32_t *w = at(ctx, p->words[k].va);
            if (!w) {
                if (kl_env_on("KL_TRACE_PATCH", 0))
                    fprintf(stderr, "  [patch] %s: not this image — 0x%llx is "
                                    "past the end of %s\n",
                            p->name, (unsigned long long)p->words[k].va, base);
                ok = 0;
            } else if (*w != p->words[k].expect) {
                fprintf(stderr, "  [patch] %s: SKIPPED — 0x%llx holds 0x%08x, "
                                "measured 0x%08x. This is not the build it was "
                                "measured against; the image is untouched.\n",
                        p->name, (unsigned long long)p->words[k].va,
                        *w, p->words[k].expect);
                ok = 0;
            }
        }
        if (!ok) continue;

        // Pass 2 — write, now that the whole group is known to fit.
        for (unsigned k = 0; k < p->nwords; k++)
            *at(ctx, p->words[k].va) = p->words[k].to;
        applied++;
        fprintf(stderr, "  [patch] %s: applied %u word(s) to %s — %s\n",
                p->name, p->nwords, base, p->why);
        if (kl_env_on("KL_TRACE_PATCH", 0))
            for (unsigned k = 0; k < p->nwords; k++)
                fprintf(stderr, "          0x%09llx 0x%08x -> 0x%08x  %s\n",
                        (unsigned long long)p->words[k].va, p->words[k].expect,
                        p->words[k].to, p->words[k].what);
    }
    return applied;
}
