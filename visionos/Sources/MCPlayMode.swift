import Foundation

// How the player wants Minecraft presented (build 71): in this window, or in the
// immersive space. Windowed by default; immersive only when the player chooses
// it in the launcher AND presses the labelled Play action in this session. The
// choice is remembered as a preselection only — the Minecraft row never
// autoboots (BootView.autobootIfAsked), so a remembered Immersive VR cannot start
// a game by itself, and no KL_IMMERSIVE line in the launch environment or
// klepton-env.txt enters the space without that press.
//
// How the choice reaches the runtime. kl_app.c decides the native guest's mode
// from ONE knob, KL_IMMERSIVE (native_vr()), read at configure (vrEnabled in
// Vivecraft's config, KLGLFW_VR), at argv build (-Dklepton.vr.android), at boot
// (kl_mono capture for the flat picture) and at re-run. kl_app_configure applies
// klepton-env.txt FIRST and with overwrite, so a value in that file beats the
// process environment. apply() therefore writes the choice to BOTH: setenv for
// this process and the env file (the same file the Resolution buttons use, same
// precedence), so every read agrees. Immersive.wanted (KleptonApp.swift) reads
// `applied` for the native guest, so the Swift half cannot disagree either.
//
// Fixed per process. KLGLFW_VR is set in native_configure and the mono capture
// is started in native_boot, both once; a re-run re-applies the env file and
// vrEnabled but not those two. So the mode the FIRST boot used is the mode every
// re-run in this process uses (`applied`); changing it means closing and
// reopening the app, and the launcher says so instead of pretending otherwise.
//
// No in-game switch. kl_app_vrlink_pending — the window -> immersive handoff —
// is Steam Link's shell door only (app_vrlink_handoff is installed for that door
// alone). The native guest has no such path, so this is a pre-launch choice.

enum MCPlayMode: String, CaseIterable, Sendable {
    case windowed, immersive

    static let defaultsKey = "mclauncher.playMode"

    /// The remembered choice; windowed when nothing was ever chosen.
    static var stored: MCPlayMode {
        MCPlayMode(rawValue: UserDefaults.standard.string(forKey: defaultsKey) ?? "") ?? .windowed
    }
    /// Whether a choice was ever made on this device. False = the windowed
    /// default is in force, not a remembered pick; the panel says which.
    static var isRemembered: Bool {
        MCPlayMode(rawValue: UserDefaults.standard.string(forKey: defaultsKey) ?? "") != nil
    }
    static func store(_ mode: MCPlayMode) {
        UserDefaults.standard.set(mode.rawValue, forKey: defaultsKey)
    }
    /// The panel's second sentence: where the preselection came from.
    static var originSentence: String {
        isRemembered ? "Remembered from your last choice; nothing starts until you press Play."
                     : "Default until you choose otherwise."
    }

    var immersive: Bool { self == .immersive }

    var title: String { self == .windowed ? "Windowed" : "Immersive VR" }
    var subtitle: String { self == .windowed ? "in this window" : "the space around you" }
    /// The labelled Play action: the player reads where the game will appear
    /// before pressing it.
    var playButtonTitle: String { self == .windowed ? "Play in a window" : "Play in VR" }
    var caption: String {
        switch self {
        case .windowed:
            return "Minecraft opens in this window. Choose Immersive VR here, before you play, to fill the space around you."
        case .immersive:
            return "Minecraft fills the space around you. Press the Digital Crown to see your room."
        }
    }
    /// What the launcher shows while a game runs.
    var playingSentence: String {
        self == .windowed ? "Minecraft is running in this window." : "Minecraft is running in the space around you."
    }

    /// The mode the first boot of this process committed to; nil until then.
    /// Written on the main thread before the boot thread starts (commit), read
    /// from the boot thread afterwards (Immersive.wanted) — never changed again.
    nonisolated(unsafe) private(set) static var applied: MCPlayMode?

    /// Export `wanted` to the runtime BEFORE kl_app_configure / kl_app_native_rerun.
    /// Once a mode is committed, the committed one is re-exported instead and the
    /// result tells the caller what the process will actually do.
    @MainActor @discardableResult
    static func apply(_ wanted: MCPlayMode) -> MCPlayMode {
        let mode = applied ?? wanted
        if mode != wanted {
            NSLog("[playmode] %@ requested, but this process already runs %@ — close and reopen the app to change it",
                  wanted.rawValue, mode.rawValue)
        }
        let value = mode.immersive ? "1" : "0"
        setenv("KL_IMMERSIVE", value, 1)
        MCEnv.setFileValue("KL_IMMERSIVE", value)
        NSLog("[playmode] %@ -> KL_IMMERSIVE=%@ (environment and klepton-env.txt)", mode.rawValue, value)
        return mode
    }

    /// Called by BootView.boot() right before the first kl_app_configure: the
    /// mode in force becomes the process's mode. Idempotent.
    @MainActor
    static func commit(_ wanted: MCPlayMode) {
        guard applied == nil else { return }
        applied = apply(wanted)
        NSLog("[playmode] committed for this process: %@", applied!.rawValue)
    }
}
