//
//  KleptonCompositor.swift — the Compositor Services half of the eye seam.
//
//  What this file owns:
//
//    1. The eye textures. The guest asks OVRPlugin for eye storage
//       (ovrp_SetupEyeTexture2), kl_ovrp forwards that to kl_glfb, and
//       kl_glfb asks the registered provider for an MTLTexture. That provider
//       is `provideEyeTexture` below. The textures are OURS and outlive the
//       frame — see "why not zero-copy" below.
//    2. The frame clock. On device the deadline belongs to Compositor
//       Services, so this loop calls kl_app_frame() once per drawable rather
//       than letting the guest own a pump loop. That is why CADisplayLink was
//       never added as an interim pacer: it would be a third mechanism to
//       then remove.
//    3. The composite pass — which is also the reprojection pass. See
//       runtime/gfx/kl_reproject.h: the guest's picture is placed as a quad at a
//       fixed far distance, sized by the frustum it was *rendered* with, in the
//       space the pose it was rendered with defines, and then looked at from
//       where the head is now through the projection this drawable wants. That
//       corrects two things at once — the stale pose (timewarp) and the fact
//       that the guest's Quest-shaped frustum is not the Vision Pro's.
//
//  **Why not zero-copy.** Handing the guest a cp_drawable's own texture would
//  avoid a copy, but drawables are per-frame while eye textures are created
//  once per (eye, stage) and reused. Timewarp needs the latter: reprojection
//  resamples an already-rendered eye image against a newer pose, so the image
//  has to still exist. Zero-copy and timewarp are mutually exclusive and
//  timewarp wins — the resampling pass reprojection needs *is* this composite
//  pass, so the copy is not actually extra work.
//
//  **Two reprojections happen, and they are not the same one.** This pass moves
//  the guest's picture from the pose it was rendered with to the pose we
//  predict for presentation. Compositor Services then does its own correction
//  from that predicted pose to what the hardware actually reports at scanout —
//  which is what `drawable.deviceAnchor` is for, and why the anchor set below
//  is the *display* one and not the render one. Setting the render pose there
//  instead would apply our delta twice. ALVR's visionOS client splits it the
//  same way (`ALVRClient/Renderer.swift`).
//
//  **The flip is not cosmetic.** GL renders bottom-up; Metal and the display
//  are top-down. The first host dump of these textures came out upside down
//. It is done in the shared shader, because the composite is the only
//  pass that reads the eye textures.
//
import SwiftUI
import CompositorServices
import Metal
import simd
import ARKit
import QuartzCore

// What the drawable costs, and therefore how much of the display we can afford
// to actually use. Three settings here decide that, and they are entangled:
//
//   * **8-bit sRGB colour, not RGBA16F.** The eye textures the guest renders
//     into stay RGBA16F — that is where Unity's HDR range lives and nothing
//     about it changes. What changes is the *drawable* this pass resolves into,
//     which is the last step before the display and holds a tone-mapped picture
//     with no headroom left to preserve. Halving its bytes per pixel halves the
//     bandwidth of every composite, and `bgra8Unorm_srgb` encodes on write, so
//     the linear values the shader produces are converted by the hardware
//     rather than thrown away. This is ALVR's choice on the same headset.
//     KL_CP_FORMAT=16 is the A/B back to rgba16Float.
//   * **Foveation.** The drawable is then rasterized through a variable rate
//     map: dense where the eye looks, sparse at the periphery. The composite
//     pass MUST set that map on its render pass — a pass that ignores it fills
//     the physical texture as if it were uniform, and the system's unwarp then
//     shows a distorted picture. That is why this was off until now, and it is
//     the reason for the amplified single-pass composite below.
//   * **maxRenderQuality = 1.0.** With foveation on, this is what buys the
//     resolution: ALVR measures the drawable going from 1888x1792 physical /
//     4338x3478 screen at the default to 2496x2432 / 6262x5020 at 1.0. It
//     costs memory (it is billed to the app) and per-frame GPU time, which is
//     why the ceiling and the *running* quality are two separate settings —
//     see `applyRenderQuality`.
//
// KL_CP_FOVEATION=0 turns the last two off together, because the quality knob
// is only accepted alongside foveation.
struct KleptonStageConfiguration: CompositorLayerConfiguration {
    /// The render quality to ask for, 0..1. Also read by the render loop, which
    /// has to set the *running* quality separately.
    static var requestedQuality: Float {
        let e = ProcessInfo.processInfo.environment["KL_CP_QUALITY"]
        let v = e.flatMap { Float($0) } ?? 1.0
        return min(max(v, 0.0), 1.0)
    }
    static var wantFoveation: Bool { klEnvOn("KL_CP_FOVEATION", default: true) }

    /// Whether `maxRenderQuality` was actually configured on the layer.
    ///
    /// Not the same question as `wantFoveation`, and the difference is a crash.
    /// The ceiling is only set when foveation is genuinely available; the
    /// *running* quality is set later by `applyRenderQuality`, and asking for a
    /// quality above a ceiling that was never configured is a configuration
    /// error, which Compositor Services reports as `__BUG_IN_CLIENT__` inside
    /// `cp_layer_renderer_set_render_quality` — an abort with no mention of
    /// either setting. The visionOS simulator is where the two diverge: it
    /// supports no foveation at all and reports its default render quality as
    /// **NaN**, so every immersive run there died on the second setting.
    nonisolated(unsafe) static var ceilingConfigured = false

    /// Foveation is only honourable with vertex amplification, so it is only
    /// asked for when the GPU has it.
    ///
    /// A foveated drawable's rate map has one layer per eye and Metal picks the
    /// layer by render-target array index, which only an amplified single pass
    /// can address (see `encodeComposite`). Without amplification the composite
    /// would have to ignore the map, and a pass that ignores the map does not
    /// render *slower*, it renders a **distorted picture** — the system unwarps
    /// something that was never warped. So the honest configuration on such a
    /// device is no foveation at all, decided here rather than discovered three
    /// layers down. Every Apple silicon GPU reports 2; this is a guard, not an
    /// expected path.
    static var canAmplify: Bool {
        MTLCreateSystemDefaultDevice()?.supportsVertexAmplificationCount(2) ?? false
    }

    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        // (supportedColorFormats(options:) is visionOS 26+ and deprecated in
        // the same release, so it is not asked here — the drawable reports the
        // format it actually got instead, in the one-time log below.)
        let want: MTLPixelFormat =
            ProcessInfo.processInfo.environment["KL_CP_FORMAT"] == "16"
            ? .rgba16Float : .bgra8Unorm_srgb
        configuration.colorFormat = want
        configuration.depthFormat = .depth32Float

        // KL_CP_AMPLIFY=0 takes foveation down with it: without an amplified
        // single pass the rate map cannot be honoured (see encodeComposite),
        // so one knob restores the entire pre-session composite rather than
        // half of it.
        let amplify = Self.canAmplify && klEnvOn("KL_CP_AMPLIFY", default: true)
        let foveate = capabilities.supportsFoveation && Self.wantFoveation && amplify
        configuration.isFoveationEnabled = foveate
        if !amplify && capabilities.supportsFoveation && Self.wantFoveation {
            NSLog("[cp] foveation refused: this GPU has no vertex amplification, "
                  + "so the composite could not honour the rate map")
        }
        let quality = Self.requestedQuality
        if foveate {
            // Gated on foveation deliberately, as ALVR gates it: an
            // unsupported render quality is a *configuration error*, which
            // fails the whole layer rather than degrading, and there is no
            // point paying for resolution the rate map is not there to spend.
            configuration.maxRenderQuality = .init(quality)
        }
        Self.ceilingConfigured = foveate
        NSLog("[cp] colour format \(want.rawValue), foveation \(foveate) "
              + "(supported \(capabilities.supportsFoveation)), "
              + "max render quality \(foveate ? String(quality) : "default") "
              + "(device default \(capabilities.defaultRenderQuality))")

        // The layout query has to be told about foveation too — the supported
        // layouts are not the same set with a rate map in play.
        let layouts = capabilities.supportedLayouts(options: foveate ? [.foveationEnabled] : [])
        configuration.layout = layouts.contains(.layered) ? .layered : .dedicated
    }
}

// Compositor Services hands out `Duration`s from its own clock epoch, and
// ARKit wants a TimeInterval. There is no bridge in the SDK, so this is it.
extension Duration {
    var timeInterval: TimeInterval {
        let c = components
        return TimeInterval(c.seconds) + TimeInterval(c.attoseconds) * 1e-18
    }
}

// One (eye, stage) allocation. `slice` is which slice of `texture` the eye
// owns: both eyes share one 2-slice array, which is what the guest's own
// texture-array path expects and what `make mtltex` [Q3] proved works.
private struct EyeAllocation {
    var texture: MTLTexture
    var slice: Int
    // 1 if the guest drew this picture with its origin at the TOP left — i.e.
    // with Vulkan rather than GL. Recorded per texture rather than decided once
    // (kl_reproject.h): getting it wrong is a correct-looking picture that is
    // upside down, and nothing in Metal, Compositor Services or the guest can
    // report it.
    var flipY: Bool = false
}

/// GL internal format -> MTLPixelFormat, for the eye textures the guest asks
/// for storage for.
///
/// **This is a requirement, not a preference.** ANGLE compares the GL internal
/// format we claim against the MTLTexture's own pixel format and refuses the
/// pair — `TextureImageSiblingMtl::ValidateClientBuffer`, "Incompatible format"
/// — so a provider hardwired to RGBA16F cannot back a guest that asked for
/// anything else. Unity asks for RGBA16F through OVRPlugin; Steam Link's OpenXR
/// swapchains ask for SRGB8_ALPHA8, which is what put this here. `nil` means
/// decline, and the guest then gets ordinary GL storage of the right format —
/// black in the headset, which is visible, rather than colour that is subtly
/// wrong, which is not.
private func klEyePixelFormat(_ glInternal: UInt32) -> MTLPixelFormat? {
    switch glInternal {
    case 0x881A: return .rgba16Float        // GL_RGBA16F
    case 0x8C43: return .rgba8Unorm_srgb    // GL_SRGB8_ALPHA8
    case 0x8058: return .rgba8Unorm         // GL_RGBA8
    default:     return nil
    }
}

final class KleptonCompositor {
    private let layerRenderer: LayerRenderer
    private var device: MTLDevice!
    // Optional, not implicitly unwrapped, and that is the whole fix for one
    // crash: when ANGLE has no MTLDevice this used to stay nil while the loop
    // carried on, and primeDisplay trapped on it before the guest thread was
    // even started — so the immersive path could not run at all without
    // KL_GLFB, which is exactly the configuration you want when the question
    // is about the lifecycle rather than the picture.
    private var queue: MTLCommandQueue?
    private var pipeline: MTLRenderPipelineState!
    private var depthState: MTLDepthStencilState?
    /// Always-pass, no write — for coplanar draws after the first. See where
    /// `depthState` is built for why the two cannot be one.
    private var depthPassState: MTLDepthStencilState?
    private var sampler: MTLSamplerState!
    /// Views this GPU can draw in one amplified pass. 2 everywhere real; 1
    /// forces the old per-view passes, and forces foveation off with them
    /// (KleptonStageConfiguration.canAmplify).
    private var amplification = 1

    // KL_CP_PROBE — the bisection ladder for a black immersive space. The pass
    // has many links and a black frame indicts all of them equally; each rung
    // removes one and keeps the rest, so whichever rung first shows something
    // names the link below it.
    //
    //   1  clear only, solid blue, no draw   does ANY drawable reach the display?
    //   2  the quad, flat magenta            does the reprojected quad land in
    //                                        the viewport at all? (geometry,
    //                                        poses, projection, 500 m depth)
    //   3  the quad, sampled, alpha := 1     is the picture arriving alpha-0?
    //   4  full-viewport blit, alpha := 1    is the eye texture lit? bypasses
    //                                        the quad, the poses and the
    //                                        projection — the viewer's own
    //                                        proven path, on device
    //
    // Read them in order: the FIRST rung that shows something is the answer,
    // and every rung above it is then known-good.
    private let probe = Int(ProcessInfo.processInfo.environment["KL_CP_PROBE"] ?? "") ?? 0
    /// The overlay pass — a guest's non-eye layers. nil when it failed to
    /// build, which is named at construction rather than per frame.
    private var overlayPipeline: MTLRenderPipelineState?
    private var probePipeline: MTLRenderPipelineState?
    /// Red, green, blue — one second each, off the wall clock. Deliberately
    /// full-intensity primaries: whatever tone mapping or colour management sits
    /// between the drawable and the display, these stay obviously themselves.
    private static func cyclingClear() -> MTLClearColor {
        switch Int(Date().timeIntervalSince1970) % 3 {
        case 0:  return MTLClearColorMake(1, 0, 0, 1)
        case 1:  return MTLClearColorMake(0, 1, 0, 1)
        default: return MTLClearColorMake(0, 0, 1, 1)
        }
    }

    private static func probeName(_ p: Int) -> String {
        switch p {
        case 1: return "clear only, solid blue, no draw"
        case 2: return "quad with flat magenta — geometry only"
        case 3: return "quad, sampled, alpha forced to 1"
        case 4: return "full-viewport blit of the eye texture, alpha forced to 1"
        default: return "unknown"
        }
    }

    // GPU ordering, not CPU. The guest's frame is in ANGLE's Metal command
    // queue and this pass is in ours, and Metal orders nothing across queues:
    // without an explicit wait the composite samples a texture ANGLE may still
    // be rendering into. That reads as intermittent tearing and is exactly the
    // kind of bug that gets blamed on the guest. kl_glfb signals this event at
    // each eglSwapBuffers (EGL_ANGLE_metal_shared_event_sync); the value is
    // also how this loop tells a fresh guest frame from a reused one.
    private var guestFrameEvent: MTLSharedEvent!
    private var lastGuestFrame: UInt64 = 0
    private var staleInARow = 0
    // The fence verdict — see the block in renderFrame that writes it.
    private var loggedFenceVerdict = false
    private var fenceAtStart: UInt64 = 0
    private var fenceWatchFrames = 0

    // Keyed by (eye, stage). Indexed by stage from the start rather than kept
    // as a "last texture" global, because ovrp_GetEyeTextureStageCount is ours
    // to answer and Unity cycles stages: a pose or an image keyed to anything
    // but the stage reprojects one frame's picture with another frame's pose,
    // which looks like latency-dependent judder rather than like a bug.
    private var eyes: [Int: EyeAllocation] = [:]
    private let eyesLock = NSLock()
    private static func key(_ eye: Int, _ stage: Int) -> Int { eye &* 64 &+ stage }

    /// The same storage for a layer that is NOT an eye — an OpenXR quad, which
    /// for a guest in cinematic mode is the whole picture. Its own table because
    /// the shapes differ: an eye is one slice of a shared 2-slice array, a layer
    /// is a plain 2D texture of whatever size the guest's swapchain is.
    private var layers: [Int: MTLTexture] = [:]
    private let layersLock = NSLock()

    // The GUEST's foveation map (KL_VRR=1), and the eye size it was built for.
    //
    // Not to be confused with `drawable.rasterizationRateMaps` — that is the
    // system foveating THIS file's composite pass, and the two are deliberately
    // detached (see unwarpGrid). This one makes the guest rasterize its own
    // expensive pass at a reduced peripheral rate, which leaves the eye texture
    // warped, which `unwarpGrid` then undoes.
    //
    // Touched only from the guest's render thread, inside provideEyeTexture.
    private var guestRateMap: MTLRasterizationRateMap?
    private var guestRateW = 0, guestRateH = 0
    private var guestRateRefused = false

    // The display's own foveation curve, sampled off the drawable's rate maps
    // in primeDisplay and re-issued at the guest's eye size. Empty when the
    // drawable is not foveated, and then the synthetic curve in kl_glfb is used
    // instead.
    private var displayQualityX: [Float] = []
    private var displayQualityY: [Float] = []

    // ARKit is the pose-in seam kl_ovrp.h describes: "on visionOS it is ARKit's
    // WorldTrackingProvider answering the same call" the SDL viewer answers on
    // the host. Nothing below the seam changes — kl_ovrp still reports node 9
    // for the head and nodes 3/4 for the hands.
    private let arSession = ARKitSession()
    private let worldTracking = WorldTrackingProvider()
    private let handTracking = HandTrackingProvider()
    private let controllers = KleptonControllers()
    private var arRunning = false
    // Whether the hand provider is in the running set — false when the wearer
    // refused hand tracking, which must leave the head and Sense untouched.
    private var handsRunning = false

    // How much work reprojection is doing, sampled for the log.
    private var reprojectFrames = 0
    private var worstDelta: Float = 0

    // KL_SYNC_GUEST=1 restores the inline shape: kl_app_frame() called on
    // this thread, inside the submission window. That is the clock the lifecycle's
    // device numbers were measured against, so it stays reachable — and it is
    // the A/B for anything that looks like a pacing problem.
    // ...and it is not offered for a guest that brought its own frame loop —
    // Steam Link's OpenXR thread, or an Unreal guest's game thread — because
    // there is no inline frame to make: `kl_app_frame()` returns -1 for both by
    // design, and taking this arm would present black forever while looking
    // like a pacing choice.
    private let syncGuest = klEnvOn("KL_SYNC_GUEST", default: false)
                            && kl_app_target_owns_frame_loop() == 0

    // How many drawables went out before the guest had a picture to put in
    // them. Counted rather than logged per frame, because on device it is
    // legitimately hundreds and each one is uninteresting.
    private var blackFrames = 0
    private var loggedDepthRange = false

    // --- Frame cadence ------------------------------------------------------
    //
    // Whether our frames actually reach the display once per display period, or
    // whether the system is holding each one across two. This is the difference
    // between a picture that is merely late — which reprojection fixes every
    // period, and which is what this whole file is built to do — and a picture
    // that is REPEATED, which reprojection never gets to touch on the repeated
    // period. A held frame during head rotation is two images in the same place
    // the eye is not, i.e. temporal doubling, and it is invisible from every
    // other counter here: `presented`, `cmdbuf done/committed` and the timewarp
    // delta all look perfect while it happens.
    //
    // Measured against the display period this run actually observed
    // (primeDisplay), not against an assumed 90 or 120 Hz.
    private var displayPeriod: TimeInterval = 0

    // --- The predicted display time seam (round 22 found it, round 23 wires it)
    //
    // `kl_ovrp_predicted_display_time()` is what ovrp_GetPredictedDisplayTime
    // answers, and the guest called it **1330 times** in launch #21. Nothing in
    // this port ever pushed a value, so it fell through to
    // `clock_gettime(CLOCK_MONOTONIC)` — the guest predicted every pose for the
    // instant it ASKED rather than the instant the frame would be shown, short
    // by a whole pipeline latency. Pistol Whip pushes it (its
    // KleptonCompositor.swift, the same seam); this port never did, and round
    // 19's STATUS asserted otherwise on Pistol Whip's behaviour.
    //
    // DEFAULT OFF for launch #22. It is a real defect and it is fixed here, but
    // it is a SECOND mechanism and launch #22 is the pose-boundary A/B; two
    // changes in one launch confound both. `KL_PREDICT_DISPLAY_TIME=1` arms it
    // — that is round 24's whole diff if the boundary fix is not enough.
    private let pushPredictedDisplayTime =
        klEnvOn("KL_PREDICT_DISPLAY_TIME", default: false)
    private var monotonicOffsetCache: TimeInterval?

    /// LayerRenderer.Clock has its own epoch; kl_ovrp.h defines the seam as
    /// CLOCK_MONOTONIC. Measured once, back to back, and applied at the push.
    private func monotonicOffset() -> TimeInterval {
        if let o = monotonicOffsetCache { return o }
        let cs = LayerRenderer.Clock.Instant.epoch
            .duration(to: LayerRenderer.Clock().now).timeInterval
        var ts = timespec()
        clock_gettime(CLOCK_MONOTONIC, &ts)
        let mono = TimeInterval(ts.tv_sec) + TimeInterval(ts.tv_nsec) * 1e-9
        let o = mono - cs
        monotonicOffsetCache = o
        NSLog(String(format: "[cp] predicted display time is now wired to the "
                     + "guest: LayerRenderer.Clock %.6f s, CLOCK_MONOTONIC "
                     + "%.6f s, offset %+.6f s, lead %.3f ms",
                     cs, mono, o, max(displayPeriod, 0) * 1000))
        return o
    }
    private var lastPresentation: TimeInterval = 0

    // --- The boot rate SETTLE gate -------------------------------------------
    //
    // The panel does not start at the rate it runs at. A Vision Pro brings the
    // layer up at 120 Hz and drops to 90 once the immersive session is properly
    // engaged, some hundreds of milliseconds later — and priming used to take a
    // fixed eight frames, i.e. ~66 ms, which lands squarely inside that window.
    // So the rate the guest was told was decided by a race. It was won five
    // sessions running and then lost once, and the loss is not subtle: the
    // guest paced everything at 120 against a panel handing out 90 slots a
    // second, 4:3, every fourth guest frame sharing a slot with its neighbour —
    // a whole-image zigzag proportional to head speed that reads exactly like a
    // pose bug and is not one. (Session 16; the 2026-08-20 22:53 boot log.)
    //
    // So: measure until the cadence has been the SAME cadence for a while, and
    // only then latch. Everything downstream — the guest's one early Hz read,
    // the frame clock's tick under KL_RATE_MODE=clock, our own cadence ratio —
    // reads the latched value, and all of them happen after `primeDisplay`
    // returns (the guest thread does not exist until `kl_app_guest_start`
    // below it), so holding here holds all of them with no extra plumbing.
    //
    // The numbers, and why:
    //   KL_HZ_SETTLE_MS (500) — the run of agreeing intervals must span half a
    //     second. That is ~45 slots at 90 Hz and ~60 at 120: far longer than
    //     the 120->90 transition itself, which the bad log puts under one
    //     second end to end, and long enough that a rate that is still moving
    //     cannot fake it. Half a second of black at boot is free — Unity takes
    //     tens of seconds to come up behind it.
    //   KL_HZ_SETTLE_TIMEOUT_MS (3000) — a panel that never gives us a steady
    //     half second is a real possibility (thermal ramping, a device under
    //     load), and blocking the boot on it forever is worse than guessing.
    //     At three seconds we latch the median of everything seen and SAY SO,
    //     so a confounded session is identifiable from its own log.
    //   tolerance 6% — 120 and 90 are 33% apart, so this discriminates them
    //     with an order of magnitude to spare, while absorbing the sub-1 ms
    //     scheduling noise the `[fc]` reports show on this hardware.
    // A gap that is a near-exact 2x or 3x of the run's period is a HELD frame,
    // not a rate change; it extends the run rather than breaking it.
    private let hzSettle          = klEnvOn("KL_HZ_SETTLE", default: true)
    private let hzSettleMs        = Double(klEnvFloat("KL_HZ_SETTLE_MS", 500))
    private let hzSettleTimeoutMs = Double(klEnvFloat("KL_HZ_SETTLE_TIMEOUT_MS", 3000))
    private let hzSettleTol       = Double(klEnvFloat("KL_HZ_SETTLE_TOL", 0.06))
    private let hzSettleMinGaps   = Int(klEnvFloat("KL_HZ_SETTLE_MIN_GAPS", 120))
    private let hzSnapTol         = Double(klEnvFloat("KL_HZ_SNAP_TOL", 0.005))

    // ROUND 10, and the reason this needed a second round: a settle gate that
    // latches the WRONG RATE PRECISELY is worse than no gate, because it looks
    // right in the log. Round 9 latched 119.81 Hz against a panel running a
    // dead-steady 120.00 — 0.16% low, 13.9 microseconds a frame, which walks
    // the guest's publish one whole panel slot every five seconds. Austin got
    // a shake that self-corrected and came back, five or six times over one
    // menu session, and the publish walk read `0x 2% / 1x 96% / 2x 2%` where a
    // coherent session reads `1x 100%`. That IS the boundary crossing, counted.
    //
    // The cause was mine and it is worth naming exactly. The eight-frame code
    // this replaced took a MEDIAN, with a comment saying why: "the first frames
    // of a freshly-started layer are not representative and one of them is
    // enough to move an average by several Hz". The settle gate replaced that
    // with a MEAN — `span / slots` — and then anchored it on the run that
    // starts at the layer's very first frame, where this function is also
    // computing the foveation curve and pushing the eye textures. In the r9 log
    // that work sits between 18.252 and 18.753: the slow frames are INSIDE the
    // measured run. Worse, the running mean is also the tolerance reference, so
    // the outliers drag the reference far enough to keep accepting themselves,
    // which is why nothing was discarded and the run never broke.
    //
    // So: the run's membership test still uses the running mean (it is a coarse
    // 6% question and drag is harmless there), but the LATCHED VALUE is the
    // median of the run's per-slot gaps. A handful of slow boot frames then
    // cannot move it at all.
    //
    // Two more, for resolution rather than robustness:
    //   KL_HZ_SETTLE_MIN_GAPS (120) — 60 intervals cannot resolve 0.16%. 120 is
    //     ~1.0 s at 120 Hz and ~1.3 s at 90, still an order of magnitude inside
    //     Unity's boot, and it doubles the resolution of the median's centre.
    //   KL_HZ_SNAP_TOL (0.005) — a measurement within half a percent of a rate
    //     the hardware actually runs is that rate, and the guest is better told
    //     the exact number than our estimate of it. The candidate set is what
    //     visionOS documents for this panel (120/100/96/90) plus the 60 that a
    //     thermal demotion produces; the closest pair is 96 and 100, 4.2%
    //     apart, so half a percent has eight times the margin it needs.
    private static let canonicalHz: [Double] = [120, 100, 96, 90, 60]
    private var cadenceN = 0
    private var cadenceSum = 0.0
    private var cadenceMax = 0.0
    private var cadenceRepeats = 0      // gaps of 1.5 periods or more
    private var missedDeadline = 0
    // Consecutive liveness reports whose mean gap disagrees with the latched
    // display period by more than 10%. See cadenceSummary().
    private var cadenceDisagreed = 0
    private let noFence = klEnvOn("KL_CP_NOFENCE", default: false)
    // The last device anchor the tracker actually answered with, and how often
    // it has had to stand in. See displayOriginFromDevice for why holding it is
    // not the same kind of guess as the identity it replaced.
    private var lastGoodOriginFromDevice: simd_float4x4?
    private var anchorWalkedBack = 0
    private var anchorHeld = 0
    private var anchorUnknown = 0
    private let holdAnchor = klEnvOn("KL_CP_ANCHOR_HOLD", default: true)
    private var cmdCommitted = 0
    private let cmdLock = NSLock()
    private var cmdCompleted = 0
    private var cmdError: Error?
    private var loggedFirstPicture = false
    private var loggedGuestEnd = false

    // Why a frame produced nothing. Compositor Services expects this loop to
    // keep presenting, and every one of these three is a path that leaves
    // WITHOUT presenting — so if the system tears the app down for not holding
    // up its end, the reason is one of these counters and nothing else in the
    // log would say so. Reported by the liveness line in renderLoop().
    private var bailNoFrame = 0
    private var bailNoDrawable = 0
    private var bailNoPipeline = 0

    init(_ layerRenderer: LayerRenderer) {
        self.layerRenderer = layerRenderer

        // The system's own pinch, rather than our arithmetic on two fingertip
        // joints. `onSpatialEvent` is how a fully-immersive Metal app is told
        // that the user selected something — the same recogniser the rest of
        // visionOS uses, with the platform's own hysteresis, its own idea of
        // what counts as a pinch, and gaze already folded in. Two rounds of
        // hand-rolled distance thresholding were unstable at exactly the
        // moment they mattered, and there is no version of that arithmetic
        // that beats the recogniser the OS ships.
        let controllers = self.controllers
        layerRenderer.onSpatialEvent = { events in
            controllers.handleSpatialEvents(events)
        }
    }

    // MARK: - Poses

    private func startARKit() {
        // Before anything ARKit: name every GameController the process can see.
        // Installed here rather than in KleptonControllers' init because this is
        // the first point at which the app is far enough up that a connect
        // notification is worth having, and it must be in place BEFORE
        // makeAccessoryProvider reads the population — otherwise a controller
        // that attaches in that window is invisible in both places.
        KleptonControllers.installControllerLog()
        Task { await runARKit() }
    }

    /// Build the provider set and run the session.
    ///
    /// Idempotent and **re-runnable**: `AccessoryTrackingProvider` takes its
    /// accessory list at construction, so a controller connecting or
    /// disconnecting means a new provider, which means running the session
    /// again with a rebuilt list. That is not a restart — the session is not
    /// stopped first, world and hand tracking carry straight on, and this is
    /// how reconnects are meant to be handled.
    private func runARKit() async {
        do {
            // Hand tracking is requested separately because it is the half
            // that can be refused: world tracking is available to any full
            // immersive space, hand tracking needs the user's consent. A
            // refusal must leave the head working rather than take the
            // whole seam down with it.
            // The guest asks for views at an instant; only the tracker can
            // answer that, so it is handed to kl_ovrp here rather than having
            // kl_openxr guess from the pose we last published.
            Self.headAtTracker = worldTracking
            kl_ovrp_set_head_at(Self.headAt)
            let accessories = await controllers.makeAccessoryProvider()
            // **ASK, rather than let run() ask for us** — for every provider
            // that needs consent, hands included. A correct usage description
            // only makes the *prompt* legal — something still has to raise it,
            // and `run()` throwing `dataProviderNotAuthorized` a millisecond
            // after the provider was built is what a `notDetermined` nobody
            // requested looks like: indistinguishable in the log from a consent
            // the user denied. Hands used to be left to run(); a run whose hand
            // consent was never asked for got no skeleton at all.
            //
            // Consent is per **bundle id**, so another Klepton port having been
            // granted accessory or hand tracking says nothing about this one.
            //
            // Both statuses are logged — before and after — so a run names
            // which of the three it is: `notDetermined -> allowed` is this call
            // doing its job, `denied` in either column is a Settings toggle
            // only the wearer can move, and `allowed -> allowed` followed by a
            // throw means the refusal is about the provider (an empty
            // accessory list) and not about consent.
            let handsWanted = HandTrackingProvider.isSupported
            var want: [ARKitSession.AuthorizationType] = []
            if handsWanted { want += HandTrackingProvider.requiredAuthorizations }
            if accessories != nil, #available(visionOS 26.0, *) {
                want += AccessoryTrackingProvider.requiredAuthorizations
            }
            var granted: [ARKitSession.AuthorizationType: ARKitSession.AuthorizationStatus] = [:]
            if !want.isEmpty {
                let before = await arSession.queryAuthorization(for: want)
                granted = await arSession.requestAuthorization(for: want)
                NSLog("[cp] ARKit authorization \(want): \(before) -> \(granted)")
            }
            func allowed(_ needs: [ARKitSession.AuthorizationType]) -> Bool {
                needs.allSatisfy { granted[$0] == .allowed }
            }

            // **Each provider on its own consent.** run() refuses the whole set
            // if any one member is unauthorised, so a denied hand prompt used to
            // take world tracking down with it — the head fell back to the
            // synthetic standing pose for the sake of a fallback input. The
            // head never needs consent; hands and controllers join only when
            // theirs is granted, and a refused one is named rather than fatal.
            var providers: [any DataProvider] = [worldTracking]
            let handsOn = handsWanted && allowed(HandTrackingProvider.requiredAuthorizations)
            if handsOn { providers.append(handTracking) }
            else if handsWanted { NSLog("[cp] hand tracking NOT authorized — head and controllers only") }
            var senseOn = false
            if let accessories, #available(visionOS 26.0, *) {
                if allowed(AccessoryTrackingProvider.requiredAuthorizations) {
                    providers.append(accessories)
                    senseOn = true
                } else {
                    NSLog("[cp] accessory tracking NOT authorized — Sense poses unavailable")
                }
            }
            try await arSession.run(providers)
            arRunning = true
            handsRunning = handsOn
            NSLog("[cp] ARKit running (hands: \(handsOn), "
                  + "sense: \(controllers.senseConnected), accessory provider: \(senseOn))")
            if let accessories, senseOn {
                // A fresh anchor stream per provider. The previous one ends
                // when its provider is replaced, so these do not accumulate.
                Task { await controllers.consumeAccessoryAnchors(accessories) }
            }
        } catch {
            NSLog("[cp] ARKit failed to start: \(error) — falling back to the "
                  + "synthetic standing head kl_ovrp provides")
        }
    }

    /// Decompose a visionOS transform into the position+quaternion kl_ovrp wants.
    ///
    /// No basis change: visionOS and the Oculus tracking space share the
    /// convention the guest expects — right-handed, +Y up, -Z forward — so a
    /// flip here would be inventing a bug rather than fixing one.
    private static func decompose(_ m: simd_float4x4) -> (SIMD3<Float>, simd_quatf) {
        (SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z), simd_quatf(m))
    }

    /// The device anchor for the instant the composite is about to draw at.
    ///
    /// This and `originFromDevice` below are one answer in two halves, and what
    /// they exist for is that **`origin_from_device` is never the identity while
    /// the session has ever tracked.** `queryDeviceAnchor` answers nil for an instant it will not
    /// predict to, and how far ahead that is is neither documented nor fixed —
    /// a guest hitch that pushes the presentation time out, or a tracking
    /// interruption, is enough to reach it.
    ///
    /// Substituting the identity there does not weaken the correction, it
    /// asserts something false: that the head is at the world origin, unrotated.
    /// The composite places the guest's picture in the space its render pose
    /// defines and looks at it from this one, so the quad stops counteracting
    /// head motion and sits fixed in front of the face — and stays there for as
    /// long as the tracker keeps refusing, which is what makes it read as stuck
    /// rather than as a glitch.
    ///
    /// Holding the last pose actually measured is wrong by however far the head
    /// has moved since, which is bounded by the length of the outage. It is also
    /// the answer `updatePoses` already gives the guest, by skipping the push
    /// and leaving the previous pose in place: the two seams now fail the same
    /// way instead of opposite ways, which is what made the disagreement
    /// invisible.
    ///
    /// The walk-back is `headAt`'s, for `headAt`'s reason — stepping back finds
    /// the edge of what the tracker will predict to instead of guessing where it
    /// is. `KL_CP_ANCHOR_HOLD=0` restores the identity fallback as the A/B.
    private func displayAnchor(at presentation: TimeInterval) -> DeviceAnchor? {
        var t = presentation
        var steps = 0
        while steps < 10 {
            if let a = worldTracking.queryDeviceAnchor(atTimestamp: t) {
                if steps > 0 { anchorWalkedBack += 1 }
                return a
            }
            t -= 0.005
            steps += 1
        }
        return nil
    }

    /// ...and the transform the composite's own math uses, which is where the
    /// holding happens. Kept separate from the anchor above because the anchor
    /// OBJECT goes to `cp_drawable.deviceAnchor`, and that is the system's
    /// input to its own reprojection: handing it one we invented would put our
    /// correction and the system's on different heads. Nil there is a refusal
    /// the system understands; the identity here is a lie it cannot see.
    private func originFromDevice(_ anchor: DeviceAnchor?) -> simd_float4x4 {
        if let anchor {
            lastGoodOriginFromDevice = anchor.originFromAnchorTransform
            return anchor.originFromAnchorTransform
        }
        guard holdAnchor, let held = lastGoodOriginFromDevice else {
            // Nothing has ever tracked — the frames before ARKit starts — or the
            // knob is off. The identity is the only answer left, and it is
            // counted so that it can never again be taken silently.
            anchorUnknown += 1
            return matrix_identity_float4x4
        }
        anchorHeld += 1
        return held
    }

    /// Sample every pose for this frame and push it across the seam.
    ///
    /// Called once per frame, *before* kl_app_frame(), so the guest renders
    /// with this frame's pose rather than the previous one's — the same
    /// ordering reason the Choreographer is ticked before nativeRender.
    /// The last few `DeviceAnchor`s pushed to the guest, and the tag each went
    /// out under. A ring rather than one slot: the guest is decoupled, so the
    /// frame being composited can be several pushes old — at 120 Hz pushes and
    /// a 72 Hz guest that is two or three, and a stall makes it more. 128 is
    /// over a second of them and costs a pointer each.
    ///
    /// Only ever touched from the frame loop (`updatePoses` and `renderFrame`
    /// are the same thread), so it needs no lock. A miss — the tag has been
    /// overwritten, or the guest's frame predates the first push — falls back to
    /// the display anchor, which is the behaviour that existed before this.
    private static let poseAnchorRing = 128
    private var poseAnchors = [(tag: UInt64, anchor: DeviceAnchor)?](
        repeating: nil, count: poseAnchorRing)
    private var poseTag: UInt64 = 0

    private func poseAnchor(tag: UInt64) -> DeviceAnchor? {
        guard tag != 0 else { return nil }
        let slot = poseAnchors[Int(tag % UInt64(Self.poseAnchorRing))]
        return slot?.tag == tag ? slot?.anchor : nil
    }

    private func updatePoses(at presentationTime: TimeInterval) {
        guard arRunning else { return }

        // Predicted to the presentation time, not "now": the pose the guest
        // renders with should be the pose the display will show it at, which is
        // the whole point of having a deadline to render against.
        if let anchor = worldTracking.queryDeviceAnchor(atTimestamp: presentationTime) {
            // This is the pose the guest will render with, and kl_ovrp latches
            // it into the stage record when the guest reaches ovrp_BeginFrame —
            // which is what the composite pass reads back to know where the
            // picture it is about to show was drawn from.
            //
            // Keep the ANCHOR OBJECT too, under a tag that travels with the pose
            // (kl_ovrp.h, `pose_tag`). Seven floats are enough to build a
            // model-view from; they are not enough to hand Compositor Services a
            // `deviceAnchor`, and KL_WARP=system needs exactly that — the anchor
            // the guest's pixels embody, so the SYSTEM does the one and only
            // warp instead of correcting on top of our correction.
            poseTag &+= 1
            poseAnchors[Int(poseTag % UInt64(Self.poseAnchorRing))] = (poseTag, anchor)
            kl_ovrp_set_head_pose_tag(poseTag)
            // ...and DATE it with the instant it is about. The pose above was
            // predicted to `presentationTime`, so that — not "now" — is the
            // clock the head velocity has to be differenced on. The two differ
            // by this thread's scheduling jitter, and dividing a uniform
            // presentation-time step by a jittery call-time one multiplies the
            // derived velocity by a per-frame noise factor. See
            // klovrp_derive_motion in runtime/xr/kl_ovrp.c.
            kl_ovrp_set_head_pose_time(presentationTime)
            // In head space, not device space — see the midpoint argument at
            // originFromHead. The guest derives its eye positions from this
            // pose and the offsets pushEyeOffsets sent, and those two have to
            // be stated about one origin or every near object sits wrong.
            let (p, q) = Self.decompose(
                Self.headFrom(anchor.originFromAnchorTransform, midpoint: eyeMidpointInDevice))
            kl_ovrp_set_head_pose(p.x, p.y, p.z, q.imag.x, q.imag.y, q.imag.z, q.real)
        }

        // Hands and controllers both go through KleptonControllers, which is a
        // merge rather than a switch: a Sense controller wins per hand where it
        // is present, the wrist anchor fills in where it is not. Only a Sense
        // controller can press anything.
        // Controllers pair and unpair mid-run, and the provider list is fixed
        // at ARKitSession.run — so a changed set means rebuilding the accessory
        // provider and running the session again. Not a restart: world and hand
        // tracking carry on untouched.
        if controllers.spatialControllersChanged() {
            NSLog("[cp] spatial controller set changed — rebuilding providers")
            // The old provider's anchors are about to stop arriving; drop what
            // it left behind so a disconnected controller cannot hold its last
            // pose while the hand fallback waits behind it.
            controllers.forgetAccessoryPoses()
            Task { await runARKit() }
        }

        // The anchors themselves, not their transforms: the grip pose is built
        // from the hand skeleton (the metacarpal a fist closes around) and so
        // is the pinch trigger, and neither is recoverable from the anchor
        // origin alone.
        //
        // Predicted to the presentation time, like the head above and like the
        // Sense controllers inside `update`. `latestAnchors` is the last sample
        // the tracker delivered, which arrives well below display rate — so a
        // 90 Hz render loop reading it draws the same hand pose for two or
        // three frames running, and hands do not hold still that long. That
        // reads as the hands running at a third of the frame rate, which is
        // exactly what it is.
        var left: HandAnchor?, right: HandAnchor?
        if handsRunning {
            let hands = handTracking.handAnchors(at: presentationTime)
            if let a = hands.leftHand,  a.isTracked { left  = a }
            if let a = hands.rightHand, a.isTracked { right = a }
        }
        controllers.update(leftHand: left, rightHand: right, at: presentationTime)
        // ...and the same seam in the other direction: whatever the guest
        // queued for the controllers to play. Here rather than inside `update`
        // because it has nothing to do with the anchors that call takes — it is
        // output, and the only thing it shares with the input half is being
        // once a frame.
        controllers.pumpHaptics()
    }

    // MARK: - Setup

    /// Must run before the guest asks for eye storage, because the provider is
    /// what answers that question.
    func installProvider() {
        // ANGLE's own MTLDevice, not MTLCreateSystemDefaultDevice(). The
        // EGL_ANGLE_metal_texture_client_buffer extension requires the texture
        // to belong to the display's device; a texture from another device
        // creates an EGLImage that fails at bind time, and on a single-GPU Mac
        // the two happen to be the same object, so testing on the host would
        // not catch getting this wrong.
        let angle = kl_glfb_mtl_device()
        if let d = angle {
            device = Unmanaged<MTLDevice>.fromOpaque(d).takeUnretainedValue() as MTLDevice
        } else {
            // No ANGLE: the null GL driver, i.e. a run asking about the
            // lifecycle rather than the picture. There will be no eye textures
            // to composite — but this loop still has to hold up its end of the
            // bargain, which is to prime the display, present cleared drawables
            // and keep publishing poses to the guest. Its own device is enough
            // for all three, and *not* taking one here is what used to kill the
            // whole compositor in primeDisplay.
            guard let d = MTLCreateSystemDefaultDevice() else {
                NSLog("[cp] no MTLDevice at all — the compositor cannot run")
                return
            }
            device = d
        }
        queue = device.makeCommandQueue()
        buildPipeline()

        guard angle != nil else {
            // Not necessarily a run without a picture: a VULKAN guest never
            // brings ANGLE up at all, and its eye textures arrive from MoltenVK
            // through kl_glfb's eye table instead of through the provider below
            // (see eyeSource). Everything ANGLE-specific — the provider and the
            // shared event — is what is skipped here, and neither exists on
            // that path.
            NSLog("[cp] ANGLE has no MTLDevice — no GL provider and no GL frame "
                  + "fence this run (KL_GLFB unset, or the guest is Vulkan); "
                  + "drawables will be cleared and presented, and any eye "
                  + "texture that reaches kl_glfb's table is still composited")
            return
        }

        // Register the shared event before the guest can swap, so no frame is
        // ever composited without the wait.
        if let ev = device.makeSharedEvent() {
            guestFrameEvent = ev
            kl_glfb_set_gpu_fence(Unmanaged.passUnretained(ev).toOpaque())
        } else {
            NSLog("[cp] no MTLSharedEvent — the composite cannot be ordered "
                  + "against ANGLE's queue")
        }

        let ctx = Unmanaged.passUnretained(self).toOpaque()
        kl_glfb_set_mtl_provider({ (eye, stage, w, h, fmt, out, ctx) -> Int32 in
            guard let ctx, let out else { return 0 }
            let me = Unmanaged<KleptonCompositor>.fromOpaque(ctx).takeUnretainedValue()
            return me.provideEyeTexture(eye: Int(eye), stage: Int(stage),
                                        w: Int(w), h: Int(h), format: fmt, out: out)
        }, ctx)
        // ...and the same for the layers that are not eyes, without which a
        // guest whose whole frame is a quad has nothing for the overlay pass to
        // sample (kl_glfb.h).
        kl_glfb_set_mtl_layer_provider({ (layer, stage, w, h, fmt, out, ctx) -> Int32 in
            guard let ctx, let out else { return 0 }
            let me = Unmanaged<KleptonCompositor>.fromOpaque(ctx).takeUnretainedValue()
            return me.provideLayerTexture(layer: Int(layer), stage: Int(stage),
                                          w: Int(w), h: Int(h), format: fmt, out: out)
        }, ctx)
        NSLog("[cp] MTL eye and layer providers installed on \(device.name) "
              + "(vertex amplification \(amplification))")
    }

    private func buildPipeline() {
        // The shader is kl_reproject.c's, shared verbatim with the macOS
        // viewer's compositor (runtime/gfx/kl_view_mtl.m) — one copy of the flip,
        // the quad and the array-slice sampler, because having them diverge
        // means debugging the picture twice, and the viewer is where this math
        // can actually be run before a device is available. Compiled from
        // source rather than shipped as a .metal so it stays beside the
        // matrices that feed it (kl_reproject_build).
        let src = String(cString: kl_reproject_msl())
        // The pipeline has to be linked against the format the LAYER got, not
        // against a constant: with the drawable now 8-bit sRGB by default, a
        // pipeline still claiming rgba16Float does not merely look wrong, it
        // fails to encode at all (Metal refuses the attachment mismatch) and
        // every frame goes out cleared.
        let color = layerRenderer.configuration.colorFormat
        do {
            let lib = try device.makeLibrary(source: src, options: nil)
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
            desc.fragmentFunction = lib.makeFunction(name: "kl_reproject_f")
            desc.colorAttachments[0].pixelFormat = color
            desc.depthAttachmentPixelFormat = .depth32Float
            // Both eyes in one pass — see encodeComposite. Declared whatever
            // the layout turns out to be, because a pipeline that permits
            // amplification costs nothing when it is not amplified. Clamped to
            // what the GPU has: asking for 2 where only 1 exists does not
            // degrade, it fails the pipeline, and a nil pipeline is a black
            // display for the whole run.
            amplification = device.supportsVertexAmplificationCount(2) ? 2 : 1
            desc.maxVertexAmplificationCount = amplification
            pipeline = try device.makeRenderPipelineState(descriptor: desc)

            // Depth WRITES, which this pass never did. visionOS reprojects the
            // submitted frame using its depth buffer, so a pass that writes no
            // depth submits perfect colour with the geometry "nothing is here at
            // any finite distance" — and the compositor honours that literally.
            // Measured on device: a fullscreen quad at reverse-Z 0 with writes
            // off is invisible; the same quad at 0.05 (2 m) with writes on
            // appears. .greater matches the reverse-Z clear of 0.
            let dsd = MTLDepthStencilDescriptor()
            dsd.depthCompareFunction = .greater
            dsd.isDepthWriteEnabled = true
            depthState = device.makeDepthStencilState(descriptor: dsd)

            // ...and the state for everything drawn AFTER the first thing at a
            // given distance. `.greater` against a depth already written is
            // false for an EQUAL depth, and every projection layer is a quad at
            // KL_REPROJECT_DEPTH — so with the state above left in force, layer
            // 0 writes its depth and every layer after it is discarded by the
            // depth test. That is the picture losing everything the guest
            // stacked on it, with no error surface: each draw is issued, each
            // returns, and the display shows the backdrop. Measured on device
            // as a lower-detail, shimmering picture, shimmering because WHICH
            // layer is first changes while the guest runs.
            let dsdAlways = MTLDepthStencilDescriptor()
            dsdAlways.depthCompareFunction = .always
            dsdAlways.isDepthWriteEnabled = false
            depthPassState = device.makeDepthStencilState(descriptor: dsdAlways)

            // The overlay pass — the guest's non-eye layers, which are the whole
            // of an Unreal title's UI. See encodeOverlays.
            //
            // The one pipeline here that BLENDS, and the eye pass must not: a
            // guest leaves the eye texture's alpha at 0 because that layer is
            // composited opaque, so a blending eye pass hands the
            // display a transparent frame over a correct picture. A UI quad's
            // alpha is authored and is the point of it.
            let odesc = MTLRenderPipelineDescriptor()
            let olib = try device.makeLibrary(source: String(cString: kl_reproject_overlay_msl()),
                                              options: nil)
            odesc.vertexFunction   = olib.makeFunction(name: "kl_ov_v")
            odesc.fragmentFunction = olib.makeFunction(name: "kl_ov_f")
            odesc.colorAttachments[0].pixelFormat = color
            odesc.depthAttachmentPixelFormat = .depth32Float
            odesc.maxVertexAmplificationCount = amplification
            odesc.colorAttachments[0].isBlendingEnabled = true
            odesc.colorAttachments[0].rgbBlendOperation = .add
            odesc.colorAttachments[0].alphaBlendOperation = .add
            odesc.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
            odesc.colorAttachments[0].sourceAlphaBlendFactor = .one
            odesc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
            odesc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
            overlayPipeline = try? device.makeRenderPipelineState(descriptor: odesc)
            if overlayPipeline == nil {
                NSLog("[cp] the overlay pipeline failed to build — a guest's non-eye "
                      + "layers will not be composited (on Unreal that is its whole UI)")
            }

            // KL_CP_PROBE: swap ONE thing about the pass and see if the black
            // moves. Each rung isolates a different link in the chain, so a
            // single device run localises a dark picture instead of a session
            // of one-hypothesis-per-build. See kl_reproject.c for the shaders.
            if probe > 0 {
                let d = MTLRenderPipelineDescriptor()
                d.colorAttachments[0].pixelFormat = color
                d.depthAttachmentPixelFormat = .depth32Float
                d.maxVertexAmplificationCount = amplification
                switch probe {
                case 2:  d.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
                         d.fragmentFunction = lib.makeFunction(name: "kl_probe_solid_f")
                case 3:  d.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
                         d.fragmentFunction = lib.makeFunction(name: "kl_probe_opaque_f")
                case 4:  d.vertexFunction   = lib.makeFunction(name: "kl_probe_full_v")
                         d.fragmentFunction = lib.makeFunction(name: "kl_probe_full_f")
                default: d.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
                         d.fragmentFunction = lib.makeFunction(name: "kl_reproject_f")
                }
                if probe >= 2 && probe <= 4 {
                    probePipeline = try? device.makeRenderPipelineState(descriptor: d)
                }
                NSLog("[cp] PROBE \(probe): \(Self.probeName(probe)) — "
                      + "this is a DIAGNOSTIC frame, not the real pass")
            }
        } catch {
            NSLog("[cp] composite pipeline failed: \(error)")
            return
        }
        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear; sd.magFilter = .linear
        sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
        sampler = device.makeSamplerState(descriptor: sd)
    }

    // MARK: - The provider

    /// Sample one rate map's per-axis quality curve into `n` uniform screen-space
    /// zones.
    ///
    /// A rate map is `screen -> physical`, and its local *rate* is that
    /// function's slope. So walking `physicalCoordinates(screenCoordinates:)`
    /// across uniform screen steps and differencing recovers the curve directly
    /// — no need for the descriptor that built it, which a built map does not
    /// report (the same reason `kl_glfb_eye_rate_zones` has to carry the zone
    /// count for maps we *do* build).
    ///
    /// Metal renormalizes quality so the peak is one sample per pixel, so the
    /// scale of what comes back does not matter, only the shape.
    private static func sampleCurve(_ map: MTLRasterizationRateMap, layer: Int,
                                    zonesX: Int, zonesY: Int) -> ([Float], [Float]) {
        let scr = map.screenSize
        func axis(_ n: Int, _ extent: Int, _ physicalAt: (Float) -> Float) -> [Float] {
            var q = [Float](repeating: 0, count: n)
            var prev = physicalAt(0)
            var peak: Float = 0
            for i in 0..<n {
                let next = physicalAt(Float(extent) * Float(i + 1) / Float(n))
                q[i] = max(0, next - prev)
                peak = max(peak, q[i])
                prev = next
            }
            // A degenerate map would make the descriptor invalid; flat full rate
            // is the honest reading of "no variation along this axis".
            guard peak > 0 else { return [Float](repeating: 1, count: n) }
            // Metal renormalizes so the peak is one sample per pixel, so only
            // the shape survives; the floor keeps a zero-width zone out of the
            // descriptor.
            for i in 0..<n { q[i] = max(q[i] / peak, 0.01) }
            return q
        }
        let qx = axis(zonesX, scr.width) {
            map.physicalCoordinates(screenCoordinates: MTLSamplePosition(x: $0, y: 0),
                                    layer: layer).x
        }
        let qy = axis(zonesY, scr.height) {
            map.physicalCoordinates(screenCoordinates: MTLSamplePosition(x: 0, y: $0),
                                    layer: layer).y
        }
        return (qx, qy)
    }

    /// Take the display's own foveation curve off a drawable, so the guest
    /// rasterizes the way the compositor already does instead of the way we
    /// guessed.
    ///
    /// Called from `primeDisplay`, which is before the guest boots and therefore
    /// before any eye texture exists — the ordering `updateGuestRateMap` needs.
    /// Sampled once, from the first drawable, exactly as `pushEyeTextureSize`
    /// is: `renderQuality` ramps rather than switching, so an early drawable can
    /// still carry the old quality — but quality scales the *resolution*, and
    /// only the normalized shape of the curve is taken from here.
    ///
    /// **The two eyes' maps are combined, per zone, by maximum.** visionOS gives
    /// each view its own map and they are not the same map mirrored-and-centred:
    /// the eyes converge, so each one's dense region sits inboard, and the pair
    /// are mirror images of each other. The guest cannot be given two, because
    /// both eyes render through ONE multisampled scene renderbuffer and resolve
    /// into ONE two-slice array texture — the map is registered against those
    /// objects, and neither of them knows which eye is in flight. Taking the
    /// envelope makes a symmetric curve that is dense wherever *either* eye
    /// wants density: strictly more samples than either map alone, so no eye is
    /// ever under-rendered, at the cost of some fragments the other eye did not
    /// need. Per-eye guest maps would mean per-eye scene storage, which is a
    /// different change to make deliberately.
    private func captureDisplayFoveation(_ drawable: LayerRenderer.Drawable) {
        guard displayQualityX.isEmpty else { return }
        let maps = drawable.rasterizationRateMaps
        guard let first = maps.first else {
            NSLog("[cp] drawable is not foveated — KL_VRR would use the synthetic "
                  + "curve (KL_VRR_ZONES / KL_VRR_EDGE)")
            return
        }
        // The zone count, ALVR's way (DummyMetalRenderer): a built map does not
        // report its zones, but physicalSize / physicalGranularity is the number
        // of cells it actually resolves to. Capped at the unwarp grid's own
        // limit, since a grid cell per zone is what makes the unwarp exact.
        let gran = first.physicalGranularity
        let phys = first.physicalSize(layer: 0)
        let cap = Int(KL_REPROJECT_GRID_MAX)
        let nx = max(1, min(cap, gran.width  > 0 ? phys.width  / gran.width  : 16))
        let ny = max(1, min(cap, gran.height > 0 ? phys.height / gran.height : 16))

        var qx = [Float](repeating: 0, count: nx)
        var qy = [Float](repeating: 0, count: ny)
        var sampled = 0
        for map in maps {
            for layer in 0..<map.layerCount {
                let (mx, my) = Self.sampleCurve(map, layer: layer, zonesX: nx, zonesY: ny)
                for i in 0..<nx { qx[i] = max(qx[i], mx[i]) }
                for i in 0..<ny { qy[i] = max(qy[i], my[i]) }
                sampled += 1
            }
        }
        guard sampled > 0 else { return }
        displayQualityX = qx
        displayQualityY = qy
        // BOTH curves, and y is not decoration: the horizontal one is an
        // envelope of two mirrored maps and is symmetric by construction, so it
        // can never show an up/down asymmetry even when one exists. The
        // vertical one is the only place a "the top periphery looks different
        // from the bottom" observation can be settled — a lopsided qy is the
        // display's own distribution being copied faithfully, a symmetric one
        // means look at our unwarp instead. Screen y=0 here is the render
        // target's row 0, which is the picture's BOTTOM (the guest stores it
        // GL-side-up), so qy is printed bottom-to-top.
        func curve(_ q: [Float]) -> String {
            q.map { String(format: "%.2f", $0) }.joined(separator: " ")
        }
        NSLog("%@", "[cp] display foveation curve: \(nx)x\(ny) zones from \(sampled) "
              + "view map(s), screen \(first.screenSize.width)x\(first.screenSize.height) "
              + "-> physical \(phys.width)x\(phys.height), granularity "
              + "\(gran.width)x\(gran.height)\n"
              + "[cp]   x rate (left to right): \(curve(qx))\n"
              + "[cp]   y rate (bottom to top): \(curve(qy))")
    }

    /// Build (or rebuild) the guest's foveation map for a `w` x `h` eye and push
    /// it to kl_glfb.
    ///
    /// **The curve is the display's**, resampled to the guest's screen size —
    /// which is why this is not simply handing the drawable's map straight
    /// through. The guest's eye size is not the drawable's: it comes from
    /// `kl_ovrp_set_eye_texture_size`, which caps and scales (`KL_OVRP_EYE_MAX`,
    /// `KL_OVRP_EYE_SCALE`) because Unity allocates stages x eyes of RGBA16F at
    /// whatever it is told. A rate map is bound to the `screenSize` it was built
    /// for, so a map for the drawable's resolution applied to a smaller eye
    /// texture would foveate against coordinates the guest never renders. Same
    /// shape, our size. `KL_VRR_ZONES` / `KL_VRR_EDGE` are the fallback for an
    /// unfoveated drawable, and are what the host stand-in
    /// (`klmtl_update_rate_map` in tests/t_mtl_provider.m) always uses.
    ///
    /// **Called before the eye texture is handed back**, deliberately. kl_glfb
    /// binds the texture as soon as this provider returns, and `FramebufferMtl`
    /// caches its render pass descriptor — a map that arrives after the first
    /// pass into a texture is silently ignored for that pass, and re-attaching
    /// the texture is not enough to force the rebuild.
    ///
    /// Keyed on the size because Unity re-creates its eye textures mid-startup
    /// at a different one; a map built for the old screen size would foveate
    /// against coordinates that no longer exist.
    private func updateGuestRateMap(w: Int, h: Int) {
        var zones: Int32 = 0
        var edge: Float = 0
        guard kl_glfb_foveation_wanted(&zones, &edge) != 0 else { return }
        if guestRateMap != nil, guestRateW == w, guestRateH == h { return }
        guard !guestRateRefused else { return }
        guard device.supportsRasterizationRateMap(layerCount: 1) else {
            // Said once: this is a property of the GPU, so it will not become
            // true later in the run.
            guestRateRefused = true
            NSLog("[cp] this GPU has no variable rasterization rate — KL_VRR ignored")
            return
        }
        var qx = displayQualityX, qy = displayQualityY
        var source = "the display's own"
        if qx.isEmpty || qy.isEmpty {
            let n = Int(zones)
            qx = [Float](repeating: 0, count: n)
            qy = [Float](repeating: 0, count: n)
            qx.withUnsafeMutableBufferPointer { kl_glfb_foveation_quality($0.baseAddress, zones, edge) }
            qy.withUnsafeMutableBufferPointer { kl_glfb_foveation_quality($0.baseAddress, zones, edge) }
            source = "synthetic, edge rate \(String(format: "%.2f", edge))"
        }

        // ---- THE RATE FLOOR, AND WHY A RATE IS A MIP LEVEL ----
        //
        // A rasterization rate map does not only thin the fragments. The
        // fragment shader runs in PHYSICAL space, so `dFdx`/`dFdy` — and every
        // texture LOD Metal derives from them — measure physical pixels, not
        // screen pixels. Nothing renormalizes them. A zone rasterized at rate
        // `r` therefore selects a mip level `log2(1/r)` HIGHER than the same
        // surface would get unfoveated: rate 0.5 is +1 level, 0.25 is +2, and
        // this display's peripheral 0.10 is **+3.3 levels**, a 10x over-blur.
        //
        // That is the blue fill. The Tetris Effect well backdrop is a fine
        // dark-blue particle field; at its true LOD it reads as black with a
        // few bright specks, and at +3 mips it collapses to its own average —
        // a flat (0, 24, 46). Austin's three screenshots carry that colour
        // pixel-identically at three different zooms, which is what says the
        // fill is a constant rather than fog, depth fighting or a gamma error.
        // The bands are the y curve's own shape: full rate across the middle of
        // the screen, falling to a tenth at the top and bottom edges. That is
        // screen-fixed, so the well slides through it as the head pitches, rolls
        // with it, and covers more of it the closer you stand — which is
        // Austin's phenomenology, all four parts, with nothing left over.
        //
        // The display's curve is right for the COMPOSITOR's 6888x5525 drawable
        // and is not automatically right for a guest rendering one 4032-wide eye
        // through a GL driver that has never heard of it. Flooring the rate
        // bounds the damage at a stated number of mip levels while keeping most
        // of the saving: the floor is the deepest over-blur we are willing to
        // hand the guest, not a foveation off switch.
        let rateFloor: Float = min(1.0, max(0.01, klEnvFloat("KL_VRR_FLOOR", 0.5)))
        func bias(_ q: [Float]) -> Double {
            guard let m = q.min(), m > 0 else { return 0 }
            return log2(1.0 / Double(m))
        }
        func frac(_ a: [Float], _ b: [Float]) -> Double {
            guard !a.isEmpty, !b.isEmpty else { return 1 }
            let ma = Double(a.reduce(0, +)) / Double(a.count)
            let mb = Double(b.reduce(0, +)) / Double(b.count)
            return ma * mb
        }
        let rawBiasX = bias(qx), rawBiasY = bias(qy), rawFrac = frac(qx, qy)
        if rateFloor > 0.01 {
            for i in qx.indices { qx[i] = max(qx[i], rateFloor) }
            for i in qy.indices { qy[i] = max(qy[i], rateFloor) }
        }

        // ---- THE RAMP: no zone may fall off faster than KL_VRR_RAMP per zone ----
        //
        // A floor bounds HOW FAR the LOD bias goes; it says nothing about how
        // FAST it gets there. The display's curve falls 0.85 -> 0.74 -> 0.53
        // -> 0.42 across three zone rows above the fovea (and 0.47 -> 0.61 ->
        // 0.86 below it, 0.43 -> 0.61 -> 0.87 at each side), so a floored map
        // changes rate by up to 0.5 of a mip level in ONE zone. A texture LOD
        // step that sharp is a visible LINE that is fixed on the screen and
        // moves over the world with the head — Vivecraft launch 6: "a
        // horizontal seam near the top… the line almost colours the trees a
        // slightly darker green" (Minecraft's alpha-tested leaves darken with
        // each mip level).
        //
        // KL_VRR_RAMP=r (0.5..0.99) replaces each curve with its smallest
        // envelope that never falls by more than a factor r per zone:
        // q'[i] = max_j q[j] * r^|i-j|. It only RAISES rates (never below the
        // display's own need), keeps the floor, and turns the step into a
        // gradient of log2(1/r) mip levels per zone (0.92 -> 0.12 per zone,
        // ~58 px at 4608). Off (1.0) by default: every other port keeps its map.
        let ramp = klEnvFloat("KL_VRR_RAMP", 1.0)
        func maxStep(_ q: [Float]) -> Double {
            var m = 0.0
            for i in q.indices.dropFirst() where q[i] > 0 && q[i - 1] > 0 {
                m = max(m, abs(log2(Double(q[i]) / Double(q[i - 1]))))
            }
            return m
        }
        let stepBefore = max(maxStep(qx), maxStep(qy))
        if ramp >= 0.5 && ramp < 0.999 {
            func envelope(_ q: [Float]) -> [Float] {
                var out = q
                for i in q.indices {
                    var best = q[i]
                    for j in q.indices {
                        let v = q[j] * powf(ramp, Float(abs(i - j)))
                        if v > best { best = v }
                    }
                    out[i] = min(1.0, best)
                }
                return out
            }
            qx = envelope(qx); qy = envelope(qy)
            NSLog("%@", "[cp] foveation RAMP \(String(format: "%.2f", ramp))/zone — "
                  + "worst per-zone step \(String(format: "%.2f", stepBefore)) -> "
                  + "\(String(format: "%.2f", max(maxStep(qx), maxStep(qy)))) mip level(s); "
                  + "fragments now \(String(format: "%.1f", 100 * frac(qx, qy)))% "
                  + "(KL_VRR_RAMP=1 is the A/B)")
        } else {
            NSLog("%@", "[cp] foveation ramp off — worst per-zone step "
                  + "\(String(format: "%.2f", stepBefore)) mip level(s) "
                  + "(KL_VRR_RAMP=0.92 smooths it)")
        }
        NSLog("%@", "[cp] guest curve x: " + qx.map { String(format: "%.2f", $0) }.joined(separator: " "))
        NSLog("%@", "[cp] guest curve y (bottom to top): "
              + qy.map { String(format: "%.2f", $0) }.joined(separator: " "))
        NSLog("%@", "[cp] foveation LOD: rate floor "
              + String(format: "%.2f", rateFloor)
              + " — worst mip bias x +\(String(format: "%.2f", rawBiasX)) -> "
              + "+\(String(format: "%.2f", bias(qx))), y +\(String(format: "%.2f", rawBiasY)) -> "
              + "+\(String(format: "%.2f", bias(qy)))"
              + "; fragments \(String(format: "%.1f", 100 * rawFrac))% -> "
              + "\(String(format: "%.1f", 100 * frac(qx, qy)))% "
              + "(KL_VRR_FLOOR=0.01 restores the display's raw curve)")

        // ---- ONE EYE'S CURVE BELONGS TO ONE EYE (883f07d, the other half) ----
        //
        // `qx` describes ONE eye, whichever branch above produced it.
        // captureDisplayFoveation walks a map whose screenSize is one eye
        // (6888x5525 on this device — one map, two LAYERS, one per view) and
        // takes the per-zone maximum of the two layers; the twin peak that
        // comes out is two mirror-image single-peaked curves overlaid, with the
        // saddle at THAT EYE's own centre. It is not, as it looks, a
        // side-by-side envelope.
        //
        // A DoubleWide guest renders both eyes into one 12288-wide canvas, and
        // handing that canvas a one-eye curve stretches the curve to twice its
        // width. `883f07d` guards the case where the map's SIZE disagrees with
        // the texture's — but the sizes agree here and the SHAPE is still
        // wrong, which is the same rule one level down.
        //
        // Measured on the r18 canvas: 64 zones over 12288 is 192 screen px a
        // zone, so eye 0's dense region lands at 66-81% across its own half
        // (its fovea displaced by about a quarter of an eye-width, towards the
        // nose) and its outboard edge rasterizes at rate 0.12 — an eighth —
        // where the eye's true periphery should be. Both eyes, mirrored.
        //
        // The fix is the curve tiled ONCE PER EYE rather than stretched across
        // them. That also makes the unwarp grid exactly zone-aligned instead of
        // accidentally so: nx clamps at KL_REPROJECT_GRID_MAX (64) and the grid
        // is built per eye, so 64 cells now cover one eye's 64 zones one-for-one
        // where before they covered 32 at two cells each.
        //
        // NOT a rescale of the curve and not a guess at what the guest wanted —
        // it is the same curve, applied to each of the two things it describes.
        let doublewide = kl_glfb_doublewide() != 0
        let oneEyeX = qx
        if doublewide && !qx.isEmpty {
            qx = qx + qx
            source += ", tiled per eye across the DoubleWide canvas"
        }

        // `__sampleCount:` is the pointer-taking initializer as Swift imports
        // it; the array-taking spelling does not exist here.
        func makeMap(_ hq: [Float]) -> MTLRasterizationRateMap? {
            let layer = hq.withUnsafeBufferPointer { hp in
                qy.withUnsafeBufferPointer { vp in
                    MTLRasterizationRateLayerDescriptor(
                        __sampleCount: MTLSize(width: hq.count, height: qy.count, depth: 0),
                        horizontal: hp.baseAddress!, vertical: vp.baseAddress!)
                }
            }
            let desc = MTLRasterizationRateMapDescriptor()
            desc.screenSize = MTLSize(width: w, height: h, depth: 0)
            desc.label = "klepton guest foveation"
            desc.setLayer(layer, at: 0)
            return device.makeRasterizationRateMap(descriptor: desc)
        }
        var map = makeMap(qx)
        if map == nil && qx.count != oneEyeX.count {
            // Doubling the zone count is the only thing new here, so a refusal
            // is most likely a limit on it. Falling back to the stretched curve
            // is the pre-r18 behaviour — worse, and enormously better than
            // losing foveation altogether, which is what an outright refusal
            // would have cost (31.5% of the fragments becomes 100%).
            NSLog("[cp] makeRasterizationRateMap \(w)x\(h) refused \(qx.count) "
                  + "horizontal zones — falling back to the one-eye curve "
                  + "stretched across both eyes")
            qx = oneEyeX
            source = source.replacingOccurrences(
                of: ", tiled per eye across the DoubleWide canvas",
                with: ", STRETCHED across both eyes (the tiled map was refused)")
            map = makeMap(qx)
        }
        guard let map else {
            guestRateRefused = true
            NSLog("[cp] makeRasterizationRateMap \(w)x\(h) failed — KL_VRR ignored")
            return
        }
        let phys = map.physicalSize(layer: 0)
        // "%@", not the string itself: NSLog's first argument is a FORMAT
        // string, and this one contains a literal per-cent sign — "% o" was
        // being eaten as a conversion, so the device's first foveation line
        // read "33.50f the fragments".
        NSLog("%@", "[cp] guest foveation: screen \(w)x\(h) -> physical "
              + "\(phys.width)x\(phys.height) "
              + "(\(String(format: "%.1f", 100.0 * Double(phys.width * phys.height) / Double(w * h)))% "
              + "of the fragments, \(qx.count)x\(qy.count) zones, \(source))")
        // Held by this property, which is what keeps it alive: kl_glfb takes an
        // unmanaged pointer and never retains, exactly as it does not retain the
        // eye textures.
        guestRateMap = map
        guestRateW = w; guestRateH = h
        kl_glfb_set_eye_rate_map(Int32(w), Int32(h), Int32(qx.count), Int32(qy.count),
                                 Unmanaged.passUnretained(map).toOpaque())
    }

    /// kl_glfb calls this when the guest asks for storage for one (eye, stage).
    ///
    /// Two things here are load-bearing and both were learned by running it
    ///: the texture must be allocated on ANGLE's device, and the `w`/`h`
    /// reported back must be the texture's *real* size. The EGL extension takes
    /// the size from the MTLTexture and ignores what we claim, so a wrong-sized
    /// texture binds **successfully** and the guest renders into storage it does
    /// not have. kl_glfb refuses a mismatch rather than accept that.
    private func provideEyeTexture(eye: Int, stage: Int, w: Int, h: Int,
                                   format: UInt32,
                                   out: UnsafeMutablePointer<kl_mtl_eye_texture>) -> Int32 {
        let k = Self.key(eye, stage)
        let partnerKey = Self.key(eye == 0 ? 1 : 0, stage)

        guard let pixelFormat = klEyePixelFormat(format) else {
            NSLog("[cp] eye \(eye) stage \(stage) asks for GL internalformat "
                  + String(format: "0x%x", format)
                  + ", which has no MTLPixelFormat here — declining, so this eye "
                  + "renders into GL storage nothing composites")
            return 0
        }

        // Before anything is handed back, including on the cached path: the
        // size is only knowable here, and the map has to be in force before the
        // texture is bound. A no-op once the map matches this size.
        updateGuestRateMap(w: w, h: h)

        // **Nothing slow happens under eyesLock.** The render loop takes it once
        // per view per frame, so anything held across it stops frame submission
        // dead — and Compositor Services kills a client that stops submitting.
        // This function used to hold it across `makeTexture`, which at a loading
        // transition means three ~164 MB private allocations back to back while
        // the guest is also allocating hard. That is seconds of no frames, and
        // it presented as the app being backgrounded or killed *during loading
        // transitions* specifically — the one moment the eye swapchain is
        // re-created. The lock now covers dictionary access only.
        //
        // Unity really does re-create its eye textures at a different size
        // mid-startup — 1832x1920 during nativeRecreateGfxState, then 2198x2304
        // once the VRDevice has settled, and again on entering a map. A provider
        // that caches one texture per stage (the obvious implementation) hands
        // back the small one, so the size is part of the identity, not just the
        // key.
        eyesLock.lock()
        let cached = eyes[k]
        var partner = eyes[partnerKey]
        eyesLock.unlock()

        // The format is part of the identity for the same reason the size is:
        // handing back a texture whose pixel format is not the one the guest
        // asked for gets the pair refused by ANGLE, and the eye then quietly
        // falls back to GL storage nothing samples.
        func fits(_ a: EyeAllocation) -> Bool {
            a.texture.width == w && a.texture.height == h
                && a.texture.pixelFormat == pixelFormat
        }

        if let a = cached, fits(a) {
            out.pointee = kl_mtl_eye_texture(texture: Unmanaged.passUnretained(a.texture).toOpaque(),
                                             slice: Int32(a.slice),
                                             w: Int32(a.texture.width),
                                             h: Int32(a.texture.height))
            return 1
        }

        // Both eyes share one array texture, so the second eye must find the
        // first eye's allocation rather than make its own.
        var tex: MTLTexture? = nil
        if let p = partner, fits(p) { tex = p.texture }

        if tex == nil {
            let desc = MTLTextureDescriptor()
            desc.textureType = .type2DArray
            desc.pixelFormat = pixelFormat
            desc.width = w; desc.height = h
            desc.arrayLength = 2                 // one slice per eye
            desc.mipmapLevelCount = 1
            // renderTarget because the guest draws into it through the EGLImage;
            // shaderRead because this file's composite pass samples it back.
            desc.usage = [.renderTarget, .shaderRead]
            desc.storageMode = .private

            guard let t = device.makeTexture(descriptor: desc) else {
                NSLog("[cp] could not allocate eye \(eye) stage \(stage) \(w)x\(h)")
                return 0
            }
            t.label = "klepton eye stage \(stage)"
            tex = t
            // CLEARED, once, before the guest is ever handed it.
            //
            // A `.private` allocation of half a gigabyte holds whatever was in
            // that memory, and the guest does not write all of it: under the
            // rate map this title rasterizes 31.5% of the fragments
            // (12288x4928 screen -> 7874x2422 physical), so about two thirds of
            // every stage is never touched by anything at all. Any sampling
            // that reaches past the written region — an off-by-one at the grid's
            // outer ring, a rect measured against the wrong texture, a rate map
            // one generation stale — reads that memory and shows it, and it
            // shows it as a CONSTANT COLOUR because that is what recycled
            // texture memory looks like.
            //
            // Austin's blue fill is that shape of defect: a large flat colour
            // with a boundary that moves with the head and is not in the game's
            // own art (he checked footage, 2026-08-23). This does not diagnose
            // it — the render-viewport watch and `[rp] guard:` are the
            // instruments for that — but it removes the only thing this class
            // of bug can be filled WITH, for one clear per stage at allocation.
            // If the blue survives this, it is in the picture the guest drew and
            // the hunt moves inside the guest.
            if let cmd = queue?.makeCommandBuffer() {
                for slice in 0..<t.arrayLength {
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture     = t
                    pass.colorAttachments[0].slice       = slice
                    pass.colorAttachments[0].loadAction  = .clear
                    pass.colorAttachments[0].storeAction = .store
                    pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1)
                    cmd.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
                }
                cmd.commit()
            } else {
                NSLog("[cp] stage \(stage): no queue yet — the eye texture is "
                      + "handed over UNCLEARED, and anything sampled outside the "
                      + "guest's rendered region is recycled memory")
            }
            // The real cost, not the arithmetic one. Metal reports what it
            // actually reserved, so this is the only honest input to "is the
            // eye swapchain why we are at 3.5 GB".
            let naive = w * h * 8 * 2
            NSLog("[cp] stage \(stage): GL "
                  + String(format: "0x%x", format) + " \(w)x\(h) array, 2 slices — "
                  + "\(t.allocatedSize / (1024 * 1024)) MiB allocated "
                  + "(uncompressed would be \(naive / (1024 * 1024)) MiB), "
                  + "compression=\(t.compressionType == .lossy ? "lossy" : "lossless")")
        }

        eyesLock.lock()
        // Re-check the partner: allocating outside the lock means the other eye
        // may have published one meanwhile. Adopting it is what keeps both eyes
        // on ONE array texture — the composite drops a view outright if they
        // disagree — and the one we just made is simply released. In practice
        // ovrp_SetupEyeTexture2 arrives serially on the guest's render thread,
        // so this costs nothing and exists so that assumption is not load-bearing.
        partner = eyes[partnerKey]
        if let p = partner, fits(p) { tex = p.texture }
        let final = tex!
        eyes[k] = EyeAllocation(texture: final, slice: eye)
        eyesLock.unlock()

        out.pointee = kl_mtl_eye_texture(texture: Unmanaged.passUnretained(final).toOpaque(),
                                         slice: Int32(eye),
                                         w: Int32(final.width), h: Int32(final.height))
        return 1
    }

    /// Storage for a guest's non-eye LAYER. See `layers` above and kl_glfb.h.
    ///
    /// Simpler than the eye allocator in every respect that made that one
    /// complicated: nothing is shared between two callers, there is no slice and
    /// no rate map. What it keeps is the identity rule — size and format are
    /// part of it, so a guest that rebuilds its swapchain is not handed the
    /// previous allocation and left rendering into storage it does not have.
    private func provideLayerTexture(layer: Int, stage: Int, w: Int, h: Int,
                                     format: UInt32,
                                     out: UnsafeMutablePointer<kl_mtl_eye_texture>) -> Int32 {
        guard let pixelFormat = klEyePixelFormat(format) else {
            NSLog("[cp] layer \(layer) stage \(stage) asks for GL internalformat "
                  + String(format: "0x%x", format)
                  + ", which has no MTLPixelFormat here — declining, so the layer "
                  + "renders into GL storage nothing composites")
            return 0
        }
        let k = layer &* 64 &+ stage
        layersLock.lock()
        let cached = layers[k]
        layersLock.unlock()
        var tex = cached
        if tex == nil || tex!.width != w || tex!.height != h ||
           tex!.pixelFormat != pixelFormat {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: pixelFormat, width: w, height: h, mipmapped: false)
            desc.usage = [.renderTarget, .shaderRead]
            desc.storageMode = .private
            guard let t = device.makeTexture(descriptor: desc) else {
                NSLog("[cp] could not allocate layer \(layer) stage \(stage) \(w)x\(h)")
                return 0
            }
            t.label = "klepton layer \(layer) stage \(stage)"
            tex = t
            layersLock.lock()
            layers[k] = t
            layersLock.unlock()
            NSLog("[cp] layer \(layer) stage \(stage): GL "
                  + String(format: "0x%x", format) + " \(w)x\(h) — "
                  + "\(t.allocatedSize / (1024 * 1024)) MiB allocated")
        }
        let final = tex!
        out.pointee = kl_mtl_eye_texture(texture: Unmanaged.passUnretained(final).toOpaque(),
                                         slice: 0,
                                         w: Int32(final.width), h: Int32(final.height))
        return 1
    }

    // MARK: - The frame loop

    func startRenderLoop() {
        let t = Thread { self.renderLoop() }
        t.name = "Klepton Compositor"
        // The guest blocks — Baselib futexes, IL2CPP's GC suspending the world —
        // and this thread now calls into it, so it must not be the main thread.
        t.start()
    }

    /// KL_CP_PROBE=9 — the floor. Apple's template minus the cube: wait for the
    /// layer, then nothing but query / update / drawable / submit / clear /
    /// present, forever. No ARKit, no priming pass, no eye-texture provider, no
    /// guest, no shared-event wait, no pipeline, no poses, no anchor.
    ///
    /// Every rung above this one has come back clean while the display stayed
    /// black, which means the thing being bisected is not a feature of the pass.
    /// This establishes whether THIS APP can put any colour on THIS display at
    /// all. Blue here and black at rung 1 localises the fault to what rung 1
    /// still has that this does not; black here says the fault is structural —
    /// the scene, the configuration, or the layer — and nothing in renderFrame()
    /// was ever going to fix it.
    private func minimalLoop() {
        NSLog("[cp] PROBE 9: MINIMAL LOOP — clear to blue, present, nothing else")
        var n = 0
        var next = Date()
        while true {
            if layerRenderer.state == .invalidated { break }
            if layerRenderer.state == .paused { layerRenderer.waitUntilRunning(); continue }
            guard let frame = layerRenderer.queryNextFrame() else { continue }
            frame.startUpdate()
            frame.endUpdate()
            if let t = frame.predictTiming() {
                LayerRenderer.Clock().wait(until: t.optimalInputTime)
            }
            guard let drawable = frame.queryDrawables().first else { continue }
            frame.startSubmission()
            // deviceAnchor is deliberately LEFT ALONE. Stripping it would be
            // stripping something already measured as working (anchor=set), and
            // the system uses it to place and reproject the frame — so a black
            // result would no longer distinguish "structural fault" from "the
            // probe broke it". A floor test may remove only what is suspect.
            if let cmd = queue?.makeCommandBuffer() {
                for view in drawable.views {
                    let map = view.textureMap
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture     = drawable.colorTextures[map.textureIndex]
                    pass.colorAttachments[0].slice       = map.sliceIndex
                    pass.colorAttachments[0].loadAction  = .clear
                    pass.colorAttachments[0].storeAction = .store
                    pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 1, 1)
                    cmd.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
                }
                drawable.encodePresent(commandBuffer: cmd)
                cmd.commit()
            }
            frame.endSubmission()
            n += 1
            if Date() >= next {
                next = Date().addingTimeInterval(2)
                NSLog("[cp] minimal: \(n) frames presented, state=\(String(describing: layerRenderer.state))")
            }
        }
        NSLog("[cp] minimal loop ended after \(n) frames")
    }

    private func renderLoop() {
        layerRenderer.waitUntilRunning()
        if probe == 9 {
            // The system default device, exactly as Apple's template uses —
            // not ANGLE's. Nothing here samples a guest texture, so there is no
            // reason to involve the device that owns them, and every reason not
            // to: it is one of the things being ruled out.
            queue = MTLCreateSystemDefaultDevice()?.makeCommandQueue()
            minimalLoop()
            return
        }
        installProvider()
        applyRenderQuality()
        startARKit()

        // Before the guest, not after: the frustum and the display rate can
        // only be read from a drawable, and the guest reads both exactly once
        // during the lifecycle below. See primeDisplay().
        primeDisplay()

        // Bring the guest up, on its own thread. _begin is
        // once per process and does the /proc report, nativeRecreateGfxState,
        // nativeResume and the first nativeRender, and it happens over there
        // rather than here: this loop must be free to present black while the
        // guest is still starting, which can take tens of seconds on device.
        //
        // Under KL_SYNC_GUEST the old inline shape is kept instead, because
        // The device measurement was taken against it and the device has
        // never been run with either. It is the A/B for anything that looks
        // like a pacing regression.
        if syncGuest {
            NSLog("[cp] KL_SYNC_GUEST — driving the guest inline on this thread")
            if kl_app_lifecycle_begin() != 0 {
                NSLog("[cp] lifecycle_begin failed: \(String(cString: kl_app_status()))")
                return
            }
        } else if kl_app_guest_start() != 0 {
            NSLog("[cp] guest thread failed to start: \(String(cString: kl_app_status()))")
            return
        }

        // This loop's own liveness, logged on a wall clock rather than a frame
        // count. The device kills an immersive app that stops presenting, and
        // that kill arrives as a bare SIGKILL with no crash report at all — so
        // "was this loop still running, and was it producing anything" is a
        // question the log has to be able to answer after the fact. A frame
        // counter cannot answer it: a loop that has stopped iterating stops
        // incrementing, and the absence of a line is exactly what a healthy
        // quiet loop looks like too.
        var presented = 0
        var iterations = 0
        var nextReport = Date()
        // From here until the loop ends, this layer — not the app's scene
        // phase — says whether the game is audible and, for the native guest,
        // whether a backgrounded app may exit (KleptonScenePolicy). A closed
        // launcher window moves the phase; it does not stop this loop.
        let audioOwner = ObjectIdentifier(self)
        KleptonAudio.layerChanged(audioOwner, .running)
        loop: while true {
            switch layerRenderer.state {
            case .invalidated:
                break loop
            case .paused:
                NSLog("[cp] layer paused at iteration \(iterations) — waiting")
                // Off screen (crown press, Home): silent until it runs again.
                KleptonAudio.layerChanged(audioOwner, .paused)
                // The one flush point a closed window is guaranteed to deliver.
                // On device the exit report never runs — KL_EXIT_ON_BACKGROUND=0
                // is baked for these targets (the app must not exit when its
                // window closes) and a force quit is SIGKILL — so the eye-stage
                // association, the per-FBO draw census and the link ledger have
                // to reach the log while the process is still alive. This is the
                // moment the space is dismissed, and it IS observed.
                kl_glfb_live_census("layer paused")
                layerRenderer.waitUntilRunning()
                NSLog("[cp] layer running again")
                // ...and the audio, which does NOT come back on its own.
                //
                // This transition is the immersive space being temporarily
                // dismissed and restored — a Digital Crown press to passthrough,
                // the app's window being closed out from under it — and visionOS
                // silently stops calling CoreAudio's render callback across it:
                // no error, no interruption notification, and an output unit
                // that still reports itself started. It is the same OS bug ALVR
                // works around, and the same workaround (rebuild, do not
                // restart). kl_audio's own heartbeat finds it too, about a
                // second later; this is the half that knows the exact moment,
                // and a second of dropped music is the difference.
                //
                // Through the policy rather than kl_audio_resume() directly:
                // the pause above silenced deliberately, and resume() alone
                // would bounce off that latch. The policy lifts it and
                // rebuilds, on the main thread (setActive is an XPC round trip
                // and this thread has a 2 s no-frames kill). Only if the layer
                // really is running — waitUntilRunning also returns on
                // invalidation, and a blip of sound before the end is wrong.
                if layerRenderer.state == .running {
                    KleptonAudio.layerChanged(audioOwner, .running)
                }
                continue loop
            default:
                // How long ONE slot took, wall clock, outside in. This is the
                // measurement launch #19 did not have: visionOS kills an
                // immersive app whose layer renderer has gone 2.0 s without
                // submitting a drawable, the kill leaves NO crash report of any
                // kind, and the only trace is `wakeboardd: killing [pid]
                // because it hasn't been sending frames for 2.0s` in the system
                // log. Anything this loop calls that can block — a synchronous
                // XPC round trip most of all — is therefore a fatal bug, and
                // this is the number that names it while the process is still
                // alive to print it.
                let slotStart = CACurrentMediaTime()
                autoreleasepool { presented += renderFrame() }
                noteSlot(CACurrentMediaTime() - slotStart)
            }
            iterations += 1
            if Date() >= nextReport {
                nextReport = Date().addingTimeInterval(2)
                cmdLock.lock(); let done = cmdCompleted; cmdLock.unlock()
                NSLog("[cp] alive: \(iterations) iters, \(presented) with a picture, "
                      + "\(blackFrames) black, guest=\(kl_app_guest_state()), "
                      + "bail(frame/drawable/pipeline)="
                      + "\(bailNoFrame)/\(bailNoDrawable)/\(bailNoPipeline), "
                      // committed vs completed: if these diverge and stay
                      // diverged, the GPU is waiting on the guest's fence and
                      // nothing is being drawn at all.
                      + "cmdbuf \(done)/\(cmdCommitted) done"
                      + (noFence ? " (NOFENCE)" : ""))
                NSLog("[cp] \(cadenceSummary())")
                NSLog("[cp] \(stageSummary())")
                // ...and, on its own slower timer inside, the block the exit
                // report would have carried. Rate-limited in C
                // (KL_GLFB_CENSUS_SECS, default 15 s) so this call is a clock
                // read on all but one visit in eight.
                kl_glfb_live_census(nil)
            }
        }
        NSLog("[cp] render loop ended after \(presented) presented frames, "
              + "\(iterations) iterations, state=\(String(describing: layerRenderer.state))")
        // The crown press invalidates the layer with NO scenePhase change at
        // all (measured on device: none fires), and the system then takes
        // ~30-60 s to background the process — which is when Lifecycle exits.
        // Go silent the moment the picture is gone, not when the OS gets to
        // it; the guest may keep rendering until the join below.
        KleptonAudio.layerChanged(audioOwner, .ended)
        // This layer's ARKit session ends with it. Once per process this never
        // mattered (the process ended with the layer); with quit-to-launcher a
        // later Launch opens a NEW layer and a new compositor, and the old
        // session left running beside the new one stalled the new one's first
        // queryDeviceAnchor on the simulator past the 2 s no-frames kill.
        if arRunning {
            arSession.stop()
            arRunning = false
            NSLog("[cp] ARKit session stopped with the layer")
        }
        // Ends the guest's loop and joins it, so its report — which is the
        // lifecycle report, and belongs to the guest's end of the run rather than
        // to this one — has been written before this returns.
        if syncGuest { kl_app_lifecycle_report() } else { kl_app_guest_stop() }
    }

    /// What each swapchain stage actually holds, per report interval.
    ///
    /// The doubling that survived every pose fix alternates
    /// with the stage count, and the only thing that differs between one stage
    /// and N is *which texture this pass samples*. So the remaining hypothesis
    /// is not two positions but two **pictures**: one stage holding a stale or
    /// wrong-generation image while another holds the live one. That is immune
    /// to every pose measurement, which is why all four of them came back clean.
    ///
    /// Three independent numbers per stage, chosen so that each failure reads
    /// differently instead of all three looking like "doubling":
    ///
    ///   draws   how many times the GUEST bound this stage as its draw target,
    ///           straight off the GL thunks and through no bookkeeping of ours.
    ///           Frozen here = the guest stopped drawing this stage.
    ///   serial  the guest frame whose pose is filed against it. Frozen while
    ///           `draws` climbs = the association, not the picture.
    ///   tex     the MTLTexture the guest is bound to, from kl_glfb's own
    ///           record — compared against the one this pass samples. These are
    ///           set by the same call and can still diverge, because the
    ///           provider caches per (eye, stage) on size while Unity re-creates
    ///           the swapchain mid-run at two different sizes. A divergence here
    ///           is the whole hypothesis, and nothing else in the run would show
    ///           it: every count stays healthy while the compositor samples a
    ///           texture the guest has stopped writing to.
    /// The storage to sample for one (eye, stage), from whichever half of the
    /// seam produced it.
    ///
    /// Two halves, and they run in opposite directions. On the **GL** path this
    /// compositor ALLOCATES the eye texture and hands it to ANGLE through the
    /// provider, so `eyes` is the authority and the guest renders into
    /// something we already hold. On the **VULKAN** path there is no provider at
    /// all: MoltenVK has already backed the guest's eye VkImage with an
    /// MTLTexture of its own, and kl_vulkan publishes that one into kl_glfb's
    /// eye table (`kl_glfb_note_eye_mtl_texture`). Nothing here can allocate it,
    /// so nothing here can be asked for it — the only way to find it is to look
    /// it up.
    ///
    /// The table is checked FIRST, and that is not arbitrary: it is filled by
    /// both paths (the provider registers its allocation there too), so it is
    /// the one place that always describes the storage the guest is actually
    /// rendering into. `eyes` remains the fallback because it survives the gap
    /// between ovrp_DestroyEyeTexture and the next SetupEyeTexture2, where the
    /// table is deliberately empty and showing the last good frame beats showing
    /// black.
    private func eyeSource(_ eye: Int, _ stage: Int) -> EyeAllocation? {
        var slice: Int32 = 0
        if let t = kl_glfb_eye_mtl_texture(Int32(eye), Int32(stage), &slice) {
            let tex = Unmanaged<MTLTexture>.fromOpaque(t).takeUnretainedValue() as MTLTexture
            return EyeAllocation(texture: arrayView(tex), slice: Int(slice),
                                 flipY: kl_glfb_eye_mtl_origin_top_left(Int32(eye),
                                                                        Int32(stage)) != 0)
        }
        eyesLock.lock(); let a = eyes[Self.key(eye, stage)]; eyesLock.unlock()
        return a
    }

    /// Every shader in kl_reproject.c samples a `texture2d_array<float>`, and
    /// binding a non-array texture to that slot does not fail — it samples
    /// nothing useful, which composites as a FLAT COLOUR over storage that is
    /// full of picture. Until Open Brush nothing here could produce one: the
    /// provider below allocates `.type2DArray` explicitly, and BONELAB's Vulkan
    /// eye image carries two array layers because the eye IS the layer there. An
    /// OpenXR guest on Vulkan has one swapchain per eye, so MoltenVK hands back a
    /// plain `.type2D` and the assumption breaks.
    ///
    /// A view rather than a second pipeline: only the type is wrong, and one
    /// shader shared by both compositors is worth more than two that can drift.
    /// Cached, because a view per frame is an allocation per frame. If the view
    /// cannot be made it says so once and returns the original — which
    /// composites exactly as badly as before, but names the reason rather than
    /// leaving a flat colour to be read as a guest that drew nothing.
    private var arrayViews: [ObjectIdentifier: MTLTexture] = [:]
    private var saidNoArrayView = false

    private func arrayView(_ tex: MTLTexture) -> MTLTexture {
        if tex.textureType == .type2DArray { return tex }
        let key = ObjectIdentifier(tex)
        if let v = arrayViews[key] { return v }
        guard let v = tex.makeTextureView(pixelFormat: tex.pixelFormat,
                                          textureType: .type2DArray,
                                          levels: 0..<tex.mipmapLevelCount,
                                          slices: 0..<1) else {
            if !saidNoArrayView {
                saidNoArrayView = true
                NSLog("[cp] the eye texture is textureType \(tex.textureType.rawValue) "
                      + "and an array VIEW of it could not be made — the shaders "
                      + "sample texture2d_array, so this composites as a flat colour")
            }
            return tex
        }
        arrayViews[key] = v
        return v
    }

    private func stageSummary() -> String {
        var out = "stages:"
        var bad = false
        for s in 0..<Int(kl_ovrp_stage_count()) {
            var r = kl_ovrp_render_pose()
            let have = withUnsafeMutablePointer(to: &r) {
                kl_ovrp_stage_render_pose(Int32(s), $0) != 0
            }
            out += " s\(s) draws=\(kl_glfb_stage_draw_count(Int32(s)))"
            out += " serial=\(have ? String(r.serial) : "-")"
            for eye in 0..<2 {
                var slice: Int32 = 0
                let guestTex = kl_glfb_eye_mtl_texture(Int32(eye), Int32(s), &slice)
                // tryLock, never lock: this runs on the render loop and a
                // diagnostic that can block frame submission is a worse bug
                // than the one it is chasing. A contended read just prints "?".
                var mine: EyeAllocation? = nil
                var read = false
                if eyesLock.try() { mine = eyes[Self.key(eye, s)]; eyesLock.unlock(); read = true }
                let mineTex = mine.map { Unmanaged.passUnretained($0.texture).toOpaque() }
                // A nil guest texture is not a disagreement: between
                // ovrp_DestroyEyeTexture and the next SetupEyeTexture2 the slot
                // is deliberately empty, and this cache still holds the old
                // texture on purpose — compositing the last good frame through
                // a loading transition is better than a black one. Only two
                // *different* live textures mean the two sides have diverged.
                if read && guestTex != nil && guestTex != mineTex { bad = true }
                // Only the low bits: two pointers into the same heap differ
                // there, and a full 64-bit address per eye per stage makes the
                // line unreadable at the moment it matters most.
                let tag = guestTex.map { String(UInt(bitPattern: $0) & 0xffffff, radix: 16) } ?? "nil"
                out += " e\(eye)=\(tag)"
                if !read { out += "?" }
                else if guestTex != nil && guestTex != mineTex { out += "!=SAMPLED" }
            }
            out += " |"
        }
        // The association's own health, live. A device run normally ends by the
        // immersive space being invalidated rather than by the guest finishing,
        // so the end-of-run report is the one thing that often is not written —
        // and these four numbers are exactly what it would have carried.
        var dropped: UInt64 = 0, multi: UInt64 = 0, cross: UInt64 = 0, disagree: UInt64 = 0
        kl_ovrp_association_stats(&dropped, &multi, &cross, &disagree)
        if dropped != 0 || multi != 0 || cross != 0 || disagree != 0 {
            out += " assoc: \(dropped) drew nothing, \(multi) multi-stage,"
            out += " \(cross) off-thread, \(disagree) index disagreed"
        }
        return out + (bad ? "  <-- the guest and the composite are on DIFFERENT textures" : "")
    }

    private func renderFrame() -> Int {
        guard let frame = layerRenderer.queryNextFrame() else { bailNoFrame += 1; return 0 }
        frame.startUpdate()
        frame.endUpdate()

        // Wait for the input deadline before sampling the pose, so the pose the
        // guest renders with is as late as the compositor allows. This is the
        // pacing that replaces a free-running pump.
        let timing = frame.predictTiming()
        if let timing {
            LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        }
        // NOT `defer { frame.endSubmission() }`. Compositor Services treats
        // ending a submission that never presented a drawable as a client bug
        // and aborts the process:
        //
        //   __BUG_IN_CLIENT__  <- cp_frame_end_submission
        //                      <- $defer #1 in renderFrame()
        //
        // — SIGABRT on the compositor thread, 2026-08-07 on device, after
        // thousands of healthy frames. A defer is exactly wrong here: the whole
        // point of the early returns below is that this frame will NOT be
        // presented, and those are the frames that must not be ended. ALVR's
        // Renderer.swift and Apple's own template both simply abandon the frame
        // — no endSubmission on that path — and this now does the same.
        // ...and it is started AFTER the drawable is in hand, below — which is
        // the order Apple's template and ALVR both use:
        //
        //   queryNextFrame -> startUpdate/endUpdate -> predictTiming
        //     -> wait(optimalInputTime) -> queryDrawable -> startSubmission
        //     -> encode -> encodePresent -> commit -> endSubmission
        //
        // startSubmission must NOT be called before queryDrawable: the two
        // bail paths below would then abandon a submission they had already
        // started, which Compositor Services aborts on.

        // Poses first, then the frame that uses them.
        if let timing {
            let present = LayerRenderer.Clock.Instant.epoch
                .duration(to: timing.presentationTime).timeInterval
            updatePoses(at: present)
            // ...and the SAME instant to the guest. See
            // `pushPredictedDisplayTime` — off by default for launch #22.
            // The lead: in the async path this frame's drawable is NOT what the
            // guest is about to draw, so one display period is the honest
            // minimum; with KL_SYNC_GUEST the guest renders inline and it is 0.
            if pushPredictedDisplayTime {
                let lead = syncGuest ? 0 : max(displayPeriod, 0)
                kl_ovrp_set_predicted_display_time(present + monotonicOffset() + lead)
            }
        }

        // Offer the guest this frame's pose and carry straight on. The guest
        // wakes on its own thread, ticks the Choreographer,
        // calls nativeRender and renders into the eye textures the provider
        // handed it, and this loop does not wait for any of it. A guest that
        // keeps up finishes before the next publish and behaves exactly as the
        // synchronous version did; a guest that does not simply leaves the
        // stage record where it was, and the composite below reprojects the
        // picture already there against the newer pose. That case is the whole
        // reason the pass exists, and it was unreachable while the thing being
        // reprojected was the thing blocking this loop.
        if syncGuest {
            _ = kl_app_frame()
        } else if paceMayPublish() {
            kl_app_guest_publish()
            // A guest that has *ended* looks exactly like a guest that is
            // merely slow — both leave the stage record where it was and both
            // make `stale` climb. Say which, once, or the log below cannot be
            // read: the same numbers mean "reprojection is earning its keep"
            // in one case and "there is nothing left to reproject" in the other.
            if !loggedGuestEnd && kl_app_guest_state() < 0 {
                loggedGuestEnd = true
                NSLog("[cp] the guest thread has ended (\(String(cString: kl_app_status())))"
                      + " — everything from here is its last picture, reprojected")
            }
        }

        // ALL of them, not just the first. visionOS 26 hands out more than one
        // drawable while the render quality is moving between two resolutions —
        // the system crossfades them so the change is not visible — and a loop
        // that renders drawables[0] and presents nothing else shows a half-empty
        // frame for the whole transition. That is exactly the transition
        // maxRenderQuality = 1.0 provokes, so the two changes belong together;
        // queryDrawable() is deprecated in 26 in favour of this for the same
        // reason, and ALVR loops over the array likewise.
        let drawables = frame.queryDrawables()
        guard !drawables.isEmpty else { bailNoDrawable += 1; return 0 }
        guard let cmd = queue?.makeCommandBuffer(), pipeline != nil, sampler != nil else {
            bailNoPipeline += 1
            return 0
        }
        // Everything above may abandon the frame; from here it will be
        // presented, so the submission can be opened.
        frame.startSubmission()
        let main = drawables[0]

        // Which stage holds a *finished* picture. -1 means the guest has not
        // completed a frame yet (ovrp_EndFrame never seen) — there is nothing
        // to show, but the drawable still has to be presented or Compositor
        // Services stalls waiting for it.
        //
        // Sampling stage 0 anyway would be worse than showing black: with the
        // guest decoupled it may well be *mid-draw* into exactly that texture,
        // so the frames before the first EndFrame would be a torn picture
        // rather than an empty one, and reprojected against no pose at all.
        //
        // KL_PRESENT_ORDER=strict (default) takes the stage and the record as
        // ONE selection, out of the lock the committer holds
        // (kl_ovrp_last_complete_record). `off` is the pre-round-5 arm: ask for
        // the stage, then ask what is in it — two lock acquisitions with the
        // guest's GPU fence thread free to commit later frames in between, so
        // the record that comes back can be a frame the stage index never named.
        // That is how a present jumps ahead of the true newest and the NEXT
        // present comes back to it: serials out of order, every per-frame
        // instrument clean, and the world alternating between two positions one
        // guest frame of head rotation apart.
        var rendered = kl_ovrp_render_pose()
        var haveRendered: Bool
        let completeStage: Int
        if Self.presentOrderStrict {
            haveRendered = withUnsafeMutablePointer(to: &rendered) {
                kl_ovrp_last_complete_record($0) != 0
            }
            completeStage = haveRendered ? Int(rendered.stage)
                                         : Int(kl_ovrp_last_complete_stage())
        } else {
            completeStage = Int(kl_ovrp_last_complete_stage())
            let s = max(0, completeStage)
            haveRendered = withUnsafeMutablePointer(to: &rendered) {
                kl_ovrp_stage_render_pose(Int32(s), $0) != 0
            }
        }
        // `complete` is redundant today and will not be: with one stage, the
        // guest's *next* ovrp_BeginFrame overwrites the record this one is
        // reading, so the moment the guest stops being driven synchronously
        // from this thread a record can describe a frame still being drawn.
        // Reprojecting against a pose whose picture does not exist yet is a
        // silent wrong answer, so it is checked now rather than after the
        // change that makes it possible.
        if haveRendered && rendered.complete == 0 { haveRendered = false }

        // **Never go backwards.** A serial older than the one already on the
        // panel is a frame the viewer has seen the future of; showing it puts
        // the world back where it was one guest frame ago, which under head
        // motion is a jump and under a repeating cadence is a shake. Under
        // `strict` the previous selection is shown again instead — a repeat,
        // which reprojection already handles and the eye reads as smooth.
        if Self.presentOrderStrict, haveRendered,
           let held = lastPresentedRecord, rendered.serial < held.serial {
            rendered = held
            presentHeldBack += 1
        }
        // The stage to sample the TEXTURE from comes off the record itself, not
        // from a separately-read index: the pair is only a pair if both halves
        // name the same frame.
        let stage = max(0, haveRendered ? Int(rendered.stage) : completeStage)

        // Order this pass after the guest's rendering. The value only advances
        // when the guest swaps, so an unchanged one also means "no new picture"
        // — which is not an error, it is the case reprojection is for.
        // Which guest frame this composite would show. Two sources, one
        // quantity: ANGLE signals our MTLSharedEvent from inside its own
        // command stream, and a VULKAN guest has no such event — kl_vulkan
        // makes the guest's queue idle at ovrp_EndFrame4 and publishes a serial
        // instead. `guestFrameEvent` being nil on that path is what keeps the
        // wait below from being encoded against a value nothing ever signals,
        // which would be a command buffer that is committed and never executes.
        // The guest's projection layers, snapshotted BEFORE the fence below is
        // read, and that order is the whole of it: the guest publishes its
        // frame's completion before the records that describe it
        // (klxr_EndFrame), so a list read first can only be OLDER than the
        // fence read second — never newer, which would be drawing layers whose
        // blits this pass never waited for. Seen as one frame of the display
        // showing a picture from several frames ago, a few seconds apart.
        projLayerCount = projLayers.withUnsafeMutableBufferPointer {
            kl_ovrp_eye_layer_live() != 0
                ? Int(kl_ovrp_proj_layers_snapshot($0.baseAddress, Int32($0.count)))
                : 0
        }
        let glFence = kl_glfb_gpu_fence_value()
        let frameValue = glFence != 0 ? glFence : kl_vulkan_frame_serial()
        // KL_CP_NOFENCE=1 skips the wait. A composite command buffer that waits
        // on an event value the guest's queue never signals is COMMITTED and
        // never EXECUTES: the drawable is presented, every counter here looks
        // healthy, and the display shows nothing. That failure is invisible
        // from this side, so it needs its own A/B — and cmdCompleted below is
        // the measurement that says whether it is happening.
        if let ev = guestFrameEvent, glFence != 0, !noFence {
            cmd.encodeWaitForEvent(ev, value: glFence)
        }
        if frameValue != 0 && frameValue == lastGuestFrame {
            staleInARow += 1
        } else {
            staleInARow = 0
            lastGuestFrame = frameValue
        }
        // **Is the fence actually moving?** It is the only thing ordering this
        // composite against ANGLE's queue, and a FROZEN one is silent: the wait
        // is satisfied for ever, the drawable is presented, every counter here
        // reads clean, and the eye texture is sampled while the guest may be
        // part-way through redrawing it. That was true for the whole of the r16
        // run — `kl_glfb_present` was gated on "the guest has never swapped",
        // and this guest swaps exactly once at bring-up (kl_ovrp.c's
        // klovrp_EndFrame4). The only trace was `stale in a row` climbing to
        // 10,767 of 10,857 iterations, which was read as an instrument fault.
        //
        // So it is a line now, and it is one-shot on each side: the value's
        // first advance says the ordering is live, and its failure to advance
        // says the composite is unordered — which is a picture made of two
        // guest frames, not a missing screenshot.
        if !loggedFenceVerdict, guestFrameEvent != nil, !noFence, haveRendered {
            if fenceWatchFrames == 0 { fenceAtStart = glFence }
            fenceWatchFrames += 1
            if glFence > fenceAtStart {
                loggedFenceVerdict = true
                NSLog("[cp] the guest's GPU fence is advancing (\(fenceAtStart) "
                      + "-> \(glFence) over \(fenceWatchFrames) composites) — "
                      + "the composite waits for ANGLE before it samples the "
                      + "eye texture")
            } else if fenceWatchFrames > 600 {
                loggedFenceVerdict = true
                NSLog("[cp] *** the guest's GPU fence has NOT advanced in 600 "
                      + "composites (stuck at \(glFence)) — the composite is "
                      + "UNORDERED against the guest's rendering and the panel "
                      + "may be showing two guest frames at once. See "
                      + "klovrp_EndFrame4's swap gate.")
            }
        }

        // The pose the picture will be *shown* at, as freshly predicted as we
        // can make it: re-queried here rather than reused from updatePoses,
        // because the guest's render has happened in between and the prediction
        // is that much better for it. This is what the pass reprojects to, and
        // therefore also what the drawable is told it was rendered with.
        let presentation = LayerRenderer.Clock.Instant.epoch
            .duration(to: main.frameTiming.presentationTime).timeInterval
        let displayAnchor = displayAnchor(at: presentation)
        let displayFromOrigin = originFromDevice(displayAnchor)

        // WHO DOES THE WARP — KL_WARP=ours (default here) or KL_WARP=system.
        //
        // There are two reprojections on this display and only one of them is
        // ours. The system warps whatever we present from the drawable's
        // `deviceAnchor` to the pose the hardware reports at scanout; our pass
        // corrects from the guest's render pose to the pose we predict for
        // presentation. Telling the system the DISPLAY pose (what this loop did
        // from the beginning) declares "this frame is already correct for
        // presentation", so its own correction is a no-op and ours is the whole
        // of it — the guest's picture is warped once, by a rotation-only pass
        // over a flat proxy, and any inexactness in that pass shows up scaled by
        // the correction magnitude. Correction magnitude ALTERNATES by
        // construction: a freshly-rendered showing needs almost none and a held
        // showing needs a full guest period's worth, so a 72 Hz guest on a
        // 120 Hz panel produces a repeating small/large/small cadence. An error
        // proportional to it is an image alternating between two positions,
        // which is the symptom this hunt is chasing.
        //
        // `system` hands the system the anchor the pixels ACTUALLY embody — the
        // one cached at this frame's pose freeze — and neutralises our own
        // correction by reprojecting towards that same pose (the model-view then
        // reduces to the display's per-eye view rotation, which is a property of
        // the panel and not a delta). The system then does the one and only
        // warp, from the true render pose, with the machinery that kept the ETT
        // test lattice welded through corrections that shook the game's content.
        //
        // The fallback is `ours` in all but name: no cached anchor (the guest's
        // frame predates the first push, or the tag has rolled out of the ring)
        // means the display anchor and the correction we always did.
        let renderAnchor = Self.warpBySystem && haveRendered
            ? poseAnchor(tag: rendered.pose_tag) : nil
        let drawableAnchor = renderAnchor ?? displayAnchor
        let warpOriginFromDevice = renderAnchor?.originFromAnchorTransform ?? displayFromOrigin
        if Self.warpBySystem && !loggedWarpArm && haveRendered {
            loggedWarpArm = true
            NSLog("[cp] KL_WARP=system: the drawable is stamped with the pose "
                  + "freeze's own anchor and our model-view is neutral — the "
                  + "SYSTEM does the one warp. Frame \(rendered.serial) tag "
                  + "\(rendered.pose_tag) -> \(renderAnchor == nil ? "NO CACHED ANCHOR (falling back to the display anchor, i.e. our warp)" : "cached anchor")")
        }

        // OpenXR's VIEW space is DEFINED as the midpoint between the eyes.
        // visionOS's device anchor is a point on the hardware and is not that
        // midpoint, so treating the two as equal declares an equality that is
        // false at both seams at once: the guest is told its head is somewhere
        // it is not, and the timewarp rotates about a centre the picture was
        // not drawn about. The second one is the expensive half — a rotation
        // applied about the wrong centre moves the eyes along a lever arm
        // nothing models, so a pure head turn smears near content while the
        // measured delta stays small and healthy-looking.
        //
        // The drawable states the midpoint every frame and it needs no tuning:
        // it is the average of the two eyes' own transforms. `deviceAnchor`
        // below still gets the raw anchor, which is what that API means.
        let originFromHead = Self.headFrom(warpOriginFromDevice, midpoint: eyeMidpointInDevice)

        // What this present will actually put on the panel — the round-5
        // measurement. Here rather than at encodePresent because this is where
        // all three halves of the tuple are in hand at once: the record, the
        // stage whose texture will be bound, and the anchor the drawable will
        // be stamped with.
        // `bridged`: the guest published no new picture for this slot, so what
        // is about to be presented is the content already in hand, reprojected
        // to a fresh pose. `staleInARow` was set above from exactly that
        // comparison; a slot with no record at all is bridged too — there is
        // nothing newer by definition.
        notePresentOrder(rendered: haveRendered ? rendered : nil, stage: stage,
                         anchorHit: renderAnchor != nil,
                         bridged: !haveRendered || staleInARow > 0)

        // Measure the pose the composite is about to DRAW with, which is not
        // always the frame record. With the layer path live the eye picture is
        // placed by the guest's own submitted layer pose; the frame record is
        // the head pose we latched when the guest opened the frame. Those are
        // different poses, and reporting the second while drawing the first
        // describes a correction nobody is applying — the delta reads healthy
        // while the picture swims.
        var measured = haveRendered ? rendered : nil
        if projLayerCount > 0 {
            var r = kl_ovrp_render_pose()
            r.serial = rendered.serial
            r.submit_ns = rendered.submit_ns
            let pl = projLayers[0]
            r.px = pl.pose.0; r.py = pl.pose.1; r.pz = pl.pose.2
            r.qx = pl.pose.3; r.qy = pl.pose.4; r.qz = pl.pose.5; r.qw = pl.pose.6
            measured = r
        }
        // Always against the DISPLAY pose, whichever arm is running: this is the
        // instrument that says how big the corrections are, and under `system`
        // our own delta is zero by construction — reporting that would replace
        // the measurement with a tautology.
        noteReprojection(rendered: measured,
                         originFromHead: Self.headFrom(displayFromOrigin,
                                                       midpoint: eyeMidpointInDevice),
                         stage: stage)

        // The drawable's depth range, once, with the depth our quad actually
        // reaches. This is not curiosity. The quad's distance is what the
        // system's own reprojection is told about our content, so it decides
        // how much parallax gets applied to a picture that has none — and the
        // one time it was chosen from a measurement, the measurement had two
        // variables in it and the wrong one got the blame. Both numbers, side
        // by side, so the next person does not have to take anyone's word.
        if !loggedDepthRange {
            loggedDepthRange = true
            let drawable = main
            let r = drawable.depthRange
            // The quad's ACTUAL clip-space depth, against the projection this
            // drawable really handed us — not the distance in metres, which
            // says nothing without the projection. In reverse-Z anything in
            // [0,1] is inside the clip range; a value near 0 means "far away",
            // which is what we want the system's own reprojection to believe
            // and is NOT the same thing as being clipped. `far = inf` above is
            // what makes that safe. Printed because this exact question was
            // argued about and settled wrongly once (kl_reproject_build).
            // No recorded pose: the depth does not depend on one, and asking
            // with nil is the case kl_reproject_build already handles.
            var probe = kl_reproject_build(nil, 0, matrix_identity_float4x4,
                                           drawable.views[0].transform,
                                           drawable.computeProjection(viewIndex: 0), 0, 0, 0)
            let ndcZ = withUnsafePointer(to: &probe) { kl_reproject_ndc_depth($0) }
            NSLog(String(format: "[cp] drawable depthRange = %@ (x=far, y=near in "
                                 + "reverse-Z); quad at %.1f m lands at NDC z %.6f "
                                 + "(%@)", "\(r)", probe.depth, ndcZ,
                         (ndcZ >= 0 && ndcZ <= 1) ? "inside the clip range"
                                                  : "*** OUTSIDE THE CLIP RANGE ***"))
            // The drawable's actual shape, once. A clear fills the whole
            // attachment whatever the viewport says, so a blue clear that is
            // not visible means the texture being cleared is not the texture
            // being displayed — and these are the numbers that say which.
            NSLog("[cp] \(drawables.count) drawable(s); [0]: "
                  + "\(drawable.colorTextures.count) color texture(s), "
                  + "\(drawable.views.count) view(s), layout="
                  + (layerRenderer.configuration.layout == .layered ? "layered" : "dedicated")
                  + ", render quality \(layerRenderer.renderQuality) of max "
                  + "\(layerRenderer.configuration.maxRenderQuality)")
            // Whose device owns what. installProvider() takes ANGLE's MTLDevice
            // so the eye textures can be shared with the guest, and its own
            // comment notes that on the host ANGLE's device and the system
            // default "happen to be the same object, so testing on the host
            // would not catch getting this wrong". Compositor Services allocates
            // the drawable on ITS device. If those are two different objects,
            // every render pass here attaches textures the command queue does
            // not own — which is not a picture that comes out wrong, it is a
            // picture that never happens.
            if let dt = drawable.colorTextures.first?.device {
                let ours = queue?.device
                NSLog("[cp] devices: queue=\(ours?.name ?? "nil")/"
                      + "\(ours.map { String($0.registryID) } ?? "-") "
                      + "drawable=\(dt.name)/\(dt.registryID) "
                      + (ours === dt ? "SAME OBJECT" : "*** DIFFERENT OBJECTS ***"))
            }
            for (i, t) in drawable.colorTextures.enumerated() {
                NSLog("[cp]   color[\(i)] \(t.width)x\(t.height) "
                      + "array=\(t.arrayLength) type=\(t.textureType.rawValue) "
                      + "fmt=\(t.pixelFormat.rawValue)")
            }
            for (i, v) in drawable.views.enumerated() {
                let m = v.textureMap
                NSLog("[cp]   view[\(i)] -> texture \(m.textureIndex) slice "
                      + "\(m.sliceIndex) viewport \(m.viewport)")
            }
            // The rate map is the whole point of foveation, and the two numbers
            // it carries are the ones to size the GUEST's eye textures against:
            // `screenSize` is the resolution the picture is rasterized at in
            // the fovea, `physicalSize` is what is actually stored. Rendering
            // the guest at much less than the screen size throws away the
            // resolution maxRenderQuality just paid for; much more is wasted.
            // (ALVR measures 4338x3478 screen / 1888x1792 physical at the
            // default quality, 6262x5020 / 2496x2432 at 1.0.)
            if let rm = drawable.rasterizationRateMaps.first {
                var line = "[cp] rate map: screen \(rm.screenSize.width)x\(rm.screenSize.height), "
                         + "\(rm.layerCount) layer(s)"
                for l in 0..<rm.layerCount {
                    let p = rm.physicalSize(layer: l)
                    line += ", physical[\(l)] \(p.width)x\(p.height)"
                }
                NSLog("%@", line)
            } else {
                NSLog("[cp] no rasterization rate map — foveation is off, the "
                      + "drawable is rasterized uniformly")
            }
        }

        // The guest's stereo separation, refreshed from the display itself.
        pushEyeOffsets(main)

        var encoded = 0
        for drawable in drawables {
            // Per drawable, not once: each one is presented separately and each
            // is reprojected by the system against its own anchor.
            drawable.deviceAnchor = drawableAnchor
            encoded += encodeComposite(drawable, cmd: cmd, stage: stage,
                                       completeStage: completeStage,
                                       rendered: rendered, haveRendered: haveRendered,
                                       originFromHead: originFromHead)
            drawable.encodePresent(commandBuffer: cmd)
        }

        // The one thing worth saying out loud about the decoupling: how long
        // the guest took to put anything in a drawable. Under the old inline
        // shape this could not be measured, because the loop did not run until
        // the guest had already produced its first frame.
        if encoded == 0 {
            blackFrames += 1
        } else if !loggedFirstPicture {
            loggedFirstPicture = true
            NSLog("[cp] first guest picture composited, after \(blackFrames) black frames")
        }

        // Did this command buffer actually RUN? "Presented" only means committed,
        // and a buffer stalled on a shared-event wait never executes — so a
        // black display and a perfect frame count are the same picture from
        // here. Counted, with the first error printed, because "committed but
        // never completed" is otherwise indistinguishable from "completed and
        // drew black".
        cmd.addCompletedHandler { [weak self] b in
            // **The composite's read of this stage is over.** Stamped here and
            // not at commit, because a commit is not a read: the question round
            // 13 asks is how long the guest had before it overwrote the texture
            // we were sampling, and the answer ends when the GPU pass does. One
            // relaxed store; the runtime never reads the value back, it only
            // subtracts it at the next file of this stage. Riding a handler
            // that already existed is what makes it free. See
            // klovrp_margin_note_file for how to read the distribution.
            kl_ovrp_note_stage_read_done(Int32(stage))
            guard let self else { return }
            self.cmdLock.lock()
            self.cmdCompleted += 1
            let first = self.cmdError == nil && b.error != nil
            if first { self.cmdError = b.error }
            self.cmdLock.unlock()
            if first { NSLog("[cp] composite command buffer error: \(b.error!)") }
        }
        cmd.commit()
        cmdCommitted += 1
        noteCadence(presentation: presentation, drawable: main)
        frame.endSubmission()      // only here — see startSubmission above
        return encoded > 0 ? 1 : 0
    }

    /// One drawable, every eye it carries. Returns how many eyes got a picture.
    ///
    /// **Both eyes go through ONE render pass now, and that is what foveation
    /// costs.** A foveated drawable comes with a rasterization rate map, and
    /// Metal applies that map's *layer N* to render-target array index N. A
    /// pass that selects one slice with `colorAttachments[0].slice` has a
    /// render-target array length of 1, so it can only ever reach the map's
    /// layer 0 — the right eye would be rasterized through the left eye's
    /// foveation pattern, and the system's unwarp would then be undoing a
    /// distortion that was never applied. So the layered path sets
    /// `renderTargetArrayLength = views.count` and draws both eyes in one draw
    /// call with vertex amplification, picking its per-eye uniforms by
    /// `[[amplification_id]]`. Apple's template and ALVR are both this shape.
    ///
    /// The `.dedicated` layout keeps the old per-view pass, with that view's
    /// own single-layer rate map, because there is nothing to amplify.
    /// KL_CP_AMPLIFY, read once: encodeComposite runs per drawable per frame,
    /// and klEnvOn rebuilds the whole environment dictionary on every call.
    private static let amplifyOn = klEnvOn("KL_CP_AMPLIFY", default: true)

    private func encodeComposite(_ drawable: LayerRenderer.Drawable,
                                 cmd: MTLCommandBuffer, stage: Int, completeStage: Int,
                                 rendered: kl_ovrp_render_pose, haveRendered: Bool,
                                 originFromHead: simd_float4x4) -> Int {
        // KL_CP_AMPLIFY=0 — back to ONE RENDER PASS PER EYE, selecting the
        // slice on the attachment, with no rate map and no amplification.
        //
        // That is exactly the composite that existed before this session, and
        // it is the A/B that has been missing: every fix stacked on the
        // doubling so far has assumed the bug predates the amplified pass, and
        // nothing has tested that assumption. If both amplifications were
        // landing on array index 0 and viewport 0 — the failure mode of a view
        // mapping that is not being applied — then both eyes' quads are drawn
        // over each other into one slice, which is two overlapping images and
        // would be indifferent to the cant, the pose source and the swapchain
        // alike. Take this rung first; it splits "regression from this session"
        // from "pre-existing", and nothing else does.
        let layered = layerRenderer.configuration.layout == .layered
                      && drawable.views.count > 1
                      && amplification >= drawable.views.count
                      && Self.amplifyOn
        if layered {
            return encodeViews(Array(drawable.views.indices), drawable: drawable,
                               layered: true,
                               rateMap: drawable.rasterizationRateMaps.first,
                               cmd: cmd, stage: stage, completeStage: completeStage,
                               rendered: rendered, haveRendered: haveRendered,
                               originFromHead: originFromHead)
        }
        var n = 0
        let maps = drawable.rasterizationRateMaps
        // One map per view, or none. A LAYERED drawable has a single map whose
        // layers are the eyes, and a pass that selects one slice can only ever
        // reach layer 0 — so handing it that map rasterizes the right eye
        // through the left eye's foveation pattern. Better no map at all.
        let perView = maps.count == drawable.views.count
        for i in drawable.views.indices {
            n += encodeViews([i], drawable: drawable, layered: false,
                             rateMap: perView ? maps[i] : nil,
                             cmd: cmd, stage: stage, completeStage: completeStage,
                             rendered: rendered, haveRendered: haveRendered,
                             originFromHead: originFromHead)
        }
        return n
    }

    /// A group of views sharing one attachment, and therefore one render pass.
    private func encodeViews(_ viewIndices: [Int],
                             drawable: LayerRenderer.Drawable,
                             layered: Bool,
                             rateMap: MTLRasterizationRateMap?,
                             cmd: MTLCommandBuffer, stage: Int, completeStage: Int,
                             rendered: kl_ovrp_render_pose, haveRendered: Bool,
                             originFromHead: simd_float4x4) -> Int {
        guard let first = viewIndices.first else { return 0 }
        let firstMap = drawable.views[first].textureMap

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture     = drawable.colorTextures[firstMap.textureIndex]
        pass.colorAttachments[0].loadAction  = .clear
        pass.colorAttachments[0].storeAction = .store
        // Rung 1 clears to blue instead of black and draws nothing at all:
        // if the display stays black through THAT, nothing this file draws
        // was ever going to be seen and the fault is below the pass.
        // Rung 1's colour CYCLES — red, green, blue, one second each — and
        // that is the point. A static colour cannot distinguish "our clear
        // is reaching the display" from "the display is showing black for
        // its own reasons", because both look like a constant field. A
        // colour that changes on a known period can: if it cycles, our
        // content is being composited and the question moves to what we
        // draw; if it is a steady anything, it is not ours at all.
        pass.colorAttachments[0].clearColor  = probe == 1
            ? Self.cyclingClear() : MTLClearColorMake(0, 0, 0, 1)
        if firstMap.textureIndex < drawable.depthTextures.count {
            pass.depthAttachment.texture     = drawable.depthTextures[firstMap.textureIndex]
            pass.depthAttachment.loadAction  = .clear
            // .store, NOT .dontCare. visionOS reprojects the submitted frame
            // using its depth buffer, so throwing the depth away hands the
            // compositor a colour image with no geometry — and a reverse-Z
            // clear of 0 reads as "everything is infinitely far away".
            // .dontCare here meant the quad's colour was correct and its
            // depth said nothing was there.
            pass.depthAttachment.storeAction = .store
            pass.depthAttachment.clearDepth  = 0.0
        }
        if layered {
            // The array length, not a slice: this is what lets the rate map's
            // per-eye layers be reached at all (see encodeComposite).
            pass.renderTargetArrayLength = viewIndices.count
        } else {
            pass.colorAttachments[0].slice = firstMap.sliceIndex
            if pass.depthAttachment.texture != nil {
                pass.depthAttachment.slice = firstMap.sliceIndex
            }
        }
        // Foveation. Metal rasterizes into the (smaller) physical texture and
        // the system unwarps on the way to the display; a pass that leaves this
        // nil while the drawable is foveated fills the physical texture
        // uniformly and the unwarp then distorts a correct picture.
        pass.rasterizationRateMap = rateMap

        guard let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { return 0 }
        enc.label = "klepton composite view\(viewIndices)"
        if probe == 1 { enc.endEncoding(); return viewIndices.count }
        guard let pipeline, let sampler else { enc.endEncoding(); return 0 }

        // One uniform per view, in the order the viewports and the amplification
        // mappings are given — the vertex shader indexes this array by
        // [[amplification_id]].
        var uniforms: [kl_reproject_uniforms] = []
        // Which texture each view samples, in the PASS's own order — index n
        // here is the [[amplification_id]] the vertex shader reads, so it is
        // parallel to `uniforms` and must be appended to on every path through
        // the loop, including the ones that skip a view.
        var sources: [MTLTexture?] = []
        var visible = 0
        for vi in viewIndices {
            let view = drawable.views[vi]
            let a = eyeSource(vi, stage)
            // No eye texture yet — the guest is still in _begin, or it has not
            // reached ovrp_SetupEyeTexture2 for this stage. The pass still runs,
            // because the clear above is the point: a drawable that is presented
            // without being written shows whatever was in that texture last, and
            // with the guest taking its own time to start there are a great many
            // such frames.
            // ...and nil again when the guest's last frame carried no eye
            // picture at all. The eye textures outlive the frame that filled
            // them, so without this the eye keeps being drawn after the guest
            // has stopped submitting a projection layer — and on a guest that
            // alternates (JKXR: a projection pair in the world, a quad in the
            // menu, out of the SAME swapchains) that is one screen composited
            // twice, full-field underneath its own panel. kl_ovrp.h has the
            // measurement.
            let alloc = (completeStage < 0 || kl_ovrp_eye_layer_live() == 0) ? nil : a
            var u = withUnsafePointer(to: rendered) { r in
                kl_reproject_build(haveRendered ? r : nil, Int32(vi),
                                   originFromHead,
                                   Self.headFromView(view.transform,
                                                     midpoint: eyeMidpointInDevice),
                                   drawable.computeProjection(viewIndex: vi),
                                   UInt32(alloc?.slice ?? 0),
                                   (alloc?.flipY ?? false) ? 1 : 0,
                                   // Shared grid for now; the block per eye is
                                   // assigned below, once the grid is known.
                                   0)
            }
            // KL_CP_EYE=<0|1> — composite ONLY that eye and leave the other
            // cleared. The measurement this whole hunt has been missing: it
            // separates BINOCULAR doubling (the two eyes disagree and cannot be
            // fused — the failure every fix so far has assumed) from TEMPORAL
            // doubling (one eye seeing two copies over time, i.e. a frame held
            // across display periods, which no amount of per-eye correctness
            // touches). If the doubling survives with one eye blanked, it is
            // temporal and everything tried so far was aimed at the wrong half.
            if let only = Self.onlyEye, only != vi {
                u.visible = 0
                uniforms.append(u)
                sources.append(nil)
                continue
            }
            u.visible = alloc == nil ? 0 : 1
            uniforms.append(u)
            sources.append(alloc?.texture)
        }

        // The two texture slots the shader has, and which view reaches which.
        //
        // This used to bind ONE texture and drop any view whose eye was not
        // identical to it, on the reasoning that both eyes share a single
        // 2-slice array. They do for every guest that goes through the eye
        // provider, which allocates exactly that, and for a Vulkan guest whose
        // eye IS the layer. They do NOT for an OpenXR guest on Vulkan: one
        // swapchain per eye means two unrelated MTLTextures, and the right eye
        // was dropped from the composite for the whole run — the log said so
        // and nothing joined it to a person seeing one eye go dark. Open Brush
        // is the first guest of that shape.
        //
        // So the shader takes two textures and each view says which is its own
        // (kl_reproject.c, `src`). View 0 samples slot 0 and every other view
        // samples slot 1, which is the two eyes; when they really do share one
        // texture it is bound twice and this reduces to what it was, bit for
        // bit, for every guest that has run here.
        let bind0 = sources.first.flatMap { $0 } ?? sources.compactMap { $0 }.first
        let bind1 = (sources.count > 1 ? sources[1] : nil) ?? bind0
        // No eye texture is not the same as no picture, and this used to return
        // here as if it were. A guest can present its whole frame as an OpenXR
        // QUAD layer and submit no projection layer at all — JKXR does, every
        // frame — so the eye pass is skipped and the overlay pass at the end of
        // this function is the composite. Everything between is the eye's.
        let haveEyes = bind0 != nil && bind1 != nil
        if !haveEyes {
            for n in uniforms.indices { uniforms[n].visible = 0 }
        }
        for n in uniforms.indices where haveEyes && uniforms[n].visible != 0 {
            // What the fragment stage will actually sample for this view. A
            // third distinct texture cannot be named — there are two slots —
            // so it is dropped rather than shown another eye's picture, which
            // is the one thing the old gate got right.
            if sources[n] === (n == 0 ? bind0 : bind1) {
                visible += 1
            } else {
                uniforms[n].visible = 0
                if !loggedSplitEyes {
                    loggedSplitEyes = true
                    NSLog("[cp] view \(viewIndices[n]) samples a third distinct "
                          + "eye texture at stage \(stage) — dropped from the "
                          + "composite (the shader binds two)")
                }
            }
        }
        // The viewport matters here in a way it did not for a full-screen
        // triangle: the quad is projected, so it lands where the projection
        // says, and the view's own bounds within a shared texture are part of
        // that. With amplification there is one per view, selected by the
        // mapping below rather than by the shader.
        enc.setViewports(viewIndices.map { drawable.views[$0].textureMap.viewport })
        if layered && viewIndices.count > 1 {
            var mappings = viewIndices.enumerated().map { (n, vi) in
                MTLVertexAmplificationViewMapping(
                    viewportArrayIndexOffset: UInt32(n),
                    renderTargetArrayIndexOffset: UInt32(drawable.views[vi].textureMap.sliceIndex))
            }
            enc.setVertexAmplificationCount(viewIndices.count, viewMappings: &mappings)
        }

        // The PER-LAYER composite, when the guest's last frame described its
        // projection layers one by one. It replaces everything the eye pass
        // does below — the layers ARE the eye picture — so `visible` comes from
        // it and the eye branch is skipped entirely. Zero is every guest that
        // files no list, which is every guest but Steam Link and every run
        // under KL_XR_LAYERS=0, and those take the eye pass unchanged.
        let nproj = projLayerCount
        if nproj > 0 {
            visible = encodeProjLayers(enc, viewIndices: viewIndices,
                                       drawable: drawable,
                                       originFromHead: originFromHead,
                                       count: nproj) > 0 ? viewIndices.count : 0
        }
        // The EYE pass, and only when there is an eye. `haveEyes` is false for
        // a guest whose whole picture is a composition layer (see above), and
        // the overlay pass below is then the entire composite rather than a
        // decoration on top of one.
        if nproj == 0, visible > 0, let bind0, let bind1 {
            if !loggedEyeTextures {
                loggedEyeTextures = true
                NSLog("[cp] eye textures: slot0 \(bind0.width)x\(bind0.height) "
                      + "type \(bind0.textureType.rawValue), slot1 "
                      + (bind1 === bind0 ? "= slot0 (the eyes share one texture)"
                                         : "\(bind1.width)x\(bind1.height) "
                                           + "type \(bind1.textureType.rawValue) "
                                           + "(one swapchain per eye)"))
            }

            // What each eye is ACTUALLY being placed with, said once per pass shape.
            //
            // This exists because a stereo geometry error has no error surface at
            // all: every call succeeds, the counters stay healthy, and the only
            // instrument is a person wearing the headset saying one eye looks wrong
            // — which cannot say WHICH of three things is wrong. The three are
            // separable from these numbers alone:
            //
            //   * `slice` — eye 1 sampling slice 0 is the right eye showing the
            //     LEFT eye's picture. Placed with eye 1's quad, that is a ~20 degree
            //     horizontal shift, because the display's per-eye tangents are
            //     mirror images (l=1.73/r=1.00 against l=1.00/r=1.73).
            //   * `tan` — the same shift with the pictures the right way round, if
            //     the quad is built with the other eye's frustum.
            //   * `rec=no` — no frame record, so BOTH quads fall back to the
            //     symmetric 90 degree default while the guest rendered asymmetric
            //     pictures. That one is wrong in both eyes, unequally.
            //
            // Keyed on the values rather than a bare `once`: the interesting case is
            // the one where they CHANGE (a stage re-created, a swapchain rebuilt),
            // and a first-frame-only line is exactly what would miss it.
            let shape = viewIndices.map { vi -> String in
                let u = uniforms[viewIndices.firstIndex(of: vi)!]
                return String(format: "v%d slice=%u tan(l%.3f r%.3f t%.3f b%.3f) vis=%u flip=%u",
                              vi, u.slice, u.tangents.x, u.tangents.y, u.tangents.z,
                              u.tangents.w, u.visible, u.flip_y)
            }.joined(separator: " | ")
            if shape != lastEyeShape {
                lastEyeShape = shape
                // DoubleWide is the one shape where both eyes on slice 0 is
                // CORRECT rather than the failure named above: the guest drew
                // one picture with a viewport per eye and each eye crops its
                // own half. Said on the line so the two cannot be confused.
                NSLog("[cp] eye placement: \(shape) | rec=\(haveRendered ? "yes" : "no") "
                      + "layered=\(layered) stage=\(stage)"
                      + (kl_glfb_doublewide() != 0
                         ? " | DoubleWide: both eyes read eye 0's texture and "
                           + "slice, cropped by their own render viewport" : ""))
            }

            // THE PROJECTION AUDIT. Both sides of the frustum question on two
            // adjacent lines per eye, once, with the numbers.
            //
            // The suspicion it settles: the guest renders through the frustum we
            // named at ovrp_GetNodeFrustum2, and this pass unprojects its picture
            // through the tangents in the frame record and reprojects through the
            // drawable's own matrix. If those three are not one frustum the
            // residual is proportional to the correction magnitude — which
            // alternates fresh/held — and an image that alternates between two
            // positions under head motion is exactly what is being reported.
            //
            // Printed per eye and per *shape* rather than once, so a frustum that
            // changes mid-run (a display re-measure, a guest that re-primes)
            // shows up rather than being averaged into a first-frame claim.
            if !loggedProjAudit {
                loggedProjAudit = true
                for vi in viewIndices where vi < 2 {
                    var told = [Float](repeating: 0, count: 4)
                    var cant = [Float](repeating: 0, count: 4)
                    told.withUnsafeMutableBufferPointer { kl_ovrp_eye_tangents(Int32(vi), $0.baseAddress) }
                    cant.withUnsafeMutableBufferPointer { kl_ovrp_eye_rotation_told(Int32(vi), $0.baseAddress) }
                    let cantDeg = simd_quatf(ix: cant[0], iy: cant[1], iz: cant[2], r: cant[3]).angle
                        * 180 / .pi
                    let u = uniforms[viewIndices.firstIndex(of: vi)!]
                    // What the drawable itself wants, recovered from the matrix
                    // this pass projects with — the third party to the question.
                    let dt = kl_reproject_tangents(drawable.computeProjection(viewIndex: vi))
                    let viewDeg = simd_quatf(drawable.views[vi].transform).angle * 180 / .pi
                    NSLog(String(format:
                        "[rp] guest eye%d proj: l %.5f r %.5f t %.5f b %.5f cant %.3f deg "
                        + "(KL_OVRP_EYE_CANT=%d)",
                        vi, told[0], told[1], told[2], told[3], cantDeg.isFinite ? cantDeg : 0,
                        kl_ovrp_eye_cant_on()))
                    NSLog(String(format:
                        "[rp] warp  eye%d assumes: l %.5f r %.5f t %.5f b %.5f "
                        + "(drawable l %.5f r %.5f t %.5f b %.5f, view rot %.3f deg) — %@",
                        vi, u.tangents.x, u.tangents.y, u.tangents.z, u.tangents.w,
                        dt.x, dt.y, dt.z, dt.w, viewDeg.isFinite ? viewDeg : 0,
                        (abs(u.tangents.x - told[0]) < 1e-4 && abs(u.tangents.y - told[1]) < 1e-4
                         && abs(u.tangents.z - told[2]) < 1e-4 && abs(u.tangents.w - told[3]) < 1e-4
                         && abs(dt.x - told[0]) < 1e-4 && abs(dt.y - told[1]) < 1e-4
                         && abs(dt.z - told[2]) < 1e-4 && abs(dt.w - told[3]) < 1e-4)
                            ? "MATCH" : "*** MISMATCH ***"))
                }
            }
            // ...and the half of the audit a boot line cannot answer: does the
            // drawable's frustum DRIFT from the one we primed the guest with?
            // The tangents were harvested from one drawable during primeDisplay
            // and the guest was told them once; this pass projects with whatever
            // matrix the live drawable hands over each frame. A slow drift there
            // is a mismatch that grows, and a first-frame comparison would call
            // it a match forever.
            for vi in viewIndices where vi < 2 {
                let dt = kl_reproject_tangents(drawable.computeProjection(viewIndex: vi))
                let u = uniforms[viewIndices.firstIndex(of: vi)!]
                let d = max(max(abs(dt.x - u.tangents.x), abs(dt.y - u.tangents.y)),
                            max(abs(dt.z - u.tangents.z), abs(dt.w - u.tangents.w)))
                if d > worstFrustumDrift {
                    worstFrustumDrift = d
                    if d > 1e-3 && !loggedFrustumDrift {
                        loggedFrustumDrift = true
                        NSLog(String(format: "[rp] *** the drawable's frustum has DRIFTED "
                                     + "from the one the guest renders with: eye %d worst "
                                     + "%.5f in a tangent (drawable l %.5f r %.5f t %.5f "
                                     + "b %.5f vs record l %.5f r %.5f t %.5f b %.5f)",
                                     vi, d, dt.x, dt.y, dt.z, dt.w, u.tangents.x,
                                     u.tangents.y, u.tangents.z, u.tangents.w))
                    }
                }
            }

            // ---------------------------------------------------------------
            // The crop, from the same frame record `rendered` came from — and it is
            // read PER EYE.
            //
            // The comment that stood here said one grid serves the amplified pass,
            // so two eyes with different viewports could not both be honoured, took
            // eye 0's, and justified it: Unity derives both from a single
            // `renderViewportScale`, so they are equal in every measured run. That
            // is true of a title that scales its resolution and FALSE of one using
            // Oculus symmetric projection, where both eyes are rendered with one
            // union frustum into a widened texture and each eye's own cone is a
            // different sub-rect of it — measured on BONELAB, eye 0 at x=0 and eye 1
            // at x=609, both 2271 wide of 2880. Eye 1 read
            // through eye 0's crop is its picture shifted by that offset, with every
            // call on the path returning success.
            //
            // So the grid carries a BLOCK PER EYE when the two rects differ, and one
            // block when they do not — which is bit-for-bit the old path for every
            // guest that has ever run here.
            var vp0 = SIMD4<Float>.zero
            var vp1 = SIMD4<Float>.zero
            var vpOf = SIMD2<Float>.zero
            if haveRendered {
                let l = rendered.viewport.0, r = rendered.viewport.1
                vp0 = SIMD4<Float>(Float(l.0), Float(l.1), Float(l.2), Float(l.3))
                vp1 = SIMD4<Float>(Float(r.0), Float(r.1), Float(r.2), Float(r.3))
                vpOf = SIMD2<Float>(Float(rendered.viewport_of.0),
                                    Float(rendered.viewport_of.1))
                // Said once, and said even when it is ZERO — which is the whole
                // point. `[ovrp] render viewport` is printed by the guest's own
                // EndFrame4 on the guest's thread, and until this line existed there
                // was nothing anywhere saying whether the number reached the
                // COMPOSITE. Those are different failures with one symptom: a rect
                // the submit never carried, a rect filed against another stage, and
                // a rect read correctly and cropped wrongly all look identical from
                // inside the headset. Zeroes here beside a non-100% line there is
                // the first; agreement is the third.
                if !loggedReadViewport {
                    loggedReadViewport = true
                    NSLog("[cp] first render viewport read from the frame record: "
                          + "\(l.0),\(l.1) \(l.2)x\(l.3) (stage \(stage), serial "
                          + "\(rendered.serial))"
                          + (l.2 <= 0 ? " — ZERO, so no crop is applied; compare "
                                      + "against the [ovrp] render viewport line" : ""))
                }
                // ...and then WATCHED. See lastViewport0 for what this is for.
                // The first twenty changes in full, because the interesting
                // thing about a dynamic-resolution title is the sequence; after
                // that only the count, so a title that rescales every frame
                // cannot drown the log it is evidence in.
                if vp0 != lastViewport0 || vp1 != lastViewport1 {
                    if lastViewport0.x >= 0 { viewportChanges += 1 }
                    lastViewport0 = vp0; lastViewport1 = vp1
                    if viewportChanges > 0, viewportSaid < 20 {
                        viewportSaid += 1
                        NSLog("[cp] the render viewport CHANGED (#\(viewportChanges)): "
                              + "eye0 \(Int(vp0.x)),\(Int(vp0.y)) "
                              + "\(Int(vp0.z))x\(Int(vp0.w))  eye1 "
                              + "\(Int(vp1.x)),\(Int(vp1.y)) "
                              + "\(Int(vp1.z))x\(Int(vp1.w)) of "
                              + "\(Int(vpOf.x))x\(Int(vpOf.y)) — the guest is "
                              + "rendering a different sub-rect, and everything "
                              + "outside it is whatever the texture held before")
                    }
                }
            }
            // Built from slot 0. The grid is a function of the eye texture's SIZE
            // and its rate map, and the two eyes agree on both — a guest that
            // rendered them at different sizes would already be failing the
            // viewport arithmetic below.
            let (gridBuf, gridVerts, gridPerEye) =
                unwarpGrid(bind0, viewport0: vp0, viewport1: vp1, viewportOf: vpOf)
            // ...and which block each view reads. Assigned here rather than passed
            // to kl_reproject_build above because the decision needs the eye
            // texture, which is not known until the loop that builds the uniforms
            // has run — so the uniforms are built for a shared grid (grid_per_eye
            // 0, the default and the old behaviour) and only a grid that really
            // carries two blocks moves eye 1 onto its own.
            if gridPerEye {
                for (n, vi) in viewIndices.enumerated() where vi == 1 {
                    uniforms[n].grid_eye = 1
                }
            }
            enc.setRenderPipelineState(probePipeline ?? pipeline)
            if let depthState { enc.setDepthStencilState(depthState) }
            enc.setFragmentTexture(bind0, index: 0)
            enc.setFragmentTexture(bind1, index: 1)
            enc.setFragmentSamplerState(sampler, index: 0)
            uniforms.withUnsafeBytes { buf in
                // Vertex only: the fragment stage has no amplification_id to index
                // this array with, so the one thing it needs — the eye's array
                // slice — arrives as a flat varying instead (kl_reproject.c).
                enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            }
            // The unwarp grid, bound to the VERTEX stage. Identity until the guest's
            // eye textures are foveated; after that it carries the squeeze this pass
            // has to undo. See unwarpGrid and kl_reproject.h.
            enc.setVertexBuffer(gridBuf, offset: 0, index: 1)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: gridVerts)
        }
        let panels = encodeOverlays(enc, viewIndices: viewIndices, drawable: drawable,
                                    originFromHead: originFromHead,
                                    writeDepth: visible == 0)
        enc.endEncoding()
        // "How many eyes got a picture", which is what the caller counts black
        // frames with — and a panel IS a picture. Without this a guest that
        // composites only quad layers reports every frame black while showing
        // its menu correctly, which is an instrument contradicting the display.
        if visible > 0 { return visible }
        return panels > 0 ? viewIndices.count : 0
    }

    /// The guest's PROJECTION layers, each as its own quad with its own frustum,
    /// in submission order — the macOS viewer's klvm_draw_proj_layers, on the
    /// device. Returns how many were drawn, which is what the caller's
    /// black-frame count needs.
    ///
    /// This REPLACES the eye pass rather than adding to it: the layers are the
    /// eye picture, stated whole instead of flattened into one texture. A guest
    /// that files no list — every guest but Steam Link, and every run under
    /// `KL_XR_LAYERS=0` — takes the eye pass unchanged, so nothing here is on
    /// any other target's path.
    ///
    /// Ordering is the draw order and nothing else. Each layer is a quad at
    /// `KL_REPROJECT_DEPTH`, so they are coplanar and the depth test cannot
    /// separate them; back to front is what OpenXR specifies and what
    /// submission order means. Depth is written by the FIRST layer drawn — the
    /// backdrop — because visionOS reprojects using the depth buffer and a
    /// frame that writes none is treated as empty (see encodeOverlays), while
    /// letting every layer write would stamp a nearer quad's depth over the
    /// same plane for no gain.
    @discardableResult
    private func encodeProjLayers(_ enc: MTLRenderCommandEncoder,
                                  viewIndices: [Int],
                                  drawable: LayerRenderer.Drawable,
                                  originFromHead: simd_float4x4,
                                  count: Int) -> Int {
        guard let pipeline, let sampler else { return 0 }
        let (grid, gridVerts) = identityGrid()
        var drawn = 0
        // Off unless asked for. Round 8 needed this every second and got its
        // answer (-1.000 .. +1.000, a 1:1 blit); round 9 is a performance
        // round, and a diagnostic that runs per layer per view inside the
        // render encode — a quaternion, two projections and an NSLog — is not
        // something to leave switched on in the runs nobody is reading.
        // `KL_CP_NDC_AUDIT=1` brings it back, at one line per layer per view
        // per second, exactly as round 8 had it.
        let audit = Self.ndcAudit && Date() >= nextProjAudit
        if audit { nextProjAudit = Date().addingTimeInterval(1) }
        for i in 0..<min(count, projLayers.count) {
            let pl = projLayers[i]
            // One uniform per amplified view, in the pass's own order — the
            // vertex shader indexes this by [[amplification_id]], so a view this
            // layer does not cover says so with visible = 0 rather than by not
            // being drawn.
            var uniforms: [kl_reproject_uniforms] = []
            var sources: [MTLTexture?] = []
            var slices: [String] = []
            for vi in viewIndices {
                let slot = vi == 1 ? pl.slot.1 : pl.slot.0
                var tex: MTLTexture? = nil
                var slice: Int32 = 0
                if slot >= 0,
                   let t = kl_glfb_eye_mtl_texture(Int32(vi), slot, &slice) {
                    // Through the array view for the reason the eye pass is:
                    // every shader in kl_reproject.c samples a
                    // texture2d_array, and a plain 2D bound to one samples
                    // nothing with no error at all.
                    tex = arrayView(Unmanaged<MTLTexture>.fromOpaque(t)
                                        .takeUnretainedValue() as MTLTexture)
                }
                let proj = drawable.computeProjection(viewIndex: vi)
                var u = withUnsafePointer(to: pl) {
                    kl_proj_layer_build($0, Int32(vi), originFromHead,
                                        Self.headFromView(drawable.views[vi].transform,
                                                          midpoint: eyeMidpointInDevice),
                                        proj, UInt32(slice))
                }
                if tex == nil { u.visible = 0 }
                slices.append(tex == nil ? "-" : "\(slice)")
                if let only = Self.onlyEye, only != vi { u.visible = 0 }
                // ---- the per-eye composite audit ------------------------
                //
                // **Where the guest's picture actually LANDS, per eye, in the
                // display's own NDC.** Round 6 proved every number either side
                // of this pass is right — the guest declares mirrored 60/45
                // frusta, the display measures the same tangents to four
                // decimals — and Austin still sees one copy left and one copy
                // right. The one quantity nobody had was the OUTPUT of the
                // placement, and it is two floats: project the quad's own left
                // and right edges and divide by w.
                //
                // A composite that is a straight 1:1 blit puts them at exactly
                // -1 and +1. Anything else is the doubling, measured: a picture
                // placed through a SYMMETRIC frustum when it was drawn through
                // an asymmetric one lands its edges at about -1.54 / +0.46 for
                // eye 0 and the mirror for eye 1, which is the two copies ~15
                // degrees either side of centre that Austin is describing.
                //
                // Printed per layer per view, once a second, because the frusta
                // move while the guest runs and a first-frame line describes a
                // shape the run mostly is not in.
                if audit {
                    let e0 = SIMD4<Float>(-u.tangents.x * u.depth, 0, -u.depth, 1)
                    let e1 = SIMD4<Float>( u.tangents.y * u.depth, 0, -u.depth, 1)
                    let p0 = u.projection * (u.model_view * e0)
                    let p1 = u.projection * (u.model_view * e1)
                    let dt = kl_reproject_tangents(proj)
                    let m  = u.model_view
                    let rot = simd_quatf(simd_float3x3(simd_make_float3(m.columns.0),
                                                       simd_make_float3(m.columns.1),
                                                       simd_make_float3(m.columns.2)))
                    let deg = rot.angle * 180 / .pi
                    NSLog(String(format:
                        "[cp] layer %d view %d slot %d slice %u vis %u | guest tan "
                        + "l%.4f r%.4f t%.4f b%.4f | display tan l%.4f r%.4f t%.4f b%.4f "
                        + "| edges land at NDC x %.3f .. %.3f (1:1 is -1.000 .. +1.000) "
                        + "| model-view %.2f deg",
                        i, vi, Int(slot), slice, u.visible,
                        u.tangents.x, u.tangents.y, u.tangents.z, u.tangents.w,
                        dt.x, dt.y, dt.z, dt.w,
                        p0.w != 0 ? p0.x / p0.w : .nan,
                        p1.w != 0 ? p1.x / p1.w : .nan,
                        deg.isFinite ? deg : 0))
                }
                uniforms.append(u)
                sources.append(tex)
            }
            // Two texture slots, view 0 in the first and every other view in the
            // second — the eye pass's rule, and for the same reason: a layer's
            // two eyes are either two slices of one texture or two textures.
            let bind0 = sources.first.flatMap { $0 } ?? sources.compactMap { $0 }.first
            let bind1 = (sources.count > 1 ? sources[1] : nil) ?? bind0
            guard let bind0, let bind1,
                  uniforms.contains(where: { $0.visible != 0 }) else {
                // Named once per layer index: "the guest submitted a layer we
                // cannot reach" and "the guest submitted fewer layers" are the
                // same picture, which is the failure this pass exists to end.
                if !loggedProjMiss.contains(i) {
                    loggedProjMiss.insert(i)
                    NSLog("[cp] projection layer \(i) (slots \(pl.slot.0)/\(pl.slot.1)) "
                          + "has no MTLTexture — not composited")
                }
                continue
            }
            // A view whose texture is neither bound slot is dropped rather than
            // shown another eye's picture — the eye pass's rule again.
            for n in uniforms.indices where uniforms[n].visible != 0 {
                if sources[n] !== (n == 0 ? bind0 : bind1) { uniforms[n].visible = 0 }
            }
            // The FIRST layer drawn writes depth and every one after it only
            // passes. visionOS reprojects from the depth buffer, so the frame
            // must carry some — but the layers are coplanar at
            // KL_REPROJECT_DEPTH, and a `.greater` test against a depth already
            // written discards every one of them.
            if let d = drawn == 0 ? depthState : depthPassState {
                enc.setDepthStencilState(d)
            }
            enc.setRenderPipelineState(pipeline)
            enc.setFragmentTexture(bind0, index: 0)
            enc.setFragmentTexture(bind1, index: 1)
            enc.setFragmentSamplerState(sampler, index: 0)
            uniforms.withUnsafeBytes { buf in
                enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            }
            // A FOVEATED layer (vivecraft launch 5): the layer IS the guest's eye
            // swapchain image, re-pointed and registered with the guest's rate
            // map by kl_glfb, so it holds a WARPED picture in physical layout.
            // The identity grid showed it as-is — the picture squeezed into the
            // physical rect (black top ~32% / right ~23% at 4608x3696) and
            // swimming as the head moved. The eye pass's unwarpGrid does the
            // undo, and it already returns the unfoveated grid for a texture
            // the map was not built for, so it is safe to ask for any layer
            // whenever a guest map exists. No map: the identity grid, unchanged.
            //
            // KL_VRR_MIRROR=1 (vivecraft 26.x): the layer is our MIRROR of one
            // slice per eye of the guest's array swapchain. kl_glfb copies each
            // slice RAW, so the mirror holds the same physical-layout picture a
            // re-pointed eye would, and the same unwarp applies.
            var layerGrid = grid, layerVerts = gridVerts
            var mode = "identity grid (no guest map)"
            if kl_glfb_eye_rate_map() != nil {
                let (g, n, _) = unwarpGrid(bind0, viewport0: .zero, viewport1: .zero,
                                           viewportOf: .zero)
                if let g, n > 0 {
                    layerGrid = g; layerVerts = n
                    mode = kl_glfb_eye_mirror_foveated() != 0
                        ? "unwarp grid (mirror)" : "unwarp grid (re-pointed eye)"
                } else {
                    mode = "identity grid (guest map, no unwarp grid for this texture)"
                }
            }
            // Once per layer per CHANGE of mode/size/slices, never per frame.
            let layerKey = "slice \(slices.joined(separator: "/")) "
                + "\(bind0.width)x\(bind0.height) — \(mode)"
            if loggedLayerMode[i] != layerKey {
                loggedLayerMode[i] = layerKey
                NSLog("[cp] per-layer composite: layer \(i) \(layerKey)")
                if mode.hasPrefix("unwarp") {
                    NSLog("[cp] per-layer composite: layer \(i) is a foveated eye "
                          + "texture \(bind0.width)x\(bind0.height) — composited "
                          + "through the guest's unwarp grid, not the identity")
                }
            }
            enc.setVertexBuffer(layerGrid, offset: 0, index: 1)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: layerVerts)
            drawn += 1
        }
        // What each layer is ACTUALLY being placed with, said when it CHANGES —
        // this guest's layer arrangement moves while it runs (a layer appears
        // and disappears about once a second, and one layer's frustum halves
        // and doubles), so a first-frame-only line describes a shape the run
        // spends most of its time not being in.
        if drawn > 0 {
            let shape = "\(count) layer(s), \(drawn) composited"
            if shape != lastProjShape {
                lastProjShape = shape
                NSLog("[cp] per-layer composite: \(shape)")
            }
        }
        return drawn
    }

    /// The identity unwarp grid, built once per density. The per-layer pass
    /// binds this rather than `unwarpGrid`'s: a layer's picture is a whole guest
    /// swapchain image copied as it is, so there is no rate map to undo and no
    /// render viewport smaller than the texture.
    ///
    /// **Its density is not cosmetic.** At 1x1 every vertex is on the outer
    /// ring, which the guard band pushes outward, so the whole picture is
    /// magnified by 1 + 2*guard — and guard follows the per-frame correction
    /// angle, so the world zooms with head speed (hla launches 3/4; the same
    /// fault `unwarpGrid` fixed for the eye pass). `KL_PROJ_LAYER_GRID`
    /// (kl_proj_layer_grid_dim) picks it: default 64 for every guest;
    /// `KL_PROJ_LAYER_GRID=1` is the pre-fix grid for a port's A/B.
    private func identityGrid() -> (MTLBuffer?, Int) {
        let dim = kl_proj_layer_grid_dim()
        if identityGridBuffer == nil || identityGridDim != dim {
            var tmp = [SIMD2<Float>](repeating: .zero,
                                     count: Int(kl_reproject_grid_entries(dim, dim)))
            tmp.withUnsafeMutableBufferPointer { kl_proj_layer_grid($0.baseAddress, dim) }
            identityGridBuffer = tmp.withUnsafeBytes {
                device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: [])
            }
            identityGridDim = dim
            NSLog("[cp] per-layer composite grid \(dim)x\(dim)"
                  + (dim > 1 ? " — the guard band stretches one edge cell, not the "
                               + "picture (KL_PROJ_LAYER_GRID=1 is the pre-fix A/B)"
                             : " — the guard band scales the WHOLE picture by "
                               + "1 + 2*guard (KL_PROJ_LAYER_GRID=64 confines it to "
                               + "the edge)"))
        }
        return (identityGridBuffer, Int(kl_reproject_grid_vertices(dim, dim)))
    }
    private var identityGridDim: UInt32 = 0
    private var loggedLayerMode: [Int: String] = [:]
    /// This frame's projection layers, taken whole and taken EARLY — see where
    /// it is filled for why the read order against the frame fence matters.
    /// Read only on the render loop, which is also the only thread that writes
    /// it, so it needs no lock of its own.
    private var projLayers = [kl_ovrp_proj_layer](repeating: kl_ovrp_proj_layer(),
                                                  count: 8)
    private var projLayerCount = 0
    private var identityGridBuffer: MTLBuffer?
    private var loggedProjMiss = Set<Int>()
    private var lastProjShape = ""
    /// Next moment the per-layer composite audit below is allowed to print.
    private var nextProjAudit = Date()
    private static let ndcAudit =
        (ProcessInfo.processInfo.environment["KL_CP_NDC_AUDIT"] ?? "0") != "0"

    /// The layers that are NOT the eye, drawn on top of it in the same encoder.
    ///
    /// `ovrp_EndFrame4` is handed a list and every reader of it here used to walk
    /// past anything but the eye layer. That is right for the frame record and it
    /// silently dropped everything else. Unity's only other layer is a 1x1 nothing
    /// renders into; **Unreal draws its UI this way**, so on RE4 it dropped the
    /// studio splash, the loading screens and the menus while the world
    /// composited perfectly.
    ///
    /// No depth ordering is needed: the eye quad sits at `KL_REPROJECT_DEPTH`
    /// (500 m) and an overlay is a few metres away, so it is in front by geometry.
    /// `KL_GUEST_OVERLAYS=0` is the A/B. Deliberately not `KL_OVERLAYS`, which is
    /// already the SYSTEM overlays (the Home indicator, KleptonApp.swift) — two
    /// knobs one letter apart, fighting over one name, is a bug with no error
    /// surface at all.
    ///
    /// Returns how many layers were actually drawn — for a guest whose whole
    /// frame is a quad that IS the picture, and the caller's black-frame count
    /// depends on knowing it.
    ///
    /// `writeDepth` is set when the overlays ARE the frame — the eye pass drew
    /// nothing, so nothing else in this pass will write depth.
    ///
    /// That distinction is the whole of it. visionOS reprojects the submitted
    /// frame using its depth buffer, and the encoder's default state writes no
    /// depth at all, so a frame whose only content is a quad layer arrives as
    /// perfect colour over a depth buffer still holding the reverse-Z clear of
    /// 0 — "nothing is here at any finite distance" — and the compositor
    /// honours that literally. It is the same measurement the eye pipeline's
    /// depth state records: a quad with writes off is invisible, the same quad
    /// with writes on appears. It never showed on the guest this pass was
    /// written for, because an Unreal title draws its eye first and its UI
    /// second, so the depth was already there to ride on.
    ///
    /// Left OFF when the eye pass did draw, which is not timidity: an overlay
    /// blends, so a transparent region of a UI quad would otherwise stamp the
    /// quad's near depth over the world behind it and reproject that patch of
    /// the world at the panel's distance.
    @discardableResult
    private func encodeOverlays(_ enc: MTLRenderCommandEncoder,
                                viewIndices: [Int],
                                drawable: LayerRenderer.Drawable,
                                originFromHead: simd_float4x4,
                                writeDepth: Bool) -> Int {
        guard overlaysEnabled, let pipe = overlayPipeline else { return 0 }
        // .greater against that same clear, and the shader's own projected
        // position carries the quad's real distance, so what lands in the
        // buffer is where the panel actually is.
        if writeDepth, let depthState { enc.setDepthStencilState(depthState) }
        let n = Int(kl_ovrp_overlay_count())
        if n == 0 { return 0 }
        var drawn = 0
        for i in 0..<n {
            var ov = kl_ovrp_overlay()
            guard kl_ovrp_overlay_get(Int32(i), &ov) != 0 else { continue }
            // One uniform per amplified view, exactly as the eye pass does — the
            // array is indexed by [[amplification_id]] and a view with no
            // placement says so with `visible = 0` rather than by not drawing,
            // because with amplification there is one draw for both.
            var uniforms: [kl_overlay_uniforms] = []
            var texture: MTLTexture? = nil
            for vi in viewIndices {
                var w: Int32 = 0, h: Int32 = 0
                // Two producers, one consumer, asked in the order they can
                // answer: a Vulkan guest's layer image belongs to kl_vulkan, and
                // everything else reaches a compositor through kl_glfb's layer
                // table — an OpenXR quad on GL is bound there and a Vulkan one
                // is recorded there too. Vulkan first, so RE4's path is
                // untouched.
                let t = kl_vulkan_layer_mtl_texture(ov.layer_id, ov.stage, Int32(vi), &w, &h)
                    ?? kl_glfb_layer_mtl_texture(ov.layer_id, ov.stage, &w, &h)
                if texture == nil, let t { texture = Unmanaged<MTLTexture>
                    .fromOpaque(t).takeUnretainedValue() }
                var u = kl_overlay_build(&ov, Int32(vi), originFromHead,
                                         Self.headFromView(drawable.views[vi].transform,
                                                           midpoint: eyeMidpointInDevice),
                                         drawable.computeProjection(viewIndex: vi))
                if t == nil { u.visible = 0 }
                uniforms.append(u)
            }
            guard let texture, uniforms.contains(where: { $0.visible != 0 }) else {
                // Named once per layer: "the guest submitted a layer we cannot
                // reach" and "the guest submitted none" are the same picture.
                if !loggedOverlayMiss.contains(ov.layer_id) {
                    loggedOverlayMiss.insert(ov.layer_id)
                    NSLog("[cp] overlay layer \(ov.layer_id) (shape \(ov.shape), stage "
                          + "\(ov.stage), \(ov.size.0)x\(ov.size.1) m) has no placement "
                          + "or no MTLTexture — not composited")
                }
                continue
            }
            enc.setRenderPipelineState(pipe)
            enc.setFragmentTexture(texture, index: 0)
            enc.setFragmentSamplerState(sampler, index: 0)
            uniforms.withUnsafeBytes { buf in
                enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
                enc.setFragmentBytes(buf.baseAddress!, length: buf.count, index: 0)
            }
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            drawn += 1
            if !loggedOverlayDraw.contains(ov.layer_id) {
                loggedOverlayDraw.insert(ov.layer_id)
                NSLog("[cp] compositing overlay layer \(ov.layer_id): "
                      + "\(ov.size.0)x\(ov.size.1) m at (\(ov.pose.4), \(ov.pose.5), "
                      + "\(ov.pose.6))"
                      + (ov.head_locked != 0 ? ", HEAD-LOCKED" : ""))
            }
        }
        return drawn
    }
    private let overlaysEnabled = klEnvOn("KL_GUEST_OVERLAYS", default: true)
    private var loggedOverlayMiss = Set<Int32>()
    private var loggedOverlayDraw = Set<Int32>()
    private var loggedSplitEyes = false
    /// Said once: what the two texture slots resolved to, and whether the eyes
    /// share one texture or arrive as one swapchain each. Two guests of
    /// different shapes composite through this one pass and nothing else says
    /// which shape a run is.
    private var loggedEyeTextures = false
    /// The projection audit — the `[rp]` pair, once, and the running check that
    /// the drawable's frustum has not drifted from the one the guest was told.
    private var loggedProjAudit = false
    private var loggedFrustumDrift = false
    private var worstFrustumDrift: Float = 0
    /// The last per-eye placement described by `[cp] eye placement`, so the line
    /// is printed when it CHANGES rather than only on the first frame.
    private var lastEyeShape = ""
    /// KL_WARP=system|ours — which reprojection actually corrects the guest's
    /// picture. See the long note at the call site in `renderFrame`.
    ///
    /// Default `ours` HERE, deliberately, even though `system` is what shipped
    /// on Pistol Whip. `system` reshapes the present path for EVERY port — it
    /// stamps the drawable with the render pose and hands the whole correction
    /// to the compositor — and it has been validated on exactly one title. The
    /// arm is present and one launch away (`KL_WARP=system`); making it the
    /// default is a per-port decision to be taken on a per-port round, not a
    /// side effect of promoting a velocity fix.
    private static let warpBySystem: Bool = {
        let e = ProcessInfo.processInfo.environment["KL_WARP"] ?? ""
        let sys = e.hasPrefix("s") || e.hasPrefix("S")
        let ours = !sys
        NSLog("[cp] KL_WARP=\(e.isEmpty ? "ours (default)" : e) — "
              + (ours ? "OUR pass does the correction and the drawable is "
                        + "stamped with the display pose (the pre-round-4 arm)"
                      : "the SYSTEM does the correction; the drawable is "
                        + "stamped with the render pose and our model-view is "
                        + "neutral"))
        return !ours
    }()
    private var loggedWarpArm = false

    /// KL_CP_EYE=<0|1>, the binocular/temporal split. See encodeViews.
    private static let onlyEye: Int? = {
        guard let e = ProcessInfo.processInfo.environment["KL_CP_EYE"],
              let v = Int(e), v == 0 || v == 1 else { return nil }
        NSLog("[cp] KL_CP_EYE=\(v) — compositing that eye ONLY; the other "
              + "stays cleared. Doubling that survives this is TEMPORAL.")
        return v
    }()

    /// Did this frame get its own display period, or is it being held?
    ///
    /// Two numbers, and they answer different questions. The **gap** between
    /// consecutive presentation times says what the system did with our frames:
    /// 1.00 periods means every frame we submit is shown once, 2.00 means each
    /// is being shown twice and half our reprojections never reach the display.
    /// The **deadline** says whether that is our fault: encoding that finishes
    /// after `renderingDeadline` is a frame the system had to fall back on.
    ///
    /// A picture can be several frames stale and still look perfect here, which
    /// is the point — staleness is reprojection's job and it does it every
    /// period. A repeat is the case reprojection cannot reach.
    private func noteCadence(presentation: TimeInterval,
                             drawable: LayerRenderer.Drawable) {
        // The deadline for THIS frame, against the same clock it is expressed
        // in. Checked after commit, so it measures the encode we just did.
        let now = LayerRenderer.Clock.Instant.epoch
            .duration(to: LayerRenderer.Clock().now).timeInterval
        let deadline = LayerRenderer.Clock.Instant.epoch
            .duration(to: drawable.frameTiming.renderingDeadline).timeInterval
        if now > deadline { missedDeadline += 1 }

        defer { lastPresentation = presentation }
        guard lastPresentation > 0 else { return }
        // The period, from whatever source has one. primeDisplay's measurement
        // is the good one, but it can fail to get eight drawables — and a
        // cadence report that silently does not exist because a DIFFERENT
        // measurement failed is exactly the kind of instrument that wastes a
        // device run. Falling back to the rate the guest is being told keeps
        // the ratio meaningful; failing that the raw milliseconds still are.
        if displayPeriod <= 0 {
            // The PANEL's rate, not the guest's: this measures how often WE
            // present, and those two numbers are deliberately allowed to differ
            // (a 72 Hz guest on a 120 Hz panel — see kl_ovrp_set_display_
            // frequency). Reading the guest's here would make the cadence ratio
            // report 0.6 periods on a perfectly paced compositor.
            let hz = Double(kl_ovrp_display_frequency_panel())
            displayPeriod = hz > 1 ? 1.0 / hz : 0
        }
        let seconds = presentation - lastPresentation
        let gap = displayPeriod > 0 ? seconds / displayPeriod : 0
        // A gap of zero or a negative one is the same drawable seen twice or a
        // clock that moved backwards; neither is a cadence measurement.
        guard seconds > 0 else { return }
        cadenceN += 1
        cadenceSum += seconds
        cadenceMax = max(cadenceMax, seconds)
        if gap >= 1.5 { cadenceRepeats += 1 }
    }

    /// The cadence numbers, for the liveness line — which is the one report
    /// here that is known to print, on a wall clock, whatever else has failed.
    /// Reported there rather than on a sample count of its own so that "the
    /// cadence line is missing" can only ever mean "no frames were presented".
    private func cadenceSummary() -> String {
        guard cadenceN > 0 else { return "cadence n/a (no presents yet)" }
        let meanS = cadenceSum / Double(cadenceN)
        let periods = displayPeriod > 0 ? meanS / displayPeriod : 0
        let out = String(format: "cadence %.2f ms mean / %.2f max", meanS * 1000,
                         cadenceMax * 1000)
            + (displayPeriod > 0 ? String(format: " = %.2f periods", periods) : "")
            + ", \(cadenceRepeats)/\(cadenceN) held >=1.5, "
            + "\(missedDeadline) past deadline"
            // Only when it has happened: a permanently-zero clause on the one
            // line known to print is noise, and a NON-zero one here is the
            // tracker refusing the instant the composite asked about — which is
            // the quad going head-locked, named. See displayOriginFromDevice.
            + (anchorWalkedBack + anchorHeld + anchorUnknown > 0
               ? ", anchor \(anchorWalkedBack) walked back"
                 + " / \(anchorHeld) held / \(anchorUnknown) unknown"
               : "")
            // Carried on the one line that is known to print. A non-zero count
            // here is a title whose rendered sub-rect moves during the run,
            // which is the standing candidate for the blue fill (lastViewport0).
            + ", \(viewportChanges) render-viewport change(s)"
        // The latch is a BOOT measurement, and visionOS reserves the right to
        // change the panel rate mid-session — a thermal demotion to 59.98 Hz
        // has been seen on this hardware. That recreates session 16's bug after
        // the point where anything can be re-latched (the guest read its rate
        // once, at boot, and Unity stored it), so the least this can do is stop
        // the next session from being diagnosed from scratch. Three consecutive
        // reports — six seconds — of the cadence disagreeing with the latched
        // period by more than 10% is not a hitch.
        if displayPeriod > 0 && cadenceN >= 30 && abs(periods - 1.0) > 0.10 {
            cadenceDisagreed += 1
            // Print at three, then keep saying it every fifteen windows (~30 s).
            // `== 3` printed once and went silent for the rest of the run, which
            // is backwards: a hitch is the case that deserves one line, and a
            // PERMANENT disagreement is the case that has to stay on the screen.
            // Launch 20 held a 2x disagreement — a 60 Hz present against the
            // 120 Hz we latched and told the guest — for 130 seconds; the log
            // said so twice inside the first 26 and never again, so the run's
            // smoothness looked unconfounded when every frame of it was.
            if cadenceDisagreed >= 3 && (cadenceDisagreed - 3) % 15 == 0 {
                NSLog(String(format: "[cp] THE PANEL IS NO LONGER RUNNING AT THE RATE WE "
                             + "LATCHED. We latched %.2f Hz at boot and told the guest "
                             + "%.2f Hz; the last six seconds present every %.2f ms, "
                             + "i.e. %.2f Hz. The guest cannot be re-told — it read its "
                             + "rate once, at boot — so its pacing is now incoherent with "
                             + "the panel and motion will judder. Treat any smoothness "
                             + "verdict from here on as confounded.",
                             1.0 / displayPeriod, kl_ovrp_display_frequency(),
                             meanS * 1000, meanS > 0 ? 1.0 / meanS : 0))
            }
        } else {
            cadenceDisagreed = 0
        }
        cadenceN = 0; cadenceSum = 0; cadenceMax = 0
        cadenceRepeats = 0; missedDeadline = 0
        anchorWalkedBack = 0; anchorHeld = 0; anchorUnknown = 0
        return out
    }

    /// Harvest what only a drawable can tell us, before the guest exists to
    /// ask: the per-eye frustum and the display's real frame rate.
    ///
    /// An ordering problem. Both numbers can only be read from a `cp_drawable`,
    /// which arrives once the frame loop is running, but the guest reads both
    /// ONCE and early — Unity latches the frustum when its VRDevice initializes
    /// and the display frequency into its VR timing config. Answering correctly
    /// one frame later answers a question nobody asks again. So a throwaway
    /// frame is rendered here purely to harvest them, before
    /// `kl_app_lifecycle_begin()`.
    ///
    /// Tangents come from the projection matrix rather than `cp_view_tangents`,
    /// which visionOS 2.0 deprecated, and the matrix is what the composite pass
    /// projects with.
    /// Tell the guest where this display's eyes actually are.
    ///
    /// `view.transform` is `device_from_view`, so its translation is the
    /// head->eye offset in the head's own frame, which is what
    /// `kl_ovrp_set_eye_offset` wants. Without it nodes 0 and 1 both answer the
    /// head pose: an IPD of zero, both eyes rendering from the same point, and
    /// a world with no disparity in it.
    ///
    /// Pushed from `primeDisplay` so it is right before the guest's first
    /// frame, and again each frame because it costs two stores and the user can
    /// change the fit of the headset mid-run.
    ///
    /// Tell the guest how big its eye render targets should be.
    ///
    /// Everything else about the synthetic headset describes a Quest 2 on
    /// purpose — `Build.MODEL`, `ovrp_GetSystemHeadsetType`, the version string
    /// — because every Oculus branch in the guest is written against one. The
    /// eye SIZE is different in kind: nothing branches on it, it is just a
    /// resolution, and the Quest's 2290x2400 is wrong in both axes for the
    /// display underneath. So it is measured rather than claimed.
    ///
    /// Which number: with foveation on, the drawable's colour textures are the
    /// PHYSICAL size — already reduced — while the rate map's `screenSize`
    /// is the logical resolution the fovea is rasterized at. The guest should be
    /// sized against the latter: matching the physical size would throw away
    /// exactly the resolution `maxRenderQuality` was set to buy. Without a rate
    /// map there is no distinction and the texture size is the answer.
    ///
    /// The clamping lives in `kl_ovrp_set_eye_texture_size`, not here, so a host
    /// run through `t_mtl_provider` gets the same policy.

    // MARK: - The unwarp grid

    /// Screen -> physical, as `kl_reproject_grid_build` wants it. No captures,
    /// so it converts to a C function pointer.
    private static let screenToPhysical: kl_reproject_s2p = { ctx, sx, sy, px, py in
        guard let ctx,
              let map = Unmanaged<AnyObject>.fromOpaque(ctx)
                  .takeUnretainedValue() as? MTLRasterizationRateMap else { return }
        let p = map.physicalCoordinates(
            screenCoordinates: MTLSamplePosition(x: sx, y: sy), layer: 0)
        px?.pointee = p.x
        py?.pointee = p.y
    }

    private var gridBuffer: MTLBuffer?
    private var gridMap: UnsafeMutableRawPointer?
    private var gridTexW = -1, gridTexH = -1
    private var gridVertexCount = 0
    private var gridViewport: SIMD4<Float> = .zero
    /// Eye 1's rect, and whether the built grid carries a block for it. Part of
    /// the cache key: a guest that starts using symmetric projection partway
    /// through (BONELAB does — the plugin turns it on once the eye textures are
    /// sized) changes only this one, and a grid keyed on eye 0's alone would
    /// never be rebuilt.
    private var gridViewportR: SIMD4<Float> = .zero
    private var gridPerEye = false
    private var loggedSplitViewports = false
    private var loggedReadViewport = false
    /// The render viewport, WATCHED rather than sampled once.
    ///
    /// Only the FIRST rect has ever been logged, and a first rect cannot answer
    /// the question r17 ends on. Austin's report of the blue fill is that its
    /// boundary moves with the game's ZOOM — more fill zoomed out, less zoomed
    /// in — and a boundary that tracks zoom is a boundary that tracks GPU load.
    /// UE4 lowers `r.ScreenPercentage` under load and renders into a SMALLER
    /// sub-rect of the same eye texture; everything outside it is whatever the
    /// texture held before. If that is what this is, the rect below changes
    /// during the run and this line says so, in one launch, with numbers.
    /// If it never changes, that hypothesis is dead and the blue is inside the
    /// picture the guest drew.
    private var lastViewport0: SIMD4<Float> = .init(repeating: -1)
    private var lastViewport1: SIMD4<Float> = .init(repeating: -1)
    private var viewportChanges = 0
    private var viewportSaid = 0
    private var loggedViewportOfSkew = false
    private var loggedRateMapSkew = false
    private var loggedGridUndersampled = false

    /// The reprojection quad's mesh, with texture coordinates that already undo
    /// the guest's foveation.
    ///
    /// Rebuilt only when the rate map or the eye texture's size changes — a rate
    /// map is static, so the unwarp is resolved once here and the rasterizer
    /// interpolates it for free, instead of every fragment paying a
    /// `map_screen_to_physical_coordinates` decode. One cell per ZONE, so the
    /// grid's vertices land on the map's own piecewise-linear breakpoints and
    /// the result is exact rather than a sampling of the curve.
    ///
    /// Note this is the GUEST's map (kl_glfb_eye_rate_map), which has nothing to
    /// do with the drawable's own foveation: Compositor Services rasterizes THIS
    /// pass through its own map, set on the render pass descriptor. Two maps,
    /// two purposes, deliberately detached.
    ///
    /// `viewport` is the guest's render viewport for this frame in eye-texture
    /// pixels, `.zero` for the whole texture — kl_ovrp_render_pose.viewport,
    /// taken from the same frame record the uniforms come from. It is part of
    /// the key: Beat Saber shrinks it on entering a map (the render-viewport
    /// note in kl_ovrp.h), and a grid built for the previous viewport crops the
    /// wrong rectangle with nothing anywhere reporting it.
    /// Returns the grid, the vertex count, and whether it carries a BLOCK PER
    /// EYE — which it does exactly when the two eyes submitted different rects.
    private func unwarpGrid(_ source: MTLTexture?,
                            viewport0: SIMD4<Float>,
                            viewport1: SIMD4<Float>,
                            viewportOf: SIMD2<Float>) -> (MTLBuffer?, Int, Bool) {
        let map = kl_glfb_eye_rate_map()
        let tw = source?.width ?? 0, th = source?.height ?? 0
        // A viewport that IS the whole texture is not a crop, and keeping it as
        // .zero means an unscaled title never leaves the pre-existing path.
        //
        // **A rect measured against another texture is not a crop, it is a
        // coincidence.** `viewportOf` is the eye texture size the guest's submit
        // was relative to (kl_ovrp.h); when it is not the texture in hand, the
        // rect says nothing about which texels of THIS one were written, and
        // cropping by it magnifies the picture by the ratio of the two — 1.2x on
        // device, which reads as a distorted, cross-eyed view rather than as a
        // cropping bug, because the binocular disparity is magnified with it.
        // Measured on a headset, 2026-08-12: a 3072x2464 rect against a
        // 3686x2956 texture, persistently, after Unity's fourth eye layer.
        //
        // The cause is fixed in klovrp_SetupLayer. This is the guard, and it is
        // deliberately NOT a rescale: with the two disagreeing we do not know
        // which region the guest wrote, and inventing one is how the first
        // version of this went wrong. Not cropping is the pre-viewport
        // behaviour — visibly wrong if it ever fires, which is the point.
        //
        // Applied to BOTH eyes, and it is one decision for the pair: a texture
        // the rects do not describe does not describe either of them.
        func sanitise(_ r: SIMD4<Float>) -> SIMD4<Float> {
            var vp = r
            if vp.z > 0, viewportOf.x > 0, viewportOf.y > 0,
               Int(viewportOf.x) != tw || Int(viewportOf.y) != th {
                if !loggedViewportOfSkew {
                    loggedViewportOfSkew = true
                    NSLog("[cp] the render viewport is \(Int(vp.z))x\(Int(vp.w)) of a "
                          + "\(Int(viewportOf.x))x\(Int(viewportOf.y)) eye texture, but "
                          + "the texture in hand is \(tw)x\(th) — NOT cropping, because "
                          + "that rect describes another texture (kl_ovrp.h viewport_of)")
                }
                vp = .zero
            }
            if vp.z <= 0 || vp.w <= 0 ||
                (vp.x == 0 && vp.y == 0 && Int(vp.z) == tw && Int(vp.w) == th) { vp = .zero }
            return vp
        }
        let vp = sanitise(viewport0)
        let vpR = sanitise(viewport1)
        // Two blocks only when the eyes really disagree. Equal rects — every
        // guest measured here before BONELAB's symmetric projection — take the
        // single-block path unchanged, cache key and all.
        let perEye = vp != vpR
        if perEye, !loggedSplitViewports {
            loggedSplitViewports = true
            NSLog("[cp] the eyes submitted DIFFERENT render viewports "
                  + "(\(Int(vp.x)),\(Int(vp.y)) \(Int(vp.z))x\(Int(vp.w)) vs "
                  + "\(Int(vpR.x)),\(Int(vpR.y)) \(Int(vpR.z))x\(Int(vpR.w))) "
                  + "— the unwarp grid carries a block per eye")
        }
        let cropped = vp.z > 0 && vp.w > 0
        let croppedR = vpR.z > 0 && vpR.w > 0
        let eyes: UInt32 = perEye ? 2 : 1
        if gridBuffer == nil || map != gridMap || tw != gridTexW || th != gridTexH
            || vp != gridViewport || vpR != gridViewportR {
            gridMap = map; gridTexW = tw; gridTexH = th
            gridViewport = vp; gridViewportR = vpR
            var nx: UInt32 = 1, ny: UInt32 = 1
            // **The map has to BELONG to the texture in hand, and the other
            // half of this seam already says so.**
            //
            // `kl_glfb_eye_rate_map()` is a global: ONE map, for whatever eye
            // size the guest last asked for. kl_glfb does not apply it blindly
            // — `kl_glfb_set_eye_rate_map` walks every registered eye texture
            // and calls `a_SetRateMap(tex, NULL)` on any whose size is not the
            // map's, printing "eye=N stage=S is WxH, not the W'xH' this rate
            // map was built for — left unfoveated". That texture is then
            // rasterized at FULL rate and holds an unwarped picture.
            //
            // This side read the same global and unwarped anyway. Unity
            // re-creates its eye swapchain constantly here — the layer is
            // allocated at our capped size and then again at Unity's own 1.2x,
            // so 4032x3234 and 4838x3880 alternate all run — and the map always
            // changes FIRST, because updateGuestRateMap is called from
            // provideEyeTexture before the new textures exist. Every one of
            // those transitions leaves this pass reading a full-rate picture
            // through a foveation curve, and the log has been printing the
            // mismatched pair the whole time: "unwarp grid 64x64 for a
            // 4032x3234 eye texture (map screen 4838x3880 ...)".
            //
            // The picture that produces is not subtly wrong. The curve is
            // ~33% of the fragments, so the unwarp magnifies the centre by
            // about 1.5x and pulls the periphery in from texels the guest
            // filled at a different density — a soft, mis-scaled frame with no
            // error surface anywhere, which is what "blurry" describes.
            //
            // So the two halves now agree on one rule: a map whose screenSize
            // is not this texture's size does not describe this texture, and
            // the honest grid for it is the identity (plus whatever crop the
            // viewport asks for). Not a rescale — the same argument
            // `sanitise` makes above about a rect measured against another
            // texture: we do not know what the guest wrote, and inventing it is
            // how this went wrong in the first place.
            let rateMap: MTLRasterizationRateMap? = {
                guard let map, tw > 0, th > 0,
                      let rm = Unmanaged<AnyObject>.fromOpaque(map)
                          .takeUnretainedValue() as? MTLRasterizationRateMap
                else { return nil }
                if rm.screenSize.width == tw && rm.screenSize.height == th { return rm }
                if !loggedRateMapSkew {
                    loggedRateMapSkew = true
                    NSLog("[cp] the guest's rate map is for a "
                          + "\(rm.screenSize.width)x\(rm.screenSize.height) eye and the "
                          + "texture in hand is \(tw)x\(th) — NOT unwarping, because "
                          + "kl_glfb left this texture UNFOVEATED for exactly the same "
                          + "reason (kl_glfb_set_eye_rate_map)")
                }
                return nil
            }()
            if let rm = rateMap, let map {
                var zx: Int32 = 0, zy: Int32 = 0
                kl_glfb_eye_rate_zones(&zx, &zy)
                if zx <= 0 || zy <= 0 {
                    // A map we did not build does not report its zones, but the
                    // GRANULARITY cell count is recoverable — the trick ALVR's
                    // DummyMetalRenderer uses to reconstruct a descriptor.
                    // Uniform in physical rather than screen space, so it is a
                    // dense sampling of the curve rather than zone-aligned and
                    // exact; at ~32px granules the residual is under a texel.
                    let gran = rm.physicalGranularity
                    let phys = rm.physicalSize(layer: 0)
                    zx = gran.width  > 0 ? Int32(phys.width  / gran.width)  : 16
                    zy = gran.height > 0 ? Int32(phys.height / gran.height) : 16
                }
                let cap = Int32(KL_REPROJECT_GRID_MAX)
                // Zone alignment is what makes the unwarp EXACT rather than a
                // piecewise-linear approximation of a piecewise-linear map
                // (kl_reproject.h), and on a DoubleWide canvas it depends on a
                // relationship between three numbers nothing here enforces: the
                // map now carries 2 x 64 zones, the grid caps at 64 cells, and
                // the grid is built PER EYE — so 64 cells land on 64 zones,
                // one for one. Drop the per-eye grid and the same map is
                // sampled at half its resolution, silently. Say so rather than
                // leave it to arithmetic to work out.
                if !perEye && zx > cap && !loggedGridUndersampled {
                    loggedGridUndersampled = true
                    NSLog("[cp] the rate map has \(zx) horizontal zones and the "
                          + "unwarp grid caps at \(cap) cells across ONE block — "
                          + "the map is being sampled at \(Int(zx) / Int(cap))x "
                          + "coarser than it was built. That is exact only when "
                          + "the grid is per-eye.")
                }
                nx = UInt32(max(1, min(cap, zx)))
                ny = UInt32(max(1, min(cap, zy)))
                let n = Int(kl_reproject_grid_entries_n(nx, ny, eyes))
                var table = [SIMD2<Float>](repeating: .zero, count: n)
                let scr = rm.screenSize
                var rects = [[vp.x, vp.y, vp.z, vp.w], [vpR.x, vpR.y, vpR.z, vpR.w]]
                let croppedEye = [cropped, croppedR]
                for e in 0..<Int(eyes) {
                    table.withUnsafeMutableBufferPointer { buf in
                        rects[e].withUnsafeBufferPointer { r in
                            kl_reproject_grid_build_eye(buf.baseAddress, UInt32(e), nx, ny,
                                                        croppedEye[e] ? r.baseAddress : nil,
                                                        Float(scr.width), Float(scr.height),
                                                        Float(tw), Float(th),
                                                        Self.screenToPhysical, map)
                        }
                    }
                }
                gridBuffer = queue?.device.makeBuffer(bytes: table,
                                                      length: n * MemoryLayout<SIMD2<Float>>.stride)
                NSLog("[cp] unwarp grid \(nx)x\(ny)\(perEye ? " x2 eyes" : "") for a "
                      + "\(tw)x\(th) eye texture "
                      + "(map screen \(scr.width)x\(scr.height), viewport "
                      + (cropped ? "\(Int(vp.x)),\(Int(vp.y)) \(Int(vp.z))x\(Int(vp.w))"
                                 : "whole texture")
                      + (perEye ? " / \(Int(vpR.x)),\(Int(vpR.y)) "
                                  + "\(Int(vpR.z))x\(Int(vpR.w))" : "") + ")")
            } else {
                // No rate map — with or without a crop to do. THIS is the path a
                // KL_VRR=0 run takes, every host run, and every port whose guest
                // is unfoveated; BONELAB's symmetric projection lands on it too.
                //
                // **It used to be ONE cell, and one cell is where the guard band
                // turns into a zoom.** With no map the screen->texture mapping is
                // affine, so four corners really do carry the SAMPLING exactly —
                // that argument was right and is why this was 1x1. What it missed
                // is that the grid also carries the guard band's POSITIONS, and
                // the shader stretches the outer RING of cells outward by `guard`
                // (kl_reproject.c, `cq`): with nx = ny = 1 every vertex is on the
                // outer ring, so there is no inner cell to hold the picture still
                // and the whole quad scales by 1 + 2*guard while its uv range
                // stays 0..1. On this display guard measures ~0.055 every frame
                // (`[rp] guard: ... margin worst 0.0495`, r23's worn log), so the
                // unfoveated composite was magnifying the guest's picture by
                // about 11% and pushing 5% of each edge out of the field of view.
                // The foveated path never showed it: at 64x64 the stretched ring
                // is one cell of sixty-four, which is the thin edge smear the
                // guard band was designed to be.
                //
                // So this path now builds at the same density as the foveated one.
                // The table is a one-off (65x65 float2 per eye) and the extra
                // vertices are the ones the foveated path has always drawn.
                let dim = UInt32(max(1, min(Int(KL_REPROJECT_GRID_MAX),
                                            Int(klEnvFloat("KL_REPROJECT_GRID", 64)))))
                nx = dim; ny = dim
                let n = Int(kl_reproject_grid_entries_n(nx, ny, eyes))
                var table = [SIMD2<Float>](repeating: .zero, count: n)
                var rects = [[vp.x, vp.y, vp.z, vp.w], [vpR.x, vpR.y, vpR.z, vpR.w]]
                let croppedEye = [cropped, croppedR]
                for e in 0..<Int(eyes) {
                    table.withUnsafeMutableBufferPointer { buf in
                        rects[e].withUnsafeBufferPointer { r in
                            kl_reproject_grid_build_eye(buf.baseAddress, UInt32(e), nx, ny,
                                                        croppedEye[e] ? r.baseAddress : nil,
                                                        Float(tw), Float(th),
                                                        Float(tw), Float(th), nil, nil)
                        }
                    }
                }
                gridBuffer = queue?.device.makeBuffer(bytes: table,
                                                      length: n * MemoryLayout<SIMD2<Float>>.stride)
                NSLog("[cp] unfoveated grid \(nx)x\(ny)\(perEye ? " x2 eyes" : "") — "
                      + (cropped || croppedR
                         ? "render viewport \(Int(vp.x)),\(Int(vp.y)) "
                           + "\(Int(vp.z))x\(Int(vp.w))"
                           + (perEye ? " / \(Int(vpR.x)),\(Int(vpR.y)) "
                                       + "\(Int(vpR.z))x\(Int(vpR.w))" : "")
                           + " of a \(tw)x\(th) eye texture, compositing that sub-rect"
                         : "the whole \(tw)x\(th) eye texture")
                      + " (the guard band stretches one cell of \(nx), not the "
                      + "whole picture — KL_REPROJECT_GRID)")
            }
            gridVertexCount = Int(kl_reproject_grid_vertices(nx, ny))
            gridPerEye = perEye
        }
        return (gridBuffer, gridVertexCount, gridPerEye)
    }

    private func pushEyeTextureSize(_ drawable: LayerRenderer.Drawable) {
        var w = 0, h = 0
        var source = "none"
        if let rm = drawable.rasterizationRateMaps.first {
            w = Int(rm.screenSize.width); h = Int(rm.screenSize.height)
            source = "rate map screenSize"
        } else if let t = drawable.colorTextures.first {
            w = t.width; h = t.height
            source = "colour texture"
        }
        guard w > 0, h > 0 else {
            NSLog("[cp] no drawable size to push — the guest keeps its default eye size")
            return
        }
        // The per-view viewport, logged rather than used. In a layered drawable
        // each view is a whole slice and the viewport is the full texture, so it
        // says the same thing; in a dedicated one it would not, and this is the
        // line that would show that before it became a wrong-sized picture.
        if let vp = drawable.views.first?.textureMap.viewport {
            NSLog(String(format: "[cp] eye size from %@: %dx%d "
                                 + "(view[0] viewport %.0fx%.0f, %d colour texture(s))",
                         source, w, h, vp.width, vp.height, drawable.colorTextures.count))
        }
        kl_ovrp_set_eye_texture_size(Int32(w), Int32(h))
    }

    /// The tracker the pose-at-time callback queries.
    ///
    /// Static because kl_ovrp takes a plain C function pointer, which cannot
    /// capture. Written once during setup and only read afterwards.
    nonisolated(unsafe) static var headAtTracker: WorldTrackingProvider?

    /// A head pose for an instant, for `kl_ovrp_set_head_at`.
    ///
    /// Two things here are defensive rather than obvious, and both come from
    /// this API's history:
    ///
    ///   * **The anchor is re-queried, never cached.** Device anchors have
    ///     carried hidden state, so an anchor object held across a frame is
    ///     not reliably the pose it was fetched for. Every call fetches.
    ///   * **The timestamp is walked back.** The tracker returns nil for an
    ///     instant it will not predict to, and how far ahead that is is not
    ///     documented and not fixed. Stepping back 5 ms at a time finds the
    ///     edge instead of guessing where it is; the interval is already
    ///     capped at 50 ms by the caller, so this walks at most that far.
    ///
    /// Returning 0 leaves the guest with the current pose, which is what it
    /// had before this existed — strictly better than inventing one.
    nonisolated(unsafe) static let headAt: @convention(c)
        (Double, UnsafeMutablePointer<Float>?) -> Int32 = { time, out in
        guard let out, let tracker = KleptonCompositor.headAtTracker else { return 0 }
        var t = time
        var anchor = tracker.queryDeviceAnchor(atTimestamp: t)
        var steps = 0
        while anchor == nil && steps < 10 {
            t -= 0.005
            anchor = tracker.queryDeviceAnchor(atTimestamp: t)
            steps += 1
        }
        guard let a = anchor else { return 0 }
        let (p, q) = KleptonCompositor.decompose(a.originFromAnchorTransform)
        // Head space, matching every other pose this file publishes.
        let m = KleptonCompositor.headFrom(a.originFromAnchorTransform,
                                           midpoint: KleptonCompositor.headAtMidpoint)
        let (hp, hq) = KleptonCompositor.decompose(m)
        _ = p; _ = q
        out[0] = hq.imag.x; out[1] = hq.imag.y; out[2] = hq.imag.z; out[3] = hq.real
        out[4] = hp.x;      out[5] = hp.y;      out[6] = hp.z
        return 1
    }

    /// The midpoint the callback uses — the instance's, copied out so a C
    /// function pointer can reach it.
    nonisolated(unsafe) static var headAtMidpoint = SIMD3<Float>(0, 0, 0)

    /// The eye midpoint in the device anchor's frame — the offset between what
    /// visionOS calls the device and what OpenXR calls VIEW.
    ///
    /// Zero until `pushEyeOffsets` has seen a drawable, and zero forever if
    /// `KL_HEAD_MIDPOINT=0`; both helpers below are the identity at zero, so
    /// the knob and the not-yet-measured case are one code path.
    private var eyeMidpointInDevice = SIMD3<Float>(0, 0, 0)

    /// `origin_from_head` from `origin_from_device`.
    private static func headFrom(_ originFromDevice: simd_float4x4,
                                 midpoint m: SIMD3<Float>) -> simd_float4x4 {
        if m == SIMD3<Float>(0, 0, 0) { return originFromDevice }
        var t = matrix_identity_float4x4
        t.columns.3 = SIMD4<Float>(m.x, m.y, m.z, 1)
        return originFromDevice * t
    }

    /// `head_from_view` from `device_from_view` — the drawable's own per-eye
    /// transform, restated about the midpoint so the whole composite chain is
    /// in one space.
    private static func headFromView(_ deviceFromView: simd_float4x4,
                                     midpoint m: SIMD3<Float>) -> simd_float4x4 {
        if m == SIMD3<Float>(0, 0, 0) { return deviceFromView }
        var t = matrix_identity_float4x4
        t.columns.3 = SIMD4<Float>(-m.x, -m.y, -m.z, 1)
        return t * deviceFromView
    }

    /// KL_HEAD_MIDPOINT, read ONCE. pushEyeOffsets runs every frame, and every
    /// `ProcessInfo.environment` access rebuilds the whole dictionary (a strdup
    /// per variable) under libc's environ lock — Batman launch 25 died inside
    /// exactly that strdup, on this line, at render rate.
    private static let headMidpointOn =
        ProcessInfo.processInfo.environment["KL_HEAD_MIDPOINT"] != "0"
    /// The midpoint last logged; the line is printed when it moves, not per
    /// frame (it was 18,843 lines of boot25.log).
    private var loggedEyeMidpoint: SIMD3<Float>?

    private func pushEyeOffsets(_ drawable: LayerRenderer.Drawable) {
        // Latched, not re-read per frame. A streaming guest sends its eye
        // transform to the host ONCE and the host will not revise it, so the
        // number the guest was given has to stay the number it was given; the
        // display side is free to move underneath it, and bridging the two is
        // the composite's job rather than something to hide by re-pushing.
        if Self.headMidpointOn, drawable.views.count >= 2 {
            let a = drawable.views[0].transform.columns.3
            let b = drawable.views[1].transform.columns.3
            eyeMidpointInDevice = SIMD3<Float>((a.x + b.x) * 0.5,
                                               (a.y + b.y) * 0.5,
                                               (a.z + b.z) * 0.5)
            Self.headAtMidpoint = eyeMidpointInDevice
            if loggedEyeMidpoint.map({ simd_distance($0, eyeMidpointInDevice) > 0.0005 })
                ?? true {
                loggedEyeMidpoint = eyeMidpointInDevice
                NSLog(String(format: "[cp] eye midpoint (%.4f, %.4f, %.4f) m from the "
                                     + "device anchor — head space is this, not the anchor "
                                     + "(KL_HEAD_MIDPOINT=0 restores the anchor)",
                             eyeMidpointInDevice.x, eyeMidpointInDevice.y,
                             eyeMidpointInDevice.z))
            }
        }
        for (i, view) in drawable.views.enumerated() where i < 2 {
            // Relative to the eye MIDPOINT, because that is what the guest
            // calls its head. The drawable states these about the device
            // anchor.
            let t = view.transform.columns.3 - SIMD4<Float>(eyeMidpointInDevice.x,
                                                            eyeMidpointInDevice.y,
                                                            eyeMidpointInDevice.z, 0)
            kl_ovrp_set_eye_offset(Int32(i), t.x, t.y, t.z)
            // ...and the eye's ROTATION, which this loop used to discard.
            //
            // These displays are canted, so device_from_view is not a pure
            // translation — and the tangents pushed by primeDisplay come from
            // computeProjection, i.e. they are expressed in the *canted* frame.
            // Sending the shape without the direction told the guest to render
            // the right cone aimed straight ahead, and the composite then
            // viewed that picture through the real canted eye. The residual is
            // a per-eye rotation, equal and opposite between the two, which is
            // seen as a doubled image rather than as a blurred one.
            let q = simd_quatf(view.transform)
            kl_ovrp_set_eye_rotation(Int32(i), q.imag.x, q.imag.y, q.imag.z, q.real)
            if !loggedEyeOffsets && i == drawable.views.count - 1 {
                loggedEyeOffsets = true
                // Both eyes in the SAME space — the midpoint-relative one that
                // is actually pushed. Printing view[0]'s raw column beside
                // view[1]'s corrected one made the two eyes look wildly
                // asymmetric (2.6 cm apart vertically, 3.4 cm in depth) when
                // the pushed values are symmetric to a tenth of a millimetre,
                // and that read as a geometry fault for as long as it took to
                // subtract the midpoint by hand.
                let l = drawable.views[0].transform.columns.3
                    - SIMD4<Float>(eyeMidpointInDevice.x, eyeMidpointInDevice.y,
                                   eyeMidpointInDevice.z, 0)
                NSLog(String(format: "[cp] eye offsets from the midpoint: "
                             + "L (%.4f, %.4f, %.4f) R (%.4f, %.4f, %.4f) "
                             + "— IPD %.1f mm",
                             l.x, l.y, l.z, t.x, t.y, t.z,
                             abs(t.x - l.x) * 1000))
                // The half of view.transform the guest is NOT told about.
                //
                // We push the translation above, and the frustum tangents
                // separately — but nodes 0/1 report the HEAD's orientation, so
                // if these transforms are also rotated (canted displays), the
                // guest renders without that rotation while the composite
                // places the picture with it. Equal and opposite in the two
                // eyes, which is the error shape that reads as doubling rather
                // than as blur. Measured, not assumed: if these are ~0 the
                // whole concern is void, and if they are degrees then
                // KL_REPROJECT_NOCANT=1 is the A/B.
                for (i, v) in drawable.views.enumerated() where i < 2 {
                    let q = simd_quatf(v.transform)
                    let deg = q.angle * 180 / .pi
                    // The axis of a near-identity rotation is numerically
                    // meaningless (and may be NaN), so it is only worth
                    // printing once there is an angle to have an axis about.
                    if deg.isFinite && deg > 0.01 {
                        NSLog(String(format: "[cp] eye %d view rotation: %.3f deg "
                                     + "about (%.3f, %.3f, %.3f) — the guest is "
                                     + "NOT told this; KL_REPROJECT_NOCANT=1 drops it",
                                     i, deg, q.axis.x, q.axis.y, q.axis.z))
                    } else {
                        NSLog("[cp] eye \(i) view rotation: none (pure translation)")
                    }
                }
            }
        }
    }
    private var loggedEyeOffsets = false

    /// Ask for the resolution the configuration already paid for.
    ///
    /// `maxRenderQuality` is a **ceiling on memory**: it decides how large the
    /// drawable textures are allocated. The quality actually rendered at is a
    /// separate, runtime setting on the layer, and it starts at the device's
    /// default — so setting only the ceiling allocates the big textures and
    /// then draws into a fraction of them, which costs the memory and buys
    /// nothing. Both have to be set, and this is the second one.
    ///
    /// The system ramps between qualities over a short period rather than
    /// switching, which is why a frame can carry more than one drawable — see
    /// queryDrawables in renderFrame.
    private func applyRenderQuality() {
        let want = KleptonStageConfiguration.requestedQuality
        // On whether the ceiling was CONFIGURED, not on whether it was wanted.
        // The knob can be on and the ceiling still absent — the simulator
        // supports no foveation — and setting the running quality then aborts
        // the process inside Compositor Services. See `ceilingConfigured`.
        guard KleptonStageConfiguration.ceilingConfigured else {
            NSLog("[cp] render quality left at \(layerRenderer.renderQuality) "
                  + "(no maxRenderQuality ceiling was configured)")
            return
        }
        let before = layerRenderer.renderQuality
        layerRenderer.renderQuality = .init(want)
        NSLog("[cp] render quality \(before) -> \(layerRenderer.renderQuality) "
              + "(asked \(want), ceiling \(layerRenderer.configuration.maxRenderQuality))")
    }

    private func primeDisplay() {
        var stamps: [TimeInterval] = []
        var tangents: [SIMD4<Float>] = []

        // The settle run: a stretch of presentations that all agree on one
        // period. `runUnits` is how many PANEL SLOTS the run spans (a held
        // frame contributes 2), so `span / units` is a self-refining estimate
        // of the period rather than whatever the run's first gap happened to be.
        var runStart:  TimeInterval = 0
        var runEnd:    TimeInterval = 0
        var runUnits   = 0.0
        var runGaps    = 0
        // Every gap in the run, divided down to ONE slot. The median of these
        // is what gets latched — see the round-10 note beside the knobs.
        var runUnitGaps: [TimeInterval] = []
        // ...and what we threw away getting there, which is the interesting
        // half of the report: it is the 120 Hz the old code would have latched.
        var deadSpan:  TimeInterval = 0
        var deadUnits  = 0.0
        var deadGaps   = 0
        var latched:   TimeInterval = 0
        var latchedMean: TimeInterval = 0   // what round 9 would have latched
        var latchedGaps = 0
        var latchedMs  = 0.0
        let clockNow: () -> TimeInterval = {
            LayerRenderer.Clock.Instant.epoch
                .duration(to: LayerRenderer.Clock().now).timeInterval
        }
        let t0 = clockNow()

        while true {
            if hzSettle {
                if latched > 0 { break }
                if (clockNow() - t0) * 1000 >= hzSettleTimeoutMs { break }
            } else if stamps.count >= 8 {
                // Eight frames: enough for a median frame interval that a slow
                // first frame cannot drag, and under 100 ms of startup. This is
                // the pre-settle behaviour, kept byte-for-byte behind
                // KL_HZ_SETTLE=0 so the gate has a one-launch A/B.
                break
            }
            guard layerRenderer.state == .running,
                  let frame = layerRenderer.queryNextFrame() else { break }
            frame.startUpdate()
            frame.endUpdate()
            let timing = frame.predictTiming()
            if let timing { LayerRenderer.Clock().wait(until: timing.optimalInputTime) }
            // Drawable first, THEN startSubmission — the documented order, and
            // the same correction as renderFrame(). These are the layer's first
            // frames, so getting the sequence wrong here is not a local
            // matter: it is the state the whole session starts from.
            let all = frame.queryDrawables()
            guard let drawable = all.first else { continue }
            frame.startSubmission()
            let stamp = LayerRenderer.Clock.Instant.epoch
                .duration(to: drawable.frameTiming.presentationTime).timeInterval
            let prev = stamps.last ?? 0
            stamps.append(stamp)
            if hzSettle, prev > 0, stamp - prev > 0 {
                let gap = stamp - prev
                let ref = runUnits > 0 ? (runEnd - runStart) / runUnits : 0
                // How many slots this gap spans, if it belongs to the run at
                // all. Held frames (2x, 3x) are the same rate; anything else
                // is a different cadence and starts the run over.
                let mult = ref > 0 ? (gap / ref).rounded() : 0
                let belongs = ref > 0 && mult >= 1 && mult <= 3
                    && abs(gap / mult - ref) <= hzSettleTol * ref
                if belongs {
                    runEnd = stamp
                    runUnits += mult
                    runGaps += 1
                    runUnitGaps.append(gap / mult)
                } else {
                    if runGaps > 0 {
                        deadSpan  += runEnd - runStart
                        deadUnits += runUnits
                        deadGaps  += runGaps
                    }
                    runStart = prev
                    runEnd   = stamp
                    runUnits = 1
                    runGaps  = 1
                    runUnitGaps = [gap]
                }
                if (runEnd - runStart) * 1000 >= hzSettleMs
                    && runGaps >= hzSettleMinGaps && !runUnitGaps.isEmpty {
                    let sorted = runUnitGaps.sorted()
                    latched     = sorted[sorted.count / 2]
                    latchedMean = (runEnd - runStart) / runUnits
                    latchedGaps = runGaps
                    latchedMs   = (runEnd - runStart) * 1000
                }
            }
            if tangents.isEmpty {
                tangents = (0..<drawable.views.count).map {
                    kl_reproject_tangents(drawable.computeProjection(viewIndex: $0))
                }
                pushEyeOffsets(drawable)
                // Before the guest boots, so it allocates the right swapchain
                // once instead of rebuilding when the size changes under it —
                // and each of those rebuilds used to leak a whole generation
                //.
                pushEyeTextureSize(drawable)
                // Also before the guest boots, and for a stricter reason: the
                // eye-texture provider builds the guest's own rate map from
                // this curve, and it must be in force before the first render
                // pass into an eye texture.
                captureDisplayFoveation(drawable)
            }
            // Every drawable the frame carries, not just the one measured from:
            // a frame that leaves one unpresented is a frame half-written.
            for d in all { clearAndPresent(d) }
            // Only after a drawable was actually presented: ending a
            // submission on the no-drawable path is an abort.
            frame.endSubmission()
        }

        for (i, t) in tangents.enumerated() where i < 2 {
            NSLog(String(format: "[cp] eye %d display tangents l=%.4f r=%.4f t=%.4f b=%.4f "
                                 + "(%.1f x %.1f degrees)", i, t.x, t.y, t.z, t.w,
                         (atan(t.x) + atan(t.y)) * 180 / .pi,
                         (atan(t.z) + atan(t.w)) * 180 / .pi))
            kl_ovrp_set_eye_frustum(Int32(i), t.x, t.y, t.z, t.w)
        }

        // The frame interval, as the median of the gaps between successive
        // presentation times. Median rather than mean because the first frames
        // of a freshly-started layer are not representative and one of them is
        // enough to move an average by several Hz.
        let gaps = zip(stamps.dropFirst(), stamps).map(-).filter { $0 > 0 }.sorted()
        let median: TimeInterval? = gaps.isEmpty ? nil : gaps[gaps.count / 2]

        // What the settle gate decided, and — the part that matters when a
        // session goes wrong — what it REFUSED. `discarded` frames suggesting
        // 120 Hz in front of a latch at 90 is this whole bug, printed.
        var chosen = median
        var chosenGaps = gaps.count
        if hzSettle {
            let deadHz = deadUnits > 0 ? 1.0 / (deadSpan / deadUnits) : 0
            if latched > 0 {
                chosen = latched
                chosenGaps = latchedGaps
                // ...and the last step, which is the difference between 119.81
                // and 120.00: a measurement within half a percent of a rate the
                // panel actually runs IS that rate. Snapping is not rounding
                // for tidiness — the guest is told this number once and paces
                // against it forever, so our estimate's last two digits become
                // a permanent frequency offset against the panel.
                let rawHz = 1.0 / latched
                var snapHz = rawHz
                for c in Self.canonicalHz where abs(rawHz - c) <= hzSnapTol * c {
                    snapHz = c
                    break
                }
                if snapHz != rawHz { chosen = 1.0 / snapHz }
                NSLog(String(format: "[cp] display rate SETTLED at %.3f Hz after %.0f ms "
                             + "of steady cadence (%d intervals in the settled run, "
                             + "median of one-slot gaps; their mean says %.3f Hz); "
                             + "%d pre-settle interval(s) discarded%@%@",
                             rawHz, latchedMs, latchedGaps,
                             latchedMean > 0 ? 1.0 / latchedMean : 0, deadGaps,
                             deadHz > 0 ? String(format: ", and they suggested %.2f Hz",
                                                 deadHz) : "",
                             snapHz != rawHz
                                ? String(format: " — SNAPPED to the panel's %.2f Hz "
                                         + "(within %.1f%%)", snapHz, hzSnapTol * 100)
                                : " — no canonical rate within "
                                  + String(format: "%.1f%%, keeping the measurement",
                                           hzSnapTol * 100)))
            } else {
                NSLog(String(format: "[cp] THE DISPLAY RATE NEVER SETTLED — %.0f ms went by "
                             + "without %.0f ms of cadence agreeing with itself to within "
                             + "%.0f%%, so the guest is being paced off the median of all "
                             + "%d interval(s) instead. Treat any smoothness verdict from "
                             + "this session as confounded.",
                             hzSettleTimeoutMs, hzSettleMs, hzSettleTol * 100, gaps.count))
            }
        }
        if let mid = chosen, mid > 0 {
            displayPeriod = mid          // what the cadence check measures against
            let hz = Float(1.0 / mid)
            NSLog(String(format: "[cp] display %.2f Hz measured over %d frames",
                         hz, chosenGaps))
            // The measurement always lands, but a pinned guest keeps its own
            // rate — kl_ovrp_set_display_frequency records this as the panel
            // and says so once. The "guest will be told" line below is the one
            // that answers what the guest actually got.
            kl_ovrp_set_display_frequency(hz)
        } else {
            NSLog("[cp] could not measure the display rate — the guest keeps "
                  + "\(kl_ovrp_display_frequency()) Hz")
        }
        NSLog(String(format: "[cp] guest will be told %.2f Hz",
                     kl_ovrp_display_frequency()))
    }

    /// A drawable with nothing in it. The priming frames are shown to the user,
    /// so they have to be *something*; an unwritten drawable is whatever was in
    /// that texture last.
    private func clearAndPresent(_ drawable: LayerRenderer.Drawable) {
        guard let cmd = queue?.makeCommandBuffer() else { return }
        for view in drawable.views {
            let map = view.textureMap
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture     = drawable.colorTextures[map.textureIndex]
            pass.colorAttachments[0].slice       = map.sliceIndex
            pass.colorAttachments[0].loadAction  = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1)
            cmd.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        }
        drawable.encodePresent(commandBuffer: cmd)
        cmd.commit()
    }

    /// The measurement that says whether any of this is doing anything.
    ///
    /// `delta` is how far the head turned between the pose the picture was
    /// rendered with and the pose it is being displayed at — zero means the
    /// guest is keeping up and the pass is a blit, and a growing value with
    /// `stale` frames behind it means pictures are being reused, which is
    /// reprojection earning its place rather than a fault.
    /// Worst pose age seen since the last line, in milliseconds.
    private var worstAgeMs: Double = 0

    private func noteReprojection(rendered: kl_ovrp_render_pose?,
                                  originFromHead: simd_float4x4, stage: Int) {
        reprojectFrames += 1
        var delta: Float = 0
        if var r = rendered {
            delta = withUnsafePointer(to: &r) {
                kl_reproject_delta_degrees($0, originFromHead)
            }
        }
        worstDelta = max(worstDelta, delta)
        // How OLD the pose being corrected against is. An angle on its own
        // cannot separate a fast head from a stale pose — it is the product of
        // the two — and those want opposite fixes. One displayed frame at
        // 120 Hz is 8.3 ms; tens of milliseconds means the record is not this
        // frame's and the correction is aimed at the wrong instant.
        var ageMs: Double = 0
        if let sub = rendered?.submit_ns, sub != 0 {
            var ts = timespec()
            clock_gettime(CLOCK_MONOTONIC, &ts)
            let now = UInt64(ts.tv_sec) * 1_000_000_000 + UInt64(ts.tv_nsec)
            if now > sub { ageMs = Double(now - sub) / 1_000_000 }
        }
        worstAgeMs = max(worstAgeMs, ageMs)
        guard reprojectFrames % 90 == 0 else { return }
        NSLog(String(format: "[cp] timewarp: stage %d serial %llu, delta %.2f deg "
                             + "(worst %.2f), pose age %.1f ms (worst %.1f), "
                             + "%d stale in a row",
                     stage, rendered?.serial ?? 0, delta, worstDelta,
                     ageMs, worstAgeMs, staleInARow))
        worstDelta = 0
        worstAgeMs = 0
    }

    // MARK: - present order (round 5)

    /// KL_PRESENT_ORDER=strict|off. `strict` (default) selects the stage, the
    /// record and therefore the texture and the anchor as ONE tuple out of one
    /// lock, and refuses to present a guest frame older than the one already
    /// shown. `off` is the pre-round-5 arm, bit for bit, so the measurement
    /// below can be taken on both.
    private static let presentOrderStrict: Bool = {
        let e = ProcessInfo.processInfo.environment["KL_PRESENT_ORDER"] ?? ""
        let off = ["off", "0", "no", "none"].contains(e.lowercased())
        NSLog("[cp] KL_PRESENT_ORDER=\(e.isEmpty ? "strict (default)" : e) — "
              + (off ? "the stage index and the record are read SEPARATELY and a "
                       + "present may show a guest frame older than the last "
                       + "(the pre-round-5 arm)"
                     : "the record, its stage's texture and its anchor are one "
                       + "selection, and a present never goes backwards"))
        return !off
    }()

    /// The last record actually presented, so a regression can be detected and
    /// (under `strict`) held back. Only the frame loop touches it.
    private var lastPresentedRecord: kl_ovrp_render_pose?
    private var presentCount: UInt64 = 0
    /// When the current 300-present window opened, so the line below can
    /// report the rate the GUEST actually delivered rather than leaving it
    /// to arithmetic on `repeats`. This is the number round 19 turned on:
    /// distinct serials per second is the guest's true frame rate, and it
    /// is the one quantity no other instrument here prints.
    private var presentWindowStart: TimeInterval = 0
    private var presentRegressions: UInt64 = 0
    private var presentWorstRegression: UInt64 = 0
    private var presentRepeats: UInt64 = 0
    /// How many panel slots each DISTINCT guest frame occupied, bucketed
    /// 1..5 and 6+. This is the round-22 measurement: a smooth run is one
    /// bucket, a jittery run is two adjacent buckets in a ragged mix. Launch
    /// #20's menu was `1x:296` (smooth) and its gameplay `2x:~100 3x:~40`
    /// (jitters) in the same build with the same warp arm.
    private var presentHoldRun = 0
    private var presentHolds = [UInt64](repeating: 0, count: 7)
    private var presentMispairs: UInt64 = 0
    private var presentWorstMispair: UInt64 = 0
    private var presentAnchorMisses: UInt64 = 0
    private var presentHeldBack: UInt64 = 0
    private var presentNoRecord: UInt64 = 0
    /// Panel slots the guest had no new picture for and this loop bridged by
    /// re-presenting the held content — see `bridged` in the census note below.
    private var presentBridged: UInt64 = 0
    private var presentBridgeRun: UInt64 = 0
    private var presentWorstBridgeRun: UInt64 = 0
    private var loggedBridge = false

    // MARK: - the frame loop's own liveness (round 21)

    /// The slowest single iteration of the render loop, and how many were slow
    /// enough to matter. **This is a watchdog instrument, not a performance
    /// one.** visionOS kills an immersive app whose `cp_layer_renderer_t` has
    /// not submitted a frame for 2.0 seconds and leaves no crash report behind
    /// — `wakeboardd: killing [pid] because it hasn't been sending frames for
    /// 2.0s` in the system log is the entire evidence. So the question this has
    /// to be able to answer after the fact is "was any slot ever close to the
    /// line, and when", and the answer has to be printed *before* the kill.
    private var slotSlow: UInt64 = 0
    private var worstSlotMs: Double = 0
    private var lifetimeWorstSlotMs: Double = 0
    private var notedSlotStall = false
    /// A slot over this is not jitter — it is something in the loop blocking.
    /// 100 ms is twelve panel periods; a fifth of the way to the kill.
    private static let slotSlowMs = 100.0

    private func noteSlot(_ seconds: Double) {
        let ms = seconds * 1000
        if ms > worstSlotMs { worstSlotMs = ms }
        if ms > lifetimeWorstSlotMs { lifetimeWorstSlotMs = ms }
        guard ms >= Self.slotSlowMs else { return }
        slotSlow += 1
        guard !notedSlotStall else { return }
        notedSlotStall = true
        NSLog(String(format: "[cp] FRAME LOOP STALLED: one slot took %.0f ms "
                     + "(%.0f panel periods). The system kills this process at "
                     + "2000 ms with no crash report — anything in this loop "
                     + "that can block is fatal, and the first suspect is a "
                     + "synchronous XPC call (CoreHaptics, GameController, "
                     + "audio). See KleptonControllers, hapticQueue.", ms,
                     ms / 8.33))
    }


    // --- KL_GUEST_FPS_DIV — an EVEN guest cadence ---------------------------
    //
    // ROUND 22. What launch #20 measured, with `KL_WARP=system` genuinely
    // armed (`Frame 1 tag 76 -> cached anchor`, `anchor misses 0` on every
    // census) and the jitter still there:
    //
    //   menu      guest 118-120 fps | repeats 0    | bridged 16-18, worst run 1
    //             frozen series alternation index 0.28-0.46      -> SMOOTH
    //   gameplay  guest  45-60 fps  | repeats 145+ | bridged 145-188, worst run 2-3
    //             frozen series alternation index 1.17-1.59      -> JITTERS
    //
    // Same build, same 4032 eye, same warp arm, same everything. The ONE thing
    // that differs between the smooth half and the jittery half is the SHAPE of
    // the cadence: at the menu every guest frame occupies exactly one panel
    // slot; in gameplay each one occupies two slots or three, in a ragged mix.
    //
    // Why the warp arm could not fix that, and why neither arm ever will. Both
    // arms correct ROTATION exactly — `ours` by rebuilding the model-view,
    // `system` by handing the compositor the anchor the pixels embody. Neither
    // corrects what is left over, because what is left over is not a rotation:
    // the eye quad is a flat proxy at KL_REPROJECT_DEPTH, so reprojection has
    // no parallax to give and every error that scales with the frame's AGE
    // survives it. A constant age is invisible — it reads as latency, not
    // motion. An age that alternates 2, 3, 2, 3 slots is an image that alternates
    // between two positions, which is precisely what Austin describes and
    // precisely what `alternation index 1.3` says the pose series is doing.
    //
    // So this gate makes the age CONSTANT. `kl_app_guest_publish()` is what
    // wakes the guest for a frame (kl_app.c: the guest sleeps until
    // `published != consumed`, level-triggered, so extra publishes do not
    // queue), and releasing it only on a grid tick paces the guest's frame
    // STARTS onto that grid. A guest that beats the grid then delivers exactly
    // one frame per `div` slots, forever, and every frame is held exactly `div`
    // slots. A guest that overruns waits for the NEXT grid tick rather than
    // free-running, so the holds are div, 2*div, 3*div — still on the grid,
    // never the 2/3 ragged mix.
    //
    //   KL_GUEST_FPS_DIV unset|0|off   the gate is not built; publish every slot
    //   KL_GUEST_FPS_DIV=auto          adaptive (pttr's default, see kl_app.c)
    //   KL_GUEST_FPS_DIV=N             pinned to N panel slots per guest frame
    //
    // Adaptive, because no single divisor fits this title: the menu makes 120
    // (div 1), most of the bar makes 55-60 (div 2 is marginal, div 3 is
    // comfortable) and the busiest stretches make 45 (div 3). A pinned 2 across
    // that range would produce a 2/4 mix, which is the same disease with a
    // longer beat. So: start at 1, raise when the guest overruns the grid on
    // more than a quarter of publishes in a 128-publish window, lower again
    // after two consecutive windows under 3%. Capped at 4 (30 fps) — below that
    // the cure is worse than the disease and the answer is scene work, not
    // pacing.
    //
    // Trading rate for evenness is the whole point and it is a real trade: at
    // div 3 the gameplay that delivered a ragged 55 delivers an even 40. If
    // boot21 shows an even cadence and Austin still sees jitter, this
    // hypothesis is DEAD and the remaining suspects are the guest's own view
    // matrices (KL_OVRP_ZERO_HEAD_VEL as the A/B) — which is the point of
    // making the cadence readable either way.
    private static let paceSpec: (on: Bool, auto: Bool, fixed: Int) = {
        let e = (ProcessInfo.processInfo.environment["KL_GUEST_FPS_DIV"] ?? "")
                    .trimmingCharacters(in: .whitespaces).lowercased()
        if e.isEmpty || e == "0" || e == "off" { return (false, false, 0) }
        if e == "auto" || e == "adaptive" { return (true, true, 1) }
        if let n = Int(e), n >= 1, n <= 8 { return (true, false, n) }
        NSLog("[cp] KL_GUEST_FPS_DIV=\(e) is not a divisor, `auto` or `off` — "
              + "the guest cadence gate stays OFF")
        return (false, false, 0)
    }()
    private var paceDiv = 1
    private var paceSlots = 0
    private var paceLastSerial: UInt64 = 0
    private var paceWindowPublishes = 0
    private var paceWindowOverruns = 0
    private var paceGoodWindows = 0
    private var paceValve: UInt64 = 0
    private var paceOverruns: UInt64 = 0
    private var pacePublishes: UInt64 = 0
    private var paceLogged = false

    /// The gate. `true` releases the guest for one frame; `false` holds it to
    /// the grid. Always `true` when the gate is off, so the un-gated shape is
    /// byte-for-byte what it was.
    private func paceMayPublish() -> Bool {
        guard Self.paceSpec.on else { return true }
        if !paceLogged {
            paceLogged = true
            paceDiv = Self.paceSpec.auto ? 1 : Self.paceSpec.fixed
            NSLog("[cp] KL_GUEST_FPS_DIV="
                  + (Self.paceSpec.auto ? "auto" : "\(Self.paceSpec.fixed)")
                  + " — the guest is released on a grid of \(paceDiv) panel "
                  + "slot(s) per frame so every frame is held the SAME number "
                  + "of slots. Ragged 2/3 holds are the round-22 jitter "
                  + "suspect; `pace` on the present-order census says whether "
                  + "the grid held.")
        }
        paceSlots += 1

        // What the guest has actually finished. Before its first completed
        // frame there is nothing to pace and a great deal to lose: boot, the
        // OBB mount and the first scene load must run free, or the gate is a
        // deadlock wearing a pacer's clothes.
        var rec = kl_ovrp_render_pose()
        let have = withUnsafeMutablePointer(to: &rec) {
            kl_ovrp_last_complete_record($0)
        } != 0
        let serial = have ? rec.serial : 0
        if serial == 0 { paceSlots = 0; return true }
        let ready = serial != paceLastSerial

        // The safety valve. A guest inside a scene load delivers nothing for
        // hundreds of slots, and `ready` would stay false through all of them
        // — a gate that stops publishing to a stalled guest never restarts it.
        // Past eight grid periods the gate opens regardless. Counted, because
        // a valve that fires every frame means the gate is not pacing anything.
        if paceSlots >= paceDiv * 8 {
            paceValve += 1
            paceRelease(serial)
            return true
        }
        guard paceSlots % paceDiv == 0 else { return false }
        // Overran this tick: wait for the next multiple rather than releasing
        // late, which is what keeps the holds on the grid instead of ragged.
        guard ready else { return false }
        paceRelease(serial)
        return true
    }

    /// The gate's own line on the present-order census — empty when the gate
    /// is off, so an un-gated log reads exactly as it did.
    private func paceLine() -> String {
        guard Self.paceSpec.on else { return "" }
        let n = pacePublishes
        pacePublishes = 0
        let o = paceOverruns
        paceOverruns = 0
        let v = paceValve
        paceValve = 0
        return String(format: " | pace grid %d slots, %llu released, %llu "
                      + "overran, %llu valve", paceDiv, n, o, v)
    }

    private func paceRelease(_ serial: UInt64) {
        pacePublishes += 1
        paceWindowPublishes += 1
        if paceSlots > paceDiv {
            paceOverruns += 1
            paceWindowOverruns += 1
        }
        paceLastSerial = serial
        paceSlots = 0
        guard Self.paceSpec.auto, paceWindowPublishes >= 128 else { return }
        let rate = Double(paceWindowOverruns) / Double(paceWindowPublishes)
        let was = paceDiv
        if rate > 0.25 && paceDiv < 4 {
            paceDiv += 1
            paceGoodWindows = 0
        } else if rate < 0.03 && paceDiv > 1 {
            paceGoodWindows += 1
            if paceGoodWindows >= 2 { paceDiv -= 1; paceGoodWindows = 0 }
        } else {
            paceGoodWindows = 0
        }
        paceWindowPublishes = 0
        paceWindowOverruns = 0
        if paceDiv != was {
            NSLog(String(format: "[cp] guest cadence grid %d -> %d slots per "
                         + "frame (%.0f%% of the last window overran); the "
                         + "guest is now paced at %.1f fps against a %.1f Hz "
                         + "panel, EVENLY", was, paceDiv, rate * 100,
                         displayPeriod > 0 ? 1.0 / (displayPeriod * Double(paceDiv)) : 0,
                         displayPeriod > 0 ? 1.0 / displayPeriod : 0))
        }
    }

    /// **Which guest frame is on the panel at each slot, and is its texture the
    /// one that frame drew?**
    ///
    /// Every per-frame instrument in this hunt reads clean — pose, projection,
    /// warp. What none of them can see is the SEQUENCE. If the panel shows
    /// serials interleaved (N, N-1, N, N+1) or pairs texture N with record N±1,
    /// each frame is individually correct and the world still alternates between
    /// two positions exactly one guest frame of head rotation apart. That is the
    /// symptom, so this is the measurement.
    ///
    /// - `regressions` a serial lower than the previous present's. Under
    ///   `strict` this is what is refused, and the refusal is `held back`.
    /// - `repeats` the same serial twice. **Expected and not a fault** — a 72 Hz
    ///   guest against a 120 Hz panel repeats roughly two presents in five.
    /// - `mispairs` the record's `stage_epoch` against the live count for that
    ///   stage: the guest has claimed that texture again since, so the pixels
    ///   being sampled are NOT the ones the record describes. Structurally
    ///   reachable on this branch — the Vulkan defer ring is four deep against
    ///   three stages, so a record can still be in flight when the guest starts
    ///   redrawing its stage. Worst is by how many guest frames.
    /// - `anchor misses` KL_WARP=system fell back to the display anchor, i.e.
    ///   to our own warp, for that present.
    /// - `bridged` panel slots the guest had **no new picture** for, which this
    ///   loop crossed by re-presenting the content already in hand against a
    ///   fresh pose. Not a fault — it is the whole reason the present path never
    ///   waits on the guest — and the run length is the number that matters: a
    ///   run of ~120 is a full second of guest stall (a scene load, an IL2CPP
    ///   hitch) crossed without the layer renderer ever going quiet. The 2.0 s
    ///   no-frames kill is what happens when a loop does NOT do this, so `worst
    ///   run` is the margin against it stated in slots.
    private func notePresentOrder(rendered: kl_ovrp_render_pose?, stage: Int,
                                  anchorHit: Bool, bridged: Bool) {
        presentCount += 1
        if bridged {
            presentBridged += 1
            presentBridgeRun += 1
            if presentBridgeRun > presentWorstBridgeRun {
                presentWorstBridgeRun = presentBridgeRun
            }
            // Once, the first time a stall long enough to be a scene load is
            // crossed on held content. Twelve slots is a tenth of a second —
            // the age at which wakeboardd starts counting ("past mitigate age
            // after 0.100000s") — so this is the log line that says the bridge
            // is what stands between a load and the kill.
            if presentBridgeRun == 12 && !loggedBridge {
                loggedBridge = true
                NSLog("[cp] stall bridge: the guest has had no new frame for 12 "
                      + "panel slots (0.1 s) and every one of them was presented "
                      + "from the picture already in hand, reprojected to a fresh "
                      + "pose. The layer renderer never goes quiet on a guest "
                      + "stall; `bridged` on the present-order census counts it.")
            }
        } else {
            presentBridgeRun = 0
        }
        if let r = rendered {
            if let prev = lastPresentedRecord {
                if r.serial < prev.serial {
                    presentRegressions += 1
                    presentWorstRegression = max(presentWorstRegression,
                                                 prev.serial - r.serial)
                } else if r.serial == prev.serial {
                    presentRepeats += 1
                    presentHoldRun += 1
                } else {
                    // A NEW guest frame reached the panel: the one before it
                    // is finished and its hold length is now known.
                    presentHolds[min(max(presentHoldRun, 1), 6)] += 1
                    presentHoldRun = 1
                }
            } else {
                presentHoldRun = 1
            }
            let live = kl_ovrp_stage_epoch_now(Int32(stage))
            if r.stage_epoch != 0 && live > r.stage_epoch {
                presentMispairs += 1
                presentWorstMispair = max(presentWorstMispair, live - r.stage_epoch)
            }
            if Self.warpBySystem && !anchorHit { presentAnchorMisses += 1 }
            lastPresentedRecord = r
        } else {
            presentNoRecord += 1
        }

        if presentCount == 1 { presentWindowStart = CACurrentMediaTime() }
        guard presentCount % 300 == 0 else { return }
        let shown = presentCount - presentNoRecord
        // The guest's DELIVERED rate. A present whose serial repeats the
        // previous one is a panel slot the guest had no new picture for, so
        // `presents - repeats - no record` over the window's own wall clock is
        // exactly how many distinct guest frames reached the display per
        // second. It is not what the guest was TOLD (KL_DISPLAY_HZ) and not
        // what the panel runs at (the cadence line); when those three disagree
        // the difference is the whole story, and until round 19 it had to be
        // computed by hand from `repeats`.
        let windowSecs = max(1e-6, CACurrentMediaTime() - presentWindowStart)
        let distinct = shown > presentRepeats ? shown - presentRepeats : 0
        let guestFps = Double(distinct) / windowSecs
        let monotonic = shown > 0
            ? 100.0 * Double(shown - presentRegressions) / Double(shown) : 100.0
        // No defer-ring counters here: this branch files a frame's record
        // straight from ovrp_EndFrame4, so there is no in-flight queue to
        // report. The port that carries the GPU-completion deferral prints
        // them beside these.
        NSLog(String(format: "[cp] present order: %llu presents, serials "
                             + "monotonic %.0f%% | regressions %llu (worst -%llu) "
                             + "| repeats %llu | tex/record mispairs %llu "
                             + "(worst +%llu) | anchor misses %llu | held back "
                             + "%llu | no record %llu | bridged %llu (worst run "
                             + "%llu) | slot worst %.0f ms (%llu over %.0f) "
                             + "| GUEST DELIVERED %llu distinct in %.2f s "
                             + "= %.1f fps | holds 1x:%llu 2x:%llu 3x:%llu "
                             + "4x:%llu 5x:%llu 6+x:%llu%@",
                     presentCount, monotonic, presentRegressions,
                     presentWorstRegression, presentRepeats, presentMispairs,
                     presentWorstMispair, presentAnchorMisses, presentHeldBack,
                     presentNoRecord, presentBridged, presentWorstBridgeRun,
                     worstSlotMs, slotSlow, Self.slotSlowMs,
                     distinct, windowSecs, guestFps,
                     presentHolds[1], presentHolds[2], presentHolds[3],
                     presentHolds[4], presentHolds[5], presentHolds[6],
                     paceLine()))
        if let h = controllers.hapticsSummary() { NSLog("[cp] \(h)") }
        presentCount = 0; presentNoRecord = 0
        presentWindowStart = CACurrentMediaTime()
        presentRegressions = 0; presentWorstRegression = 0
        presentRepeats = 0
        presentMispairs = 0; presentWorstMispair = 0
        for i in presentHolds.indices { presentHolds[i] = 0 }
        presentAnchorMisses = 0; presentHeldBack = 0
        // The bridge RUN deliberately survives the window boundary: a stall
        // does not stop being one because 300 presents happened to elapse
        // inside it. Only the totals reset.
        presentBridged = 0; presentWorstBridgeRun = presentBridgeRun
        slotSlow = 0; worstSlotMs = 0
    }
}
