import SwiftUI
import Foundation
import CompositorServices

// The visionOS host app (Swift for the platform layer).
//
// The boot gate is deliberately a plain WindowGroup and nothing else: the gate is "the
// guest boots inside an app bundle under AMFI, and a veneer executes", and an
// ImmersiveSpace would have put Compositor Services into the picture before
// there was anything to present, so a failure in either half would have read
// as a failure of the other.
//
// The immersive path sits BESIDE it rather than in place of it, behind
// KL_IMMERSIVE. The window still boots and still reports, so the boot measurement
// stays takeable on the same binary — which matters because that measurement
// is how a device regression gets localised.

/// A KL_* knob read by value, not by presence — the Swift twin of
/// `kl_env_on()`. Both defaults below are ON, so `KL_FOO=0` has to be a real
/// way to say no rather than a second way to say yes.
func klEnvOn(_ name: String, default dflt: Bool) -> Bool {
    guard let v = ProcessInfo.processInfo.environment[name] else { return dflt }
    return !["", "0", "no", "off", "false"].contains(v.lowercased())
}

/// The same for a scalar — `kl_env_float()`'s twin. An unparseable value falls
/// back to the default rather than to zero: a typo in a gain knob that silently
/// means "off" is the kind of thing that gets diagnosed as broken hardware.
func klEnvFloat(_ name: String, _ dflt: Float) -> Float {
    guard let v = ProcessInfo.processInfo.environment[name], let f = Float(v) else { return dflt }
    return f
}

enum Immersive {
    static let id = "KleptonImmersive"
    // Default ON for Beat Saber, OFF for Steam Link, and the difference is not a
    // preference — it is what each app can currently put on screen.
    //
    // Klepton runs a VR title, so the immersive space IS the app: launching from
    // the Home View and getting a window with a Boot button is a test harness,
    // not a product. The window-and-report shape stays exactly one knob away
    // (KL_IMMERSIVE=0) because it is the measurement that localises a device
    // regression.
    //
    // Steam Link is the other way round until its 2D shell has a window. Its VR
    // half cannot draw anything without an authorized session, and a session
    // arrives by hand (KL_SLINK_SARGS) — so the default launch opens an
    // immersive space that is black by construction and hides the one surface
    // with information on it. KL_IMMERSIVE=1 turns it on for a run that HAS a
    // session, which is the run that wants it.
    //
    // Reads kl_app_target_is_steamlink(), so it is only meaningful after
    // kl_app_configure. Both call sites are inside boot(), after configure has
    // returned; the scene body reads `mixed`, not this.
    static var wanted: Bool {
        // The native guest (Minecraft): the player's play-mode choice, committed
        // by BootView.boot() before the first configure (MCPlayMode.swift).
        // MCPlayMode.apply() also setenv()s KL_IMMERSIVE and writes it to
        // klepton-env.txt, so kl_app.c's native_vr() reads the same answer at
        // configure, argv, boot and re-run. Windowed unless chosen otherwise.
        if kl_app_target_is_native() != 0, let mode = MCPlayMode.applied { return mode.immersive }
        // Every other guest: VR by default except Steam Link, as before.
        return klEnvOn("KL_IMMERSIVE", default: kl_app_target_is_steamlink() == 0)
    }

    // .mixed or .full, decided per target — and this is the setting the room
    // breaks through.
    //
    // .mixed was the right default while the picture was being *found*: the guest
    // renders an opaque world, so passthrough shows through nowhere it matters,
    // and seeing the room makes scale, distance and IPD judgeable against real
    // surroundings instead of against a black void.
    //
    // It is the wrong default for a title someone is PLAYING. In .mixed the
    // system is compositing the room with our content every frame, and it decides
    // per pixel and per moment what wins — so a person who walks in, a monitor, a
    // desk edge flicker into the scene. That is not visionOS's close-range people
    // awareness (which is a safety feature and cannot be turned off, see
    // superhot/STATUS.md); it is us asking for the room and then being surprised
    // to get it. Phase 11: Austin played SUPERHOT worn and reported exactly this.
    //
    // So the default is now the target's own answer — `opaque_world` in kl_app.c's
    // TARGETS table, on for superhot and off for the other two, which leaves Beat
    // Saber and Steam Link behaving precisely as they did. KL_FULL is unchanged
    // and still decides it outright in either direction: KL_FULL=1 forces .full,
    // KL_FULL=0 forces .mixed, and unset means "whatever this title wants".
    static var mixed: Bool {
        !klEnvOn("KL_FULL", default: kl_app_target_opaque_world() != 0)
    }

    /// Whether the wearer's real arms and hands are drawn over the immersive
    /// scene.
    ///
    /// visionOS shows them by default, which is right for an app the wearer
    /// reaches into and wrong for a VR title that models the hands itself:
    /// SUPERHOT draws low-poly hands, puts guns in them and throws them, and the
    /// real pair arrives in the same place, tracked slightly differently, doing
    /// something slightly different. Two left hands, disagreeing.
    ///
    /// `.hidden` for a target whose guest draws its own (`own_hands` in kl_app.c),
    /// `.automatic` otherwise — so Beat Saber's behaviour is unchanged by this
    /// commit, and a BS player who wants it can flip it the same way.
    ///
    /// **One answer, applied once.** The scene used to carry two
    /// `.upperLimbVisibility` modifiers — this target default and the Matting
    /// panel's toggle — that disagreed for a target with `own_hands` and left
    /// which of them won to SwiftUI. Now, in order:
    ///
    ///   1. `KL_HANDS`, when set, decides outright: `KL_HANDS=1` asks for the
    ///      real limbs `.visible` (useful for judging whether the guest's hands
    ///      track the same pose), `KL_HANDS=0` hides them.
    ///   2. The Matting panel's hand-matting toggle, when OFF, hides them. It is
    ///      a person's saved choice; it can take limbs away, never add them over
    ///      a guest that draws its own.
    ///   3. The native guest (Minecraft) asks for them `.visible`: the wearer
    ///      wants their real arms, hands and held Sense controllers over the
    ///      world, and `.automatic` leaves that to a system default that full
    ///      immersion does not promise.
    ///   4. `own_hands` hides them; anything else is `.automatic`.
    ///
    /// This is the system's passthrough of the real limbs only. The 6DoF hand
    /// and Sense poses the guest draws with come from ARKit (KleptonCompositor
    /// runARKit) and are independent of it in both directions. And `.visible`
    /// is a request: the system may still not draw them while the space is not
    /// on screen.
    static func upperLimbs(handMatting: Bool) -> Visibility {
        if ProcessInfo.processInfo.environment["KL_HANDS"] != nil {
            return klEnvOn("KL_HANDS", default: true) ? .visible : .hidden
        }
        if !handMatting { return .hidden }
        if kl_app_target_is_native() != 0 { return .visible }
        return kl_app_target_own_hands() != 0 ? .hidden : .automatic
    }

    // How many times the CompositorLayer closure has been entered. SwiftUI may
    // re-evaluate a scene, and a second layer would mean the render loop the log
    // describes is not the one on screen.
    nonisolated(unsafe) private static var closures = 0
    static func bump() -> Int { closures += 1; return closures }

    /// Whether the system's persistent overlays — the Home indicator, and the
    /// hand-gesture affordance that fades in beneath it — stay on top of the
    /// guest's picture.
    ///
    /// Hidden by default. The overlay is drawn by the system *over* the
    /// immersive scene, and both guests here put their own interactive content
    /// exactly where it lands: Beat Saber's lower menu row and Steam Link's
    /// dashboard toolbar are both near the bottom of the field, so the overlay
    /// sits on the controls rather than beside them. It also reappears on every
    /// hand raise, which for a title driven entirely by raised hands is
    /// continuous.
    ///
    /// `.hidden` is a request, not a guarantee — the system still shows the
    /// indicator at moments it considers mandatory (the first seconds of a
    /// space, a pending system alert), which is why this is the same `Visibility`
    /// ALVR passes rather than a claim that it is gone. `KL_OVERLAYS=1` puts it
    /// back, which is what a run wants when the question is whether the system
    /// still thinks our space is on screen at all.
    static var systemOverlays: Visibility {
        klEnvOn("KL_OVERLAYS", default: false) ? .automatic : .hidden
    }
}

/// What backgrounding means for this process: the end of it.
///
/// ALVR's shape, and for the same reason. Everything the guest is holding when
/// the app goes away is either unresumable or expensive to re-establish — the
/// ARKit session, the Compositor Services layer, the ANGLE context and the eye
/// swapchain behind it, FMOD's OpenSL player, and a Unity engine that has been
/// told it is on a Quest and never expects the display to leave. Resuming that
/// correctly is a project of its own; resuming it *incorrectly* is a class of
/// bug that reports itself as "the second run is broken" long after the cause.
/// Exiting makes every launch the first launch.
///
/// `KL_EXIT_ON_BACKGROUND=0` keeps the old behaviour, which is what a debugging
/// session wants when the headset comes off with a capture still open.
///
/// **Except the native guest's world, while its layer is still ours.** Closing
/// the Minecraft launcher window while the world plays takes the app's phase to
/// `.background` with the space still presenting (build 69, on device), and
/// exiting there killed a game the wearer was in. So for that guest the exit
/// waits while the immersive layer is running or resumably paused, and is
/// taken, on the original terms, the moment the layer ends or the game's run
/// does. The decision is KleptonScenePolicy.lifetime; this enum only carries
/// it out. Every other guest exits on `.background` exactly as before.
enum Lifecycle {
    static func scenePhaseChanged(to phase: ScenePhase) {
        NSLog("[app] scene phase -> \(phase)")
        // The phase is one of the audio's two inputs, not its decision. This
        // used to silence on every .inactive and only .active undid it — so
        // closing the launcher window while the immersive space played muted
        // the game for good: the phase left .active and, with no window left,
        // never came back. While an immersive layer exists its state decides
        // (a crown press or Home pauses/ends it and that silences); with none,
        // the phase does, as before. See KleptonScenePolicy.
        // The audio half also decides nothing about exiting — reconsider()
        // below does, from the same two inputs.
        //
        // .active is also the audio's cue to rebuild, and it needs one: this OS
        // silently stops calling CoreAudio's render callback across a scene
        // transition with no error and no interruption notification. See
        // kl_audio_resume; the compositor hooks the immersive half of the same
        // transition, and kl_audio's heartbeat catches whatever neither of them
        // sees. Unconditional rather than "only if we were away" — a rebuild of
        // a healthy unit costs a few milliseconds and cannot go wrong.
        switch phase {
        case .active:     KleptonAudio.scenePhaseChanged(to: .active)
        case .background: KleptonAudio.scenePhaseChanged(to: .background)
        default:          KleptonAudio.scenePhaseChanged(to: .inactive)
        }
        // And the controllers need one for the same reason the audio does.
        // GameController stops forwarding events to an app that is not
        // frontmost, and a glanced-at visionOS notification takes the
        // foreground away without ever backgrounding us — so `.active` is the
        // only signal this process gets that input may need re-establishing.
        // See KleptonControllers.appBecameActive.
        if phase == .active { KleptonControllers.appBecameActive() }
        if phase == .background {
            // The pipeline cache save is throttled (kl_vulkan.c); write its
            // tail now, while the process is certainly still alive to do it.
            kl_vulkan_pipeline_cache_flush()
            if !exitOnBackground {
                NSLog("[app] backgrounded; KL_EXIT_ON_BACKGROUND=0, staying alive")
            }
        }
        reconsider("scene phase \(phase)", grace: false)
    }

    private static var exitOnBackground: Bool { klEnvOn("KL_EXIT_ON_BACKGROUND", default: true) }

    // The native run count when the keep-alive began (nil = not keeping alive),
    // the poll that watches for the game ending with no window left to see it,
    // and a deferred exit already in flight. Main thread only.
    nonisolated(unsafe) private static var keptAtRuns: Int32?
    nonisolated(unsafe) private static var keepAlivePoll: Timer?
    nonisolated(unsafe) private static var exitPending = false

    /// Re-decide the process's lifetime. Called on every scene phase, on every
    /// immersive layer change (KleptonAudio.layerChanged — the half that still
    /// fires once the launcher window, and the phase handler under it, are
    /// gone), and once a second while kept alive. Main thread.
    ///
    /// `grace` is for a layer that has just ended: its render thread is still
    /// stopping ARKit and joining the guest (renderLoop's tail), so the exit
    /// waits a bounded 2 s and is then re-decided rather than taken blind.
    static func reconsider(_ why: String, grace: Bool) {
        let native = kl_app_target_is_native() != 0
        // The game's run has ended since we started keeping it alive: Quit Game
        // with the launcher window closed. BootView's poll would normally answer
        // that, and it went with the window. Its own Quitting… window is bounded
        // in C (KL_MC_QUIT_GRACE_MS), so waiting for the count is bounded too.
        let gameOver = native && keptAtRuns.map { kl_app_native_runs() > $0 } == true
        let decision = KleptonScenePolicy.lifetime(
            phase: KleptonAudio.phase, layer: KleptonAudio.layer, nativeGuest: native,
            exitOnBackground: exitOnBackground, gameOver: gameOver)
        switch decision {
        case .carryOn:
            if keptAtRuns != nil { NSLog("[app] keep-alive over (\(why)) — \(KleptonAudio.phase.rawValue)") }
            keepAlivePoll?.invalidate(); keepAlivePoll = nil
            keptAtRuns = nil
        case .keepForImmersive:
            guard keptAtRuns == nil else { return }
            keptAtRuns = kl_app_native_runs()
            NSLog("[app] backgrounded (\(why)) but the immersive layer is "
                  + "\(KleptonAudio.layer.rawValue) — NOT exiting; the exit follows the layer or the game ending")
            // With grace: whatever it finds (the game's run over, the layer
            // ended) takes the same bounded 2 s as a layer end, and cannot cut
            // short one already running.
            keepAlivePoll = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
                reconsider("keep-alive poll", grace: true)
            }
        case .exit:
            if grace {
                guard !exitPending else { return }
                exitPending = true
                NSLog("[app] backgrounded and \(why) — exiting in 2 s unless the space comes back")
                DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                    exitPending = false
                    reconsider("\(why), after grace", grace: false)
                }
                return
            }
            exitNow(why)
        }
    }

    private static func exitNow(_ why: String) {
        NSLog("[app] backgrounded — exiting (KL_EXIT_ON_BACKGROUND; \(why))")
        // The last word on whether the run made a sound. This is the ONLY exit a
        // headset run ever takes — the report at the end of kl_app_lifecycle
        // belongs to the synthetic sweep and has never run on device — so if the
        // audio numbers are not printed here they are not printed at all.
        kl_audio_heartbeat(stderr)
        // Flush before exit rather than relying on it. The guest's log is a
        // file in the container (kl_app.c redirects stdout/stderr there) and it
        // is the only account of the run that survives; `exit` does flush
        // stdio, but it also runs atexit handlers and static destructors inside
        // a guest whose threads are still live, and one of those blocking would
        // turn a clean exit into a watchdog kill with a truncated log.
        fflush(nil)
        exit(0)
    }
}

@main
struct KleptonApp: App {
    init() {
        // Before any view reads launch.txt. A bundled payload with an empty
        // container has to be a game dir by the time the launcher appears.
        KleptonPayload.materialize()
    }
    @Environment(\.scenePhase) private var scenePhase
    /// Observed so the Matting panel's hand-matting toggle re-evaluates the
    /// immersive scene's `.upperLimbVisibility` live. The object is the shared
    /// singleton the panel edits, not a second copy.
    @ObservedObject private var chroma = KleptonChroma.shared

    var body: some Scene {
        WindowGroup { BootView() }
            .defaultSize(width: 1280, height: 800)
            // On the app's phase, not this window's: closing the boot window
            // while the immersive space is up is not backgrounding, and must
            // not be treated as it. The log line above every decision is what
            // makes that distinction checkable on a device rather than assumed.
            .onChange(of: scenePhase) { _, phase in Lifecycle.scenePhaseChanged(to: phase) }

        ImmersiveSpace(id: Immersive.id) {
            CompositorLayer(configuration: KleptonStageConfiguration()) { layerRenderer in
                // How many times this closure runs, and for which renderer. If
                // it runs twice, the loop we watch in the log is not necessarily
                // the layer being displayed — which would explain a correct pass
                // that nobody sees.
                NSLog("[cp] CompositorLayer closure #\(Immersive.bump()) "
                      + "renderer=\(ObjectIdentifier(layerRenderer))")
                KleptonCompositor(layerRenderer).startRenderLoop()
            }
        }
        // Per target now — see Immersive.mixed. Superhot asks for .full so the
        // room cannot composite into a game being played; Beat Saber and Steam
        // Link keep .mixed. KL_FULL decides it outright either way.
        .immersionStyle(selection: .constant(Immersive.mixed ? .mixed : .full),
                        in: .mixed, .full)
        // The wearer's real arms — see Immersive.upperLimbs, which folds the
        // target default, KL_HANDS and the Matting panel's toggle into ONE
        // answer (two modifiers here used to disagree). On the Scene, like
        // persistentSystemOverlays below and for the same reason: a
        // CompositorLayer has no view hierarchy to attach a View-level modifier
        // to. Read from the observed settings, so the toggle takes effect
        // without a relaunch.
        .upperLimbVisibility(Immersive.upperLimbs(handMatting: chroma.settings.handMatting))
        // See Immersive.systemOverlays. On the scene, not on a view inside it:
        // a CompositorLayer has no view hierarchy for the View-level modifier to
        // attach to, so the Scene-level one is the only one that applies here.
        .persistentSystemOverlays(Immersive.systemOverlays)
    }
}

/// Where the runtime's two roots live.
///
/// Guest code stays in the bundle. A vivecraft build with KL_BUNDLE_PAYLOAD
/// also carries the read-only game payload (jars, assets) in klepton-payload/
/// and symlinks it from Documents on first launch (KleptonPayload.swift).
/// Other targets still stage assets into the container: AMFI accepts a dylib
/// inside a bundle we signed, and the big asset trees carry no code.
enum Paths {
    static var resources: String { Bundle.main.bundlePath }
    static var container: String {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
    }
}

/// The launcher's boot state, one per PROCESS rather than one per window.
///
/// BootView used to keep all of this in @State, which belongs to the window.
/// The native guest's world now outlives a closed launcher (Lifecycle keeps
/// the process for the immersive layer), so the window can be destroyed and
/// recreated mid-game, and a fresh BootView read running=false,
/// finished=false, openedSpace=false, gamesAnswered=0. Launch was enabled
/// again (boot() would configure and kl_app_boot a second time and report
/// "already run in this process" over a game being played); KL_AUTOBOOT=1
/// re-prepared the launch arguments and booted on its own; Quit Game could
/// not dismiss a space "this window" never opened; and a gamesAnswered of 0
/// against a native_runs already past it would answer an OLD game's end by
/// dismissing the CURRENT game's space.
///
/// Main actor, like MCLauncherModel and MSAAccounts beside it. Every write is
/// BootView's, on the main thread — the boot thread's results arrive through
/// DispatchQueue.main.async exactly as they did into @State.
@MainActor
final class BootSession: ObservableObject {
    static let shared = BootSession()
    /// Internal rather than private so a test can start from a fresh one.
    init() {}

    @Published var log = ""
    @Published var status = "idle"
    @Published var running = false
    @Published var finished = false
    @Published var succeeded = false

    /// The immersive space this process asked for and has not given back.
    private(set) var openedSpace = false
    /// The 2D -> VR handoff, answered once.
    private(set) var handedOff = false
    /// How many of the native guest's returns have been answered
    /// (kl_app_native_runs counts them in C).
    private(set) var gamesAnswered: Int32 = 0
    /// KL_AUTOBOOT is "boot this launch", once per process — not once per
    /// window that happens to appear.
    private(set) var autobootTaken = false

    /// boot()'s own guard, asked BEFORE anything is prepared: no launch while
    /// one is starting (`running`) or has happened and not ended (`finished`).
    /// gameEnded() clears both, which is what lets a quit-to-launcher re-run
    /// through; a failed boot leaves `finished`, which was already final.
    var mayLaunch: Bool { !running && !finished }

    func claimAutoboot() -> Bool {
        guard !autobootTaken else { return false }
        autobootTaken = true
        return true
    }

    func claimHandoff() -> Bool {
        guard !handedOff else { return false }
        handedOff = true
        return true
    }

    /// Claim the space before asking for it, so two paths (or two windows)
    /// cannot both open it.
    func claimSpace() -> Bool {
        guard !openedSpace else { return false }
        openedSpace = true
        return true
    }

    /// What openImmersiveSpace answered. Anything but `.opened` leaves no space
    /// to dismiss, so the claim is released — once, no retry here; the next
    /// Launch or re-run may claim again. An `.opened` that lands after the
    /// claim was already given back (Quit Game raced the open) is not re-taken.
    func spaceOpened(_ opened: Bool) {
        if !opened { openedSpace = false }
    }

    /// Give the space back; true if there was one to dismiss.
    func releaseSpace() -> Bool {
        guard openedSpace else { return false }
        openedSpace = false
        return true
    }

    /// True once per finished native run. Taken before the await that
    /// answers it, so a second window's poll cannot answer the same end.
    func claimGameEnd(runs: Int32) -> Bool {
        guard runs > gamesAnswered else { return false }
        gamesAnswered = runs
        return true
    }
}

struct BootView: View {
    // Process state lives in BootSession (see there); these forward to it so
    // every read and write below — and the launcher's arguments in `body` —
    // keeps its name and its shape.
    @ObservedObject private var session = BootSession.shared
    private var log: String {
        get { session.log } nonmutating set { session.log = newValue } }
    private var status: String {
        get { session.status } nonmutating set { session.status = newValue } }
    private var running: Bool {
        get { session.running } nonmutating set { session.running = newValue } }
    private var finished: Bool {
        get { session.finished } nonmutating set { session.finished = newValue } }
    private var succeeded: Bool {
        get { session.succeeded } nonmutating set { session.succeeded = newValue } }
    // The flat guest's picture, and the handoff that ends it. Polled rather
    // than pushed: both are C state written by a guest thread, and a callback
    // out of one into SwiftUI would be a main-actor hop from a thread that is
    // mid-frame. A timer at 5 Hz costs nothing and cannot deadlock.
    // showShell stays per window: it is re-derived from C on every poll.
    @State private var showShell = false
    // The Minecraft row's themed launcher (KleptonMCLauncher.swift) stands in
    // front of the report; "Details" flips to the report and back.
    @ObservedObject private var launcher = MCLauncherModel.shared
    @ObservedObject private var accounts = MSAAccounts.shared
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    var body: some View {
        Group {
            if showShell { ShellWindow() }
            // Minecraft always opens its product launcher. Technical reports
            // remain in diagnostics exports and the other runtime targets.
            else if MSAAccounts.isMinecraft || (MCLauncher.enabled && !launcher.details) {
                MCLauncherView(running: running, finished: finished, succeeded: succeeded, status: status,
                               onLaunch: { launch() },
                               onDetails: { NSLog("[launcher] diagnostics are available as an export") })
            } else { bootReport }
        }
        .task { await watchPresentation() }
        .task { await autobootIfAsked() }
    }

    /// Boots on its own. Tapping an app and then tapping Boot is a harness,
    /// not a product — and the scripted paths (`visionos/run.sh`) wanted
    /// this anyway, which is what KL_AUTOBOOT was for. `KL_AUTOBOOT=0`
    /// restores the button-only shape for hand-driven debugging, where the
    /// point is to attach or start a capture before the guest runs.
    ///
    /// **The Minecraft row never autoboots (build 71).** Its play mode is a
    /// choice the player makes with the labelled Play action, every session;
    /// a remembered Immersive VR choice must not start a game on its own, and
    /// neither KL_AUTOBOOT=1 nor KL_LAUNCHER=0 may answer for the player. A
    /// deliberate scripted test still exists: KL_LAUNCHER_SCRIPT's explicit
    /// `mode=…;launch` steps go through the same Play action. Every other
    /// guest keeps its autoboot exactly as before.
    private func autobootIfAsked() async {
        // The gate reads the store, so the list (and deviceUnlocked) has to be
        // loaded before either the button or an autoboot decides. KL_MSA=0
        // still reloads: that knob skips the session, not the gate.
        if MSAAccounts.isMinecraft || MSAAccounts.wanted {
            await MSAAccounts.shared.reload()
        }
        if MSAAccounts.isMinecraft {
            if klEnvOn("KL_AUTOBOOT", default: false) || MCLauncher.autoboot {
                NSLog("[launcher] KL_AUTOBOOT is ignored for Minecraft (build 71): the game starts only from the Play action")
            }
            return
        }
        guard klEnvOn("KL_AUTOBOOT", default: true) else { return }
        guard autobootMayProceed() else { return }
        guard OwnershipGate.allowsLaunch() else {
            OwnershipGate.recordRefusal()
            return
        }
        // A few seconds to pick another account (KL_MSA_CHOOSER_SECONDS);
        // touching the panel holds the boot for the Boot button.
        if MSAAccounts.wanted, !(await MSAAccounts.shared.countdownThenBoot()) { return }
        guard launchMayProceed("autoboot") else { return }
        boot()
    }

    private func autobootMayProceed() -> Bool {
        guard session.claimAutoboot() else {
            NSLog("[launcher] autoboot already taken in this process — not again for a reopened window")
            return false
        }
        return launchMayProceed("autoboot")
    }

    /// The idempotence guard, and it sits BEFORE prepareForLaunch and the
    /// profile seeding because those are the mutations (launch arguments,
    /// game dir, env) that must not be redone under a game that is running.
    /// boot()'s own guard is too late for them. Ahead of the ownership gate,
    /// too, so an ignored press records no refusal.
    private func launchMayProceed(_ who: String) -> Bool {
        guard session.mayLaunch else {
            NSLog("[launcher] \(who) ignored — a game is already launching or running in this process "
                  + "(running=\(running) finished=\(finished))")
            return false
        }
        return true
    }

    /// A person pressed Launch/Boot: the launcher's choices first.
    private func launch() {
        guard launchMayProceed("Launch") else { return }
        guard OwnershipGate.allowsLaunch() else {
            OwnershipGate.recordRefusal()
            return
        }
        if MCLauncher.enabled { MCLauncherModel.shared.prepareForLaunch(applyPlay: true) }
        else if MSAAccounts.isMinecraft {                                     // KL_LAUNCHER=0: same game dirs,
            MCProfiles.prepareForLaunch()                                     // same play-mode rule
            MCPlayMode.apply(MCLauncherModel.shared.playMode)
        }
        boot()
    }

    /// Which of the two things the window can be showing, decided by what the
    /// guest is actually producing rather than by a knob.
    ///
    /// kl_present observes the mode — a window surface was created, or an eye
    /// texture was set up — so a guest that surprises us is described correctly
    /// rather than according to a flag someone remembered to set. The 2D->VR
    /// handoff is the transition kl_present.h was written for.
    private func watchPresentation() async {
        while !Task.isCancelled {
            let mono = kl_present_mode_now() == KL_PRESENT_MONO
            // The native guest's flat picture (windowed play mode, build 71) is
            // shown only while ITS run is on: from Launch until the game returns,
            // and not while it is closing (Quitting… belongs to the launcher).
            // kl_present is not read back for whether the mode resets after
            // glfwTerminate, so this window must not depend on that; a reopened
            // window mid-game shows the game, after the game it shows the
            // launcher. Every other guest keeps the plain rule.
            let native = kl_app_target_is_native() != 0
            let shell = mono && (!native || (session.running && !launcher.quitting))
            if shell != showShell { showShell = shell }

            if kl_app_vrlink_pending() != 0, session.claimHandoff() {
                NSLog("[app] 2D -> VR handoff: \(String(cString: kl_app_vrlink_sargs()))")
                // Unconditionally, not behind Immersive.wanted: that default
                // says what a LAUNCH should open, and this is a run that has
                // just been handed the one thing the VR half cannot start
                // without. The space claim is shared with boot() so the two
                // paths cannot both open it.
                if session.claimSpace() {
                    let r = await openImmersiveSpace(id: Immersive.id)
                    NSLog("[cp] openImmersiveSpace (after handoff) -> \(r)")
                    session.spaceOpened(Self.opened(r))
                }
            }
            // The game closed its window (Quit Game -> glfwTerminate): its picture
            // is gone, so the space goes NOW rather than when main returns (which
            // launch 17 showed can take until the 15 s watchdog). The launcher
            // reads Quitting… until the run ends (kl_app.c's KL_MC_QUIT_GRACE_MS).
            if kl_app_target_is_native() != 0, !launcher.quitting, kl_app_native_closing() != 0 {
                launcher.quitting = true
                NSLog("[launcher] the game closed its window — dismissing the space, waiting for the run to end")
                fputs("[launcher] the game closed its window — dismissing the space\n", stderr)
                if session.releaseSpace() {
                    await dismissImmersiveSpace()
                }
            }
            if kl_app_target_is_native() != 0, session.claimGameEnd(runs: kl_app_native_runs()) {
                await gameEnded()
            }
            try? await Task.sleep(for: .milliseconds(200))
        }
    }

    /// Whether openImmersiveSpace actually gave us the space. `.userCancelled`,
    /// `.error` and anything a later SDK adds all mean there is none to hold.
    private static func opened(_ r: OpenImmersiveSpaceAction.Result) -> Bool {
        if case .opened = r { return true }
        return false
    }

    /// The native guest (Minecraft) returned from its main: Quit Game. Before
    /// this the immersive space stayed up showing the last picture reprojected
    /// (the "black environment") and the launcher still said Playing. Now the
    /// space goes away and the launcher is back to idle — accounts, play target
    /// and resolution editable — and its Launch runs the game again in this
    /// process (kl_app_native_rerun). If the run's isolate could not be torn
    /// down (kl_app_native_rerun_ok, see native_isolate_teardown) a second run
    /// cannot share the process: the app exits instead, so the next tap on
    /// the icon lands on the launcher rather than on a black space.
    private func gameEnded() async {
        let rc = kl_app_native_rc()
        let rerun = kl_app_native_rerun_ok() != 0
        if session.releaseSpace() {
            await dismissImmersiveSpace()
        }
        if !rerun {
            NSLog("[launcher] game ended (rc=\(rc)) — no re-run in this process; exiting so the next launch starts at the launcher")
            fputs("[launcher] game ended (rc=\(rc)) — no re-run in this process; exiting\n", stderr)
            fflush(nil)
            exit(0)
        }
        NSLog("[launcher] game ended (rc=\(rc)) — back to the launcher")
        fputs("[launcher] game ended (rc=\(rc)) — back to the launcher\n", stderr)
        fflush(nil)
        status = "game ended (rc=\(rc))"
        running = false; finished = false; succeeded = false
        launcher.quitting = false
        launcher.details = false
        launcher.gameEnded()
    }

    /// What the report's heading calls this app (ui69).
    ///
    /// The Minecraft row ships under its own name: Klepton is the visionOS /
    /// OpenXR / native host the precompiled game runs on, not the product, so
    /// the heading is the bundle's CFBundleDisplayName ("Minecraft Compiled")
    /// — or "Minecraft" if the Info.plist has none — and the host is credited
    /// on the "Runtime: Klepton" line beside it rather than hidden. Every
    /// other target IS the Klepton app and keeps the Klepton heading.
    /// Display only: nothing here is read by boot, the gate or the launcher.
    private var reportTitle: String {
        guard MSAAccounts.isMinecraft else { return "Klepton" }
        let name = (Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return name.isEmpty ? "Minecraft" : name
    }

    private var bootReport: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                if MCLauncher.enabled {
                    Button("Launcher", systemImage: "chevron.backward") { launcher.details = false }
                }
                Text(reportTitle).font(.largeTitle.bold())
                // The guest, by name. Two apps are built from this tree and they
                // look identical from the front; a boot log that does not say
                // which one produced it is a log that can be read as the other's.
                Text(String(cString: kl_app_target_name()))
                    .foregroundStyle(.secondary)
                // The host, by name, where the heading no longer says it (see
                // reportTitle): the upstream runtime stays credited in Details.
                if MSAAccounts.isMinecraft {
                    Text("Runtime: Klepton")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                // Build identity (ui68): the report is what shows during the
                // boot (Details, KL_LAUNCHER=0, autoboot), so the build is named
                // here too. Display only (KleptonMCLauncher.swift, MCBuildFooter).
                MCBuildFooter(id: "report", light: false)
                Spacer()
                if finished {
                    Label(succeeded ? "initJni completed" : status,
                          systemImage: succeeded ? "checkmark.circle.fill" : "xmark.octagon.fill")
                        .foregroundStyle(succeeded ? .green : .red)
                }
            }

            ScrollView {
                Text(log.isEmpty ? "Press Boot to load the guest chain." : log)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: .infinity)

            // The controller-alignment sliders, in THIS window because it stays
            // open beside the immersive space — so the guest keeps rendering
            // while they move, which is the entire point of them. Collapsed by
            // default: it is a tuning instrument, not part of booting.
            DisclosureGroup("Controller alignment") { TuningView() }
                .font(.callout)

            // ...and the matting dials, the same way and in the same window.
            // Collapsed by default for the same reason: a key colour is found
            // once for a room and then left alone.
            DisclosureGroup("Matting") { ChromaView() }
                .font(.callout)

            // The Minecraft row's accounts (KleptonMSA.swift): which one this
            // boot signs in as, Add account…, default. Locked once booted.
            // Shown while the ownership gate is shut even if KL_MSA=0, so
            // Add account is how the device gets unlocked.
            if MSAAccounts.wanted || (OwnershipGate.enforced && accounts.gateReady && !accounts.deviceUnlocked) {
                AccountsPanel(locked: running || finished)
            }
            if OwnershipGate.enforced && accounts.gateReady && !accounts.deviceUnlocked {
                Text(OwnershipGate.lockedSentence(for: accounts.accounts))
                    .font(.body)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack(spacing: 16) {
                Button(running ? "Running…" : "Boot") { launch() }
                    .disabled(running || OwnershipGate.blocksLaunch(ready: accounts.gateReady, unlocked: accounts.deviceUnlocked))
                if running { ProgressView() }
                if finished, !log.isEmpty {
                    ShareLink(item: log) { Label("Export log", systemImage: "square.and.arrow.up") }
                }
                Spacer()
                Text(status).font(.footnote).foregroundStyle(.secondary)
            }
        }
        .padding(24)
    }

    private func boot() {
        // Guarded on this side too, not only in kl_app.c. The guest boots
        // once per process — the runtime's JNI tables are process-global, so a
        // second run re-registers every native onto the same table and the
        // numbers stop being comparable to the host's. Disabling the button
        // while running is not enough: the run *finishes*, and the obvious
        // next thing to do with a finished run is press Boot again.
        guard !running, !finished else { return }
        // Before the image is mapped and before the immersive space opens.
        // kl_app.c checks again at native_run, which is the call every path
        // (this one, a re-run, KL_LAUNCHER=0, a script) makes before main.
        guard OwnershipGate.allowsLaunch() else {
            OwnershipGate.recordRefusal()
            return
        }
        // Launch after a Quit Game (the native guest's quit-to-launcher): the
        // process already booted, so this is a re-run, not a boot — the env
        // file and the command line are rebuilt and the space opens again.
        if kl_app_native_runs() > 0 {
            rerun()
            return
        }
        // The play mode this process will run (build 71): the launcher's
        // choice, exported to KL_IMMERSIVE before configure reads it and fixed
        // from here on. Windowed unless the player chose Immersive VR.
        if kl_app_target_is_native() != 0 { MCPlayMode.commit(MCLauncherModel.shared.playMode) }
        let windowedNative = kl_app_target_is_native() != 0 && !Immersive.wanted
        running = true; log = ""

        // Off the main thread: the guest blocks — Unity's Baselib waits on
        // futexes, IL2CPP's GC suspends the world — and a blocked main thread
        // is a watchdog kill on this platform, which would present as a crash
        // with no report rather than as the hang it is.
        Thread.detachNewThread {
            let rc = kl_app_configure(Paths.resources, Paths.container)
            if rc != 0 {
                // To the SYSTEM log as well as the window, because this is the
                // one failure that happens before there is a klepton-boot.log to
                // write into — kl_app_boot opens that file, and configure runs
                // first. A scripted run therefore saw no output at all and read
                // as a launch that never reached our code; the reason was
                // sitting in a SwiftUI label nobody was looking at.
                NSLog("[app] configure failed: \(String(cString: kl_app_status()))")
                DispatchQueue.main.async {
                    status = String(cString: kl_app_status())
                    log = "configure failed: \(status)\n\n" + stagingHelp
                    running = false; finished = true; succeeded = false
                }
                return
            }

            let logPath = String(cString: kl_app_log_path())
            // The report is read back from the file rather than returned,
            // because an unimplemented JNI call aborts the process by design.
            // If this run dies, the next launch still shows how far it got.
            let poll = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
                if let t = try? String(contentsOfFile: logPath, encoding: .utf8) {
                    DispatchQueue.main.async { log = t }
                }
            }
            RunLoop.current.add(poll, forMode: .common)

            var result = kl_app_boot()

            // The audio session, and the placement is deliberate on both sides.
            //
            // AFTER kl_app_boot, because boot is what opens (and truncates)
            // Documents/klepton-boot.log — anything this prints earlier goes to
            // a stderr nobody can retrieve from a headset. The first device run
            // did exactly that: the session was configured correctly and the two
            // lines saying so were invisible, which is the wrong way round for
            // the one subsystem whose failure mode is silence.
            //
            // BEFORE the lifecycle, because that is when FMOD opens its OpenSL
            // player, and kl_audio builds its output unit against the session's
            // measured sample rate. A unit initialised against a guess starts
            // successfully and produces nothing, with no error code anywhere in
            // the path. There is a wide margin here: boot ends at initJni and
            // the first [sl] line is thousands of log lines later.
            KleptonAudio.start()
            // Carry on into the Android lifecycle when asked, in the same
            // process and on the same thread. Only after boot has reported, so a
            // lifecycle failure cannot be mistaken for a boot failure — and only
            // when asked, because the gate is a boot that stops at initJni and
            // it must stay possible to take exactly that measurement.
            //
            // This is where libil2cpp (66 MB, 3,083 x18 veneers) first loads
            // under AMFI, and where the synthetic /proc is first read on device.
            //
            // Skipped under KL_IMMERSIVE: there the compositor is the frame
            // clock and drives kl_app_lifecycle_begin/_frame itself, and both
            // entries are once-per-process, so running the pump here as well
            // would take the lifecycle's only turn.
            //
            // Which knob asks for it depends on the guest, because a frame
            // budget is meaningless to one that owns its own frame loop. Beat
            // Saber's is KL_FRAMES — nativeRender calls, counted. Steam Link's
            // is KL_SLINK_WAIT — seconds of looper pump, exactly as it is on the
            // command line. Gating both on KL_FRAMES was the first version and
            // it made the window path on that target load the chain, report, and
            // then never call ANativeActivity_onCreate at all: a run that looks
            // finished and never started the activity.
            // ...and whether it is asked for AT ALL differs too. Beat Saber's
            // window path stops at initJni unless KL_FRAMES says otherwise,
            // because continuing is what the gate must not do. Steam Link's has
            // no such reason: its chain report is printed and complete BEFORE
            // the activity starts, so that measurement stays takeable from any
            // run, and stopping there instead would mean the default launch
            // loads the guest and then does nothing at all. So it always runs,
            // bounded by KL_SLINK_WAIT (30 s by default) — bounded rather than
            // open-ended because the media/audio/XR/GL report is written at the
            // END of the pump, and that is the report a working run has no other
            // way of producing.
            //
            // The Unreal guest is the Steam Link arm's shape for the Steam Link
            // arm's reasons: its budget is KL_UE4_WAIT in seconds (a frame count
            // means nothing to a guest that owns its frame loop), and its report
            // is written at the END of the pump, so stopping at the chain would
            // mean the default launch loads the guest and then does nothing.
            //
            // The native guest in WINDOWED play mode (build 71) is the one more
            // case: kl_app_lifecycle is its whole run (kl_app.c: native_run on
            // this thread, blocking until Quit Game), its picture arrives through
            // kl_mono into ShellWindow, and there is no immersive space. It is
            // taken explicitly rather than through owns_frame_loop/KL_FRAMES, so
            // the windowed game runs whatever the driver table says.
            let env = ProcessInfo.processInfo.environment
            let ownsLoop = kl_app_target_owns_frame_loop() != 0
            if result == 0, !Immersive.wanted,
               windowedNative || ownsLoop || env["KL_FRAMES"] != nil {
                result = kl_app_lifecycle(UInt32(env["KL_FRAMES"] ?? "") ?? 1)
            }
            poll.invalidate()

            let text = (try? String(contentsOfFile: logPath, encoding: .utf8)) ?? ""
            DispatchQueue.main.async {
                log = text
                if windowedNative, result == 0 {
                    // The windowed game has ENDED (native_run returned): idle, not
                    // Playing. gameEnded() answers the run too (claimGameEnd), in
                    // either order; both leave the launcher idle with the same
                    // words, so a late arrival cannot re-lock it.
                    status = "game ended (rc=\(kl_app_native_rc()))"
                    running = false; finished = false; succeeded = false
                    return
                }
                status = String(cString: kl_app_status())
                succeeded = (result == 0)
                running = false; finished = true
                // The space opens only after boot has succeeded, and from here
                // rather than from .task, because the compositor's very first
                // act is kl_app_lifecycle_begin() — which needs the UnityPlayer
                // that kl_app_boot creates. Opening it in parallel with boot
                // would be a race whose losing side reports "kl_app_boot must
                // run first" and reads like a missing binding.
                if result == 0, Immersive.wanted, session.claimSpace() {
                    // The result is logged rather than discarded: `.error` and
                    // `.userCancelled` both leave the app alive with no
                    // compositor, which is indistinguishable in the log from a
                    // compositor that came up and died — and those want
                    // completely different next moves. Either one also gives
                    // the claim back, so a closed space is never held as open.
                    Task { @MainActor in
                        let r = await openImmersiveSpace(id: Immersive.id)
                        NSLog("[cp] openImmersiveSpace -> \(r)")
                        session.spaceOpened(Self.opened(r))
                    }
                }
            }
        }
    }

    private func rerun() {
        running = true
        // The process's committed mode (MCPlayMode.applied) — a re-run cannot
        // change it: KLGLFW_VR and the mono capture were fixed by the first
        // configure/boot (kl_app.c). prepareForLaunch re-exported the same
        // value, so kl_app_native_rerun's env refresh and vrEnabled edit agree.
        let windowedNative = kl_app_target_is_native() != 0 && !Immersive.wanted
        Thread.detachNewThread {
            // Off the main thread: it reads files and asks for the MSA session
            // (a network round trip in kl_msa_session).
            let rc = kl_app_native_rerun()
            DispatchQueue.main.async {
                status = String(cString: kl_app_status())
                if rc != 0 {
                    // Not finished=true: that would lock the launcher with no way
                    // to try again (e.g. Launch pressed before the old layer's
                    // render loop had ended). Idle, with the reason in the footer.
                    NSLog("[launcher] re-run refused: \(status)")
                    running = false; finished = false; succeeded = false
                    return
                }
                // The render loop's end silenced the session; this run needs it.
                KleptonAudio.rearm()
                if windowedNative {
                    // Windowed re-run (build 71): no space to open; the game runs
                    // again on a thread of its own, exactly as the first run did
                    // on the boot thread (kl_app_lifecycle -> native_run has no
                    // once-guard for the native guest). `running` stays true until
                    // it returns; the window shows the picture meanwhile.
                    Thread.detachNewThread {
                        let r = kl_app_lifecycle(1)
                        DispatchQueue.main.async {
                            if r == 0 {
                                status = "game ended (rc=\(kl_app_native_rc()))"
                                running = false; finished = false; succeeded = false
                            } else {
                                status = String(cString: kl_app_status())
                                running = false; finished = true; succeeded = false
                            }
                        }
                    }
                    return
                }
                running = false; finished = true; succeeded = true
                if Immersive.wanted, session.claimSpace() {
                    Task { @MainActor in
                        let r = await openImmersiveSpace(id: Immersive.id)
                        NSLog("[cp] openImmersiveSpace (re-run) -> \(r)")
                        session.spaceOpened(Self.opened(r))
                    }
                }
            }
        }
    }

    private var stagingHelp: String {
        """
        The APK assets are staged into the app's Documents container rather \
        than bundled, so they survive a reinstall and are uploaded once. Run:

            KLEPTON_TARGET=\(String(cString: kl_app_target_name())) \
                visionos/stage_assets.sh <device-udid>

        from the repo root with the device paired and unlocked.
        """
    }
}
