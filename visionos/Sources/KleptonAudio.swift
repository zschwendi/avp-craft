import AVFAudio
import Foundation

/// Whether the game should be audible, from the two things that can say the
/// wearer has left it: the app's scene phase and the immersive layer's state.
///
/// **The layer outranks the phase while it exists.** The phase is the app's,
/// and it moves for things that are not the wearer leaving: closing the
/// launcher window while the immersive space plays takes it to `.inactive` and
/// then `.background` (both seen on device, build 69) and nothing ever brings
/// it back to `.active`, because the only window is gone. Silencing on that was
/// the "closed the launcher, the game went mute" bug — the space kept
/// presenting, the latch in kl_audio.c kept every recovery out, and only an
/// `.active` that could no longer arrive would have cleared it.
///
/// The layer is the system's own statement about the space: `.running` means
/// Compositor Services is taking our frames, i.e. the wearer is in the game.
/// A Digital Crown press or Home takes the space off screen and the layer says
/// so — `.paused` or invalidated (`.ended`) — and that silences, phase or no
/// phase. Coming back is `.paused -> .running`, which restores. So this is not
/// "play in the background": the OS pausing or ending the space still wins.
///
/// With no layer (the launcher on its own, KL_IMMERSIVE=0, a flat guest) the
/// phase decides exactly as it always has.
///
/// `lifetime` is the same argument applied to Lifecycle's exit-on-background,
/// and for the native guest (Minecraft) only. Build 69 on device: the wearer
/// closed the launcher window, the world kept playing, and 12 s later the app's
/// phase reached `.background` and Lifecycle exited the game out from under
/// them ("backgrounded — exiting (KL_EXIT_ON_BACKGROUND)", 00:33:33.876). The
/// aggregate phase reaching `.background` with no window left is not the
/// experience closing while the layer is still running or resumably paused.
/// So, while one is, the forced exit waits. It still happens, on the original
/// terms, as soon as the layer ends or the game itself ends; a kept-alive
/// process with nothing to show is not an outcome this allows. Every other
/// guest keeps the original rule untouched.
enum KleptonScenePolicy {
    enum Phase: String { case active, inactive, background }
    enum Layer: String { case none, running, paused, ended }

    static func silent(phase: Phase, layer: Layer) -> Bool {
        switch layer {
        case .running:        return false
        case .paused, .ended: return true
        case .none:           return phase != .active
        }
    }

    /// Whether a layer report counts. `.running` is a compositor taking the
    /// space and always does; anything else counts only from the compositor
    /// that holds it now — a re-run's old layer ending late must neither
    /// silence nor end the new one.
    static func accepts(_ next: Layer, from owner: ObjectIdentifier,
                        current: ObjectIdentifier?) -> Bool {
        next == .running || owner == current
    }

    enum Lifetime: String {
        case carryOn            // not backgrounded, or KL_EXIT_ON_BACKGROUND=0
        case keepForImmersive   // backgrounded, but the space is still ours
        case exit               // the original exit-on-background
    }

    /// `gameOver`: the native game's run has ended (kl_app_native_runs moved)
    /// since the keep-alive began. With the launcher window gone nothing else
    /// would notice — BootView's poll dies with its window.
    static func lifetime(phase: Phase, layer: Layer, nativeGuest: Bool,
                         exitOnBackground: Bool, gameOver: Bool) -> Lifetime {
        guard phase == .background, exitOnBackground else { return .carryOn }
        guard nativeGuest, !gameOver else { return .exit }
        switch layer {
        case .running, .paused: return .keepForImmersive
        case .none, .ended:     return .exit
        }
    }
}

/// Native Minecraft has its own OpenAL output, independent of kl_audio's PCM
/// ring. A window handoff may deactivate the session without changing our
/// desired-audibility latch, so recovery is an event as well as a state change.
enum MCAudioRecoveryPolicy {
    enum Event: String { case sceneHandoff, returned, route, interruptionEnded, mediaReset }
    static func allowed(phase: KleptonScenePolicy.Phase, layer: KleptonScenePolicy.Layer,
                        silenced: Bool, interrupted: Bool, event: Event,
                        handoffEligible: Bool = false) -> Bool {
        guard !silenced, !KleptonScenePolicy.silent(phase: phase, layer: layer) else { return false }
        // An explicit scene/layer return can reacquire a session the OS took
        // during the handoff. A route notification alone cannot undo a genuine
        // interruption, Crown pause, or Home transition.
        return !interrupted || event == .returned || (event == .sceneHandoff && handoffEligible)
    }
    static func resumptionEvent(phase: KleptonScenePolicy.Phase,
                                layer: KleptonScenePolicy.Layer,
                                handoffEligible: Bool) -> Event {
        // A system shouldResume notification is still an automatic handoff
        // when no scene transition intervened; it must share that attempt cap.
        handoffEligible && phase != .active && layer == .running ? .sceneHandoff : .interruptionEnded
    }
}

/// Category options per target, pure so CPU fixtures can pin them.
///
/// The native (Minecraft) target adds `.mixWithOthers` — a candidate fix for the
/// physically observed failure, not a tuning choice. Build71 on device logged a
/// SYSTEM deactivation with an interruption context and reason Default ("another
/// session was activated") while the immersive world kept rendering. Its
/// non-mixable session was taken by another app's activation, and since that
/// app kept its audio, no shouldResume ever came. A mixable session is not the
/// one taken for ordinary playback elsewhere, and it also stops the game from
/// silencing the user's own music at launch. Calls/Siri still interrupt, and
/// KleptonAudio still yields to them (see MCAudioRecoveryPolicy).
///
/// `.allowBluetoothA2DP` stays whenever the microphone is declared:
/// `.playAndRecord` is kept for voice chat, and the category alone records
/// nothing. Non-native targets get exactly their previous options: `[]` for
/// `.playback`, `[.allowBluetoothA2DP]` for `.playAndRecord`.
enum MCAudioCategoryPolicy {
    static func options(nativeGuest: Bool, microphoneDeclared: Bool) -> AVAudioSession.CategoryOptions {
        var options: AVAudioSession.CategoryOptions = microphoneDeclared ? [.allowBluetoothA2DP] : []
        if nativeGuest { options.insert(.mixWithOthers) }
        return options
    }
}

/// Pure scheduling state used by the real session controller and CPU fixtures.
/// A successful device operation cancels its chain. Automatic scene handoffs
/// get at most two activation attempts until a real scene/layer transition.
struct MCAudioRecoverySchedule {
    enum Admission { case allowed, capped(log: Bool) }
    enum Outcome { case recovered, retry, blocked }
    private(set) var generation: UInt64 = 0
    private(set) var handoffAttempts = 0
    private(set) var capped = false
    private var capLogged = false
    private var phase: KleptonScenePolicy.Phase = .active
    private var layer: KleptonScenePolicy.Layer = .none

    mutating func observe(phase nextPhase: KleptonScenePolicy.Phase,
                          layer nextLayer: KleptonScenePolicy.Layer) {
        guard phase != nextPhase || layer != nextLayer else { return }
        phase = nextPhase; layer = nextLayer
        cancel()
        handoffAttempts = 0; capped = false; capLogged = false
    }
    mutating func cancel() { generation &+= 1 }
    mutating func begin() -> UInt64 { cancel(); return generation }
    func isCurrent(_ token: UInt64) -> Bool { generation == token }
    mutating func admit(_ event: MCAudioRecoveryPolicy.Event) -> Admission {
        if (capped && event != .returned) || (event == .sceneHandoff && handoffAttempts >= 2) {
            capped = true
            cancel()
            let log = !capLogged; capLogged = true
            return .capped(log: log)
        }
        if event == .sceneHandoff { handoffAttempts += 1 }
        return .allowed
    }
    static func outcome(nativeGuest: Bool, helperResult: Int32) -> Outcome {
        if !nativeGuest || helperResult == MC_AUDIO_RECOVERED || helperResult == MC_AUDIO_REOPENED {
            return .recovered
        }
        return .retry
    }
    mutating func finish(_ outcome: Outcome, token: UInt64) {
        guard isCurrent(token) else { return } // an older attempt cannot cancel a newer event
        if outcome != .retry { cancel() }
    }
}

/// The audio session — the one part of playback that CoreAudio alone will not
/// give you on this platform.
///
/// `runtime/media/kl_audio.c` owns the output unit and the ring, and it is plain C
/// that compiles unchanged for macOS and visionOS. What does *not* port is the
/// session: on macOS there is none, and on visionOS an output unit will
/// initialise, start, report success and produce nothing at all unless an
/// `AVAudioSession` has been configured and activated first. That failure has
/// no error code anywhere in it, which is exactly why this file exists and why
/// it logs what it did.
///
/// The second job here is keeping the stream. visionOS inherits iPadOS's view
/// that audio is a resource the system lends out: an interruption, a route
/// change, a media-services reset or a trip through the background can all
/// take the unit away, and the unit does not always notice — it stays nominally
/// started and simply stops calling back. So there are two independent
/// recoveries, and they are meant to overlap:
///
///   * these notifications, which are precise and occasionally do not arrive;
///   * kl_audio.c's own watchdog on the producer side, which notices a render
///     callback that has stopped arriving and rebuilds the unit regardless.
///
/// Either alone has been enough to lose audio for a whole session on this
/// family of OS. Together, a missed notification costs a half-second gap.
enum KleptonAudio {
    nonisolated(unsafe) private static var started = false
    nonisolated(unsafe) private static var observers: [NSObjectProtocol] = []
    // Set by setSilenced: while true, the route-change / interruption recoveries
    // must not restart the unit — exiting immersion fires exactly those
    // notifications, and without the gate they would "recover" the silence away
    // a moment after the crown press asked for it.
    nonisolated(unsafe) private static var silenced = false
    nonisolated(unsafe) private static var interrupted = false
    nonisolated(unsafe) private static var interruptionAllowsHandoff = false
    nonisolated(unsafe) private static var recovery = MCAudioRecoverySchedule()
    nonisolated(unsafe) private static var needsOpenALReopen = false
    nonisolated(unsafe) private static var lastRecovery: TimeInterval = -.infinity

    // KleptonScenePolicy's inputs, read by Lifecycle too. Main thread only:
    // Lifecycle is on it already and layerChanged hops to it, so there is one
    // writer and no lock.
    nonisolated(unsafe) private(set) static var phase: KleptonScenePolicy.Phase = .active
    nonisolated(unsafe) private(set) static var layer: KleptonScenePolicy.Layer = .none
    // Which compositor the layer state belongs to. A re-run opens a new layer;
    // the old one's late `.ended` must not silence the new one's `.running`.
    nonisolated(unsafe) private static var layerOwner: ObjectIdentifier?

    /// The app's scene phase (Lifecycle, main thread).
    static func scenePhaseChanged(to next: KleptonScenePolicy.Phase) {
        phase = next
        recovery.observe(phase: phase, layer: layer)
        settle(resume: next == .active)
        // Closing the last window can auto-deactivate AVAudioSession even
        // while the immersive layer stays running. No false->true silenced
        // transition occurs in that case, and .active may never arrive again.
        if next != .active && layer == .running {
            requestRecovery(.sceneHandoff)
        }
    }

    /// The immersive layer's state, from the compositor's render thread.
    ///
    /// Hops to main rather than acting here: `setActive` is a round trip to
    /// the audio server, and a render loop that blocks on one for long enough
    /// is killed by the system with no crash report (see renderLoop).
    ///
    /// Also Lifecycle's second input. Its phase handler hangs off the
    /// launcher's WindowGroup, and once that window is closed this callback
    /// is the only thing left that sees the space end.
    static func layerChanged(_ owner: ObjectIdentifier, _ next: KleptonScenePolicy.Layer) {
        DispatchQueue.main.async {
            guard KleptonScenePolicy.accepts(next, from: owner, current: layerOwner) else {
                NSLog("[au] layer \(next) from a compositor that no longer owns the space — ignored")
                return
            }
            layerOwner = owner
            // Back from a pause is the moment this OS has silently stopped the
            // render callback (see kl_audio_resume), so it rebuilds.
            let resumed = layer == .paused && next == .running
            layer = next
            recovery.observe(phase: phase, layer: layer)
            settle(resume: resumed)
            Lifecycle.reconsider("layer \(next.rawValue)", grace: next == .ended)
        }
    }

    /// A new run is about to open a new space (quit-to-launcher re-run). The
    /// last layer ended and silenced; until the new one reports, the phase
    /// decides again — which is what the launcher window, `.active`, wants.
    static func rearm() {
        layer = .none
        layerOwner = nil
        recovery.observe(phase: phase, layer: layer)
        settle(resume: false)
    }

    private static func settle(resume: Bool) {
        let want = KleptonScenePolicy.silent(phase: phase, layer: layer)
        NSLog("[au] owner: phase=\(phase.rawValue) layer=\(layer.rawValue) -> "
              + (want ? "silent" : "playing"))
        if want != silenced {
            setSilenced(want)          // an unsilence rebuilds the unit itself
        } else if resume && !want {
            requestRecovery(.returned)
        }
    }

    /// Silence or restore the output unit, without tearing anything down.
    /// Only `settle` decides when — see KleptonScenePolicy. Other apps go
    /// quiet on the crown press; this is that.
    private static func setSilenced(_ next: Bool) {
        guard started, next != silenced else { return }
        silenced = next
        let session = AVAudioSession.sharedInstance()
        if next {
            recovery.cancel() // invalidate delayed returns before deactivation
            controlOpenAL(Int32(MC_AUDIO_PAUSE))
            kl_audio_set_silenced(1)
            do { try session.setActive(false) }
            catch { NSLog("[mc-audio] session deactivate failed"); logFailure("session deactivate", error) }
        } else {
            requestRecovery(.returned)
        }
    }

    /// Bounded retries cover OS deactivation arriving after the phase event.
    /// Every newer event or deliberate silence cancels the older generation.
    /// These operations stay on main, never the compositor or mixer callback.
    private static func requestRecovery(_ event: MCAudioRecoveryPolicy.Event, immediate: Bool = true) {
        guard started else { return }
        // A reset itself may emit a route event. Do not let that notification
        // create a fresh retry chain; the current chain already covers it.
        if event == .route && ProcessInfo.processInfo.systemUptime - lastRecovery < 1.0 { return }
        let generation = recovery.begin()
        if immediate {
            let outcome = recoverNow(event)
            recovery.finish(outcome, token: generation)
            guard outcome == .retry && recovery.isCurrent(generation) else { return }
        }
        for delay in [0.15, 0.80] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                guard recovery.isCurrent(generation) else { return }
                let outcome = recoverNow(event)
                recovery.finish(outcome, token: generation)
            }
        }
    }

    private static func recoverNow(_ event: MCAudioRecoveryPolicy.Event) -> MCAudioRecoverySchedule.Outcome {
        guard MCAudioRecoveryPolicy.allowed(phase: phase, layer: layer,
                    silenced: silenced, interrupted: interrupted, event: event,
                    handoffEligible: interruptionAllowsHandoff) else { return .blocked }
        let now = ProcessInfo.processInfo.systemUptime
        // Our setActive/directStereo can cause a route notification. Coalesce
        // that feedback instead of repeatedly resetting a healthy device.
        guard now - lastRecovery >= 0.10 else { return .retry }
        switch recovery.admit(event) {
        case .allowed: break
        case .capped(let log):
            if log { NSLog("[mc-audio] automatic handoff recovery capped; waiting for scene return") }
            return .blocked
        }
        lastRecovery = now
        let session = AVAudioSession.sharedInstance()
        do {
            if needsOpenALReopen { try Self.setCategory(session) }
            try session.setActive(true)
            try Self.directStereo(session)
        } catch {
            NSLog("[mc-audio] session recovery failed (%@)", event.rawValue)
            logFailure("session recovery (\(event.rawValue))", error)
            return .retry // never start a device against an inactive session
        }
        interrupted = false
        interruptionAllowsHandoff = false
        // The PCM sink remains correct for the other targets. Minecraft's
        // OpenAL must be recovered separately after the session is active.
        kl_audio_session_ready(session.sampleRate)
        kl_audio_interrupted(0)
        kl_audio_set_silenced(0)
        kl_audio_resume()
        let action = needsOpenALReopen ? Int32(MC_AUDIO_REOPEN) : Int32(MC_AUDIO_RESET)
        let result = controlOpenAL(action)
        let restored = result == MC_AUDIO_RECOVERED || result == MC_AUDIO_REOPENED
        if restored { needsOpenALReopen = false }
        NSLog("[mc-audio] recovery event=%@ result=%d", event.rawValue, result)
        // Other guests use the recovered PCM sink and have no OpenAL device.
        return MCAudioRecoverySchedule.outcome(nativeGuest: kl_app_target_is_native() != 0,
                                              helperResult: result)
    }

    @discardableResult private static func controlOpenAL(_ action: Int32) -> Int32 {
        guard kl_app_target_is_native() != 0 else { return Int32(MC_AUDIO_NOT_LOADED) }
        let frameworks = Bundle.main.privateFrameworksPath ?? Bundle.main.bundlePath + "/Frameworks"
        return (frameworks + "/libopenal.dylib").withCString { mc_openal_audio_control($0, action) }
    }

    /// Configure and activate the session, then tell the C side what the
    /// hardware rate turned out to be. Safe to call more than once.
    static func start() {
        // The launcher's boot worker calls this before entering the guest.
        // Serialize session/state ownership with scene and notification events,
        // while retaining the required before-guest activation ordering.
        if !Thread.isMainThread {
            DispatchQueue.main.sync { Self.start() }
            return
        }
        guard !started else { return }
        started = true

        let session = AVAudioSession.sharedInstance()
        do {
            // .playback, not .ambient: Beat Saber's music is the point of the
            // app, not a decoration over someone else's audio, and .ambient is
            // silenced by the ringer switch on this OS family. .default mode
            // keeps the system's own spatialisation out of the way — the guest
            // mixes its own stereo and anything we add on top is a second
            // opinion about a scene we already rendered.
            try Self.setCategory(session)
            // Ask for the guest's rate. If the system grants it the resampler
            // in kl_audio.c degenerates to a copy; if it does not, kl_audio
            // measures what it actually got and resamples. Either way this is a
            // request, never an assumption — a unit built against an assumed
            // rate is the failure that reports success and stays silent.
            try session.setPreferredSampleRate(48000)
            try session.setPreferredIOBufferDuration(0.010)
            try session.setActive(true)
            // After activation, which is the order ALVR uses and the order the
            // session's own routing decisions are made in.
            try Self.directStereo(session)
        } catch {
            NSLog("[au] AVAudioSession setup failed: \(error) — expect silence")
            logFailure("session setup", error)
        }

        NSLog("[au] session active: \(session.sampleRate) Hz, "
              + "\(session.outputNumberOfChannels) ch out, "
              + "IO buffer \(String(format: "%.1f", session.ioBufferDuration * 1000)) ms")
        logSessionState("start")
        kl_audio_session_ready(session.sampleRate)

        if #available(visionOS 27.0, *) {
            observe(AVAudioSession.didBecomeInactiveNotification) { note in
                let context = note.userInfo?[AVAudioSession.deactivationContextKey]
                    as? AVAudioSession.DeactivationContext
                let reason = context?.interruptionContext?.reason
                // The SDK specifies interruptionContext only for interruption
                // by another application. A system deactivation without one,
                // or the explicit scene-background reason, can be the closing
                // window; never override another app's interruption.
                let handoff = reason == .sceneWasBackgrounded
                    || (context?.source == .system && context?.interruptionContext == nil)
                NSLog("[mc-audio] session inactive source=%ld reason=%ld",
                      context?.source.rawValue ?? -1, reason.map { Int($0.rawValue) } ?? -1)
                logSessionState("inactive")
                becameInactive(handoffEligible: handoff)
            }
            observe(AVAudioSession.resumptionRecommendationNotification) { note in
                let context = note.userInfo?[AVAudioSession.resumptionContextKey]
                    as? AVAudioSession.ResumptionContext
                logSessionState("resumption")
                guard context?.recommendation == .shouldResume else {
                    becameInactive(handoffEligible: false)
                    return
                }
                let event = MCAudioRecoveryPolicy.resumptionEvent(phase: phase, layer: layer,
                                                handoffEligible: interruptionAllowsHandoff)
                interrupted = false
                interruptionAllowsHandoff = false
                requestRecovery(event)
            }
        } else {
            observe(AVAudioSession.interruptionNotification) { note in
                let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt ?? 0
                logSessionState(AVAudioSession.InterruptionType(rawValue: raw) == .began
                                ? "interruption-began" : "interruption-ended")
                if AVAudioSession.InterruptionType(rawValue: raw) == .began {
                    let reason = note.userInfo?[AVAudioSessionInterruptionReasonKey] as? UInt
                    becameInactive(handoffEligible: reason == AVAudioSession.InterruptionReason.sceneWasBackgrounded.rawValue)
                } else {
                    let options = note.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0
                    guard AVAudioSession.InterruptionOptions(rawValue: options).contains(.shouldResume) else { return }
                    let event = MCAudioRecoveryPolicy.resumptionEvent(phase: phase, layer: layer,
                                                    handoffEligible: interruptionAllowsHandoff)
                    interrupted = false
                    interruptionAllowsHandoff = false
                    requestRecovery(event)
                }
            }
        }

        // A route change is the headphones going in or out, and on this device
        // also the transition into and out of an immersive space. The hardware
        // sample rate can change with it, which is why the C side rebuilds the
        // unit rather than restarting it: a unit initialised against the old
        // rate starts happily and never calls back.
        observe(AVAudioSession.routeChangeNotification) { note in
            let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt ?? 0
            let reason = AVAudioSession.RouteChangeReason(rawValue: raw)
            NSLog("[au] route change (reason \(raw)), now \(session.sampleRate) Hz")
            // Exiting immersion fires a route change; a crown press silenced us
            // on purpose, and "recovering" here would undo it a moment later.
            guard !silenced else { return }
            switch reason {
            case .oldDeviceUnavailable, .newDeviceAvailable, .override,
                 .routeConfigurationChange, .categoryChange:
                requestRecovery(.route)
            default:
                break
            }
        }

        // Rare, and total: every audio object in the process is invalid. Nothing
        // survives it except a full rebuild, and the session has to be
        // configured again from scratch first.
        observe(AVAudioSession.mediaServicesWereResetNotification) { _ in
            NSLog("[au] media services were reset — reconfiguring from scratch")
            needsOpenALReopen = true
            interrupted = false
            interruptionAllowsHandoff = false
            requestRecovery(.mediaReset) // policy checks before activating anything
        }
    }

    private static func becameInactive(handoffEligible: Bool) {
        interrupted = true
        interruptionAllowsHandoff = handoffEligible
        recovery.cancel()
        controlOpenAL(Int32(MC_AUDIO_PAUSE))
        kl_audio_interrupted(1)
        if handoffEligible && phase != .active && layer == .running {
            // The deactivation may arrive after the phase event. Delay lets
            // an actual Crown/Home pause or invalidation cancel recovery.
            requestRecovery(.sceneHandoff, immediate: false)
        }
    }

    /// The microphone is opt-in per target: only a target whose Info.plist
    /// carries NSMicrophoneUsageDescription (gen_xcodeproj.py adds it for
    /// Vivecraft's voice chat) gets `.playAndRecord`; every other target keeps
    /// `.playback`. `.default` mode, not `.voiceChat`: the voice processing unit
    /// would also process the game's own mix. No `.defaultToSpeaker` — there is
    /// no receiver to avoid on this device. `.allowBluetoothA2DP` keeps AirPods
    /// on the high-quality route. Record permission is asked for here, at boot
    /// in the flat window, rather than by the first capture inside immersion.
    ///
    /// The native (Minecraft) target's session is MIXABLE — see
    /// MCAudioCategoryPolicy. Every other target keeps exactly the options it
    /// had.
    private static func setCategory(_ session: AVAudioSession) throws {
        let microphone = Bundle.main.object(forInfoDictionaryKey: "NSMicrophoneUsageDescription") != nil
        let options = MCAudioCategoryPolicy.options(nativeGuest: kl_app_target_is_native() != 0,
                                                    microphoneDeclared: microphone)
        guard microphone else {
            try session.setCategory(.playback, mode: .default, options: options)
            NSLog("[mc-audio] category playback options=0x%lx", options.rawValue)
            return
        }
        try session.setCategory(.playAndRecord, mode: .default, options: options)
        NSLog("[mc-audio] category playAndRecord options=0x%lx", options.rawValue)
        let perm = AVAudioApplication.shared.recordPermission
        NSLog("[au] category playAndRecord (microphone declared), record permission "
              + (perm == .granted ? "granted" : perm == .denied ? "DENIED" : "undetermined"))
        if perm == .undetermined {
            AVAudioApplication.requestRecordPermission { granted in
                NSLog("[au] record permission \(granted ? "granted" : "DENIED")")
            }
        }
    }

    /// Take the system's spatial audio out of the path: two channels, played
    /// where the guest mixed them.
    ///
    /// **This is why the sound followed the window.** visionOS spatialises app
    /// audio by default, and the sound stage it spatialises *into* is anchored
    /// to the app's scene — so a stereo mix that the guest has already panned
    /// for a head-mounted listener gets panned a second time, towards a window,
    /// by a system that has no idea where anything in the guest's world is. It
    /// is not a mixing bug and no gain change fixes it; the whole scene simply
    /// sits wherever the window is.
    ///
    /// `.bypassed` is the escape hatch, and it is the same one ALVR uses
    /// (`EventHandler.fixAudioForDirectStereo`) for the same reason: the audio
    /// arrives already spatialised by something that knows the scene, so the
    /// only correct thing the OS can do with it is play it. Beat Saber's FMOD
    /// mix is exactly that — `kl_opensl.c` hands us a finished stereo buffer.
    ///
    /// `setPreferredOutputNumberOfChannels(2)` goes with it: bypassing the
    /// spatialiser on a route that has offered more than two channels would
    /// otherwise leave the mix to be spread across them by whatever downmix
    /// happens to be in the way.
    ///
    /// ALVR's other two calls are deliberately NOT imported. It takes
    /// `.playAndRecord` and `.voiceChat` because it needs the microphone for
    /// SteamVR; we do not, and `.playAndRecord` would hand the ringer switch a
    /// veto over the music this app exists to play.
    ///
    /// `KL_AUDIO_SPATIAL=1` leaves the system's spatialiser in, which is the
    /// A/B if the sound is ever wrong in a way that is not "in the wrong
    /// place".
    private static func directStereo(_ session: AVAudioSession) throws {
        guard ProcessInfo.processInfo.environment["KL_AUDIO_SPATIAL"] != "1" else {
            NSLog("[au] KL_AUDIO_SPATIAL=1 — leaving the system spatialiser in the path")
            return
        }
        try session.setPreferredOutputNumberOfChannels(2)
        try session.setIntendedSpatialExperience(.bypassed)
        NSLog("[au] direct stereo: system spatialisation bypassed, 2 ch preferred")
    }

    /// What the next device log needs to tell the interrupter classes apart:
    /// our own options, and the system's two public hints about whether other
    /// audio is playing. Booleans and a bit mask only — no app identity.
    private static func logSessionState(_ label: String) {
        let s = AVAudioSession.sharedInstance()
        NSLog("[mc-audio] session %@ options=0x%lx otherAudio=%d secondaryHint=%d",
              label, s.categoryOptions.rawValue,
              s.isOtherAudioPlaying ? 1 : 0, s.secondaryAudioShouldBeSilencedHint ? 1 : 0)
    }

    /// An activation or category failure, by NSError domain and code only.
    private static func logFailure(_ label: String, _ error: Error) {
        let e = error as NSError
        NSLog("[mc-audio] %@ failed domain=%@ code=%ld", label, e.domain, e.code)
    }

    private static func observe(_ name: Notification.Name,
                                _ body: @escaping (Notification) -> Void) {
        observers.append(NotificationCenter.default.addObserver(
            forName: name, object: AVAudioSession.sharedInstance(),
            queue: .main, using: body))
    }
}
