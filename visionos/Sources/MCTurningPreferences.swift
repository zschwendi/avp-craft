import Foundation

// Turning in VR (build 72): Smooth or 45° Snap, a launcher setting applied to the
// SELECTED profile's Vivecraft config before every launch.
//
// What it really does. The staged Vivecraft 26.2 (MCVR.processBindings, javap
// offsets 637-847 / 850-974) reads VRSettings.worldRotationIncrement — JSON key
// "vrWorldRotationIncrement" (VRSettings.java:286), default 45 (287):
//   == 0   the CONTINUOUS branch: each binding step subtracts
//          worldRotationXSensitivity * 10 * stickAxisX from worldRotation (mod 360)
//   != 0   the SNAP branch: a stick click (|axis| > 0.5) turns by ±increment,
//          and the RotateLeft/RotateRight keys add the increment
// Vivecraft's own options screen shows 0 as "Smooth" (VRSettings.java:2184-2185),
// so "0.0" is a first-class value, not an out-of-range trick. The value is a
// string in the file, parsed with Float.parseFloat; "0.0"/"45.0" are the exact
// spellings Float.toString writes.
//
// What it does NOT do. It does not change which stick turns (Vivecraft's own
// binding), does not add dt-aware easing or stick pitch, and is not parity with
// the reference build's camera. It is the supported turning behaviour the
// compiled client already has, exposed as a choice. The HUD pitch key is not
// touched: no renderer in the staged classes consumes it.
//
// Rules. Line-wise edit of the ONE `"vrWorldRotationIncrement": "<current>"` line
// (the shape Gson writes and kl_app.c's vrEnabled edit relies on), verified by
// re-parse: every other key byte for byte, saves/options untouched, written
// atomically. Already equal -> no write. Key missing or JSON unparseable -> no
// write, logged. No config file yet -> no write and NO partial seed: a partial
// file would make MCUIComfort read its missing HUD keys as "customized" and lock
// that profile out of the comfort migration, so the first launch of a brand-new
// profile keeps Vivecraft's default (45) until Vivecraft writes its full config
// on exit; from the second launch the choice applies. Reapplied every launch, so
// an in-game change to this one option is overridden at the next Play.

enum MCTurning: String, CaseIterable, Sendable {
    case smooth, snap45

    static let defaultsKey = "mclauncher.turning"
    static let configKey = "vrWorldRotationIncrement"
    static let configRelative = "config/vivecraft-client-config.json"

    /// Smooth until the player chooses otherwise.
    static var stored: MCTurning {
        MCTurning(rawValue: UserDefaults.standard.string(forKey: defaultsKey) ?? "") ?? .smooth
    }
    static var isRemembered: Bool {
        MCTurning(rawValue: UserDefaults.standard.string(forKey: defaultsKey) ?? "") != nil
    }
    static func store(_ t: MCTurning) { UserDefaults.standard.set(t.rawValue, forKey: defaultsKey) }

    var title: String { self == .smooth ? "Smooth" : "45° Snap" }
    var subtitle: String { self == .smooth ? "hold the turn stick" : "turn in steps" }
    /// Exactly what Vivecraft writes for this float.
    var configValue: String { self == .smooth ? "0.0" : "45.0" }

    // --- pure -------------------------------------------------------------

    enum Decision: Equatable {
        case unchanged(String)                 // already this value
        case rewrite(String, from: String)     // new text, previous value
        case keyMissing
        case malformed(String)
    }

    static func decide(_ text: String, value: String) -> Decision {
        guard let dict = parse(text) else { return .malformed("not a JSON object of settings") }
        guard let current = dict[configKey] as? String else { return .keyMissing }
        if current == value { return .unchanged(current) }
        let from = "\"\(configKey)\": \"\(current)\"", to = "\"\(configKey)\": \"\(value)\""
        let n = text.components(separatedBy: from).count - 1
        guard n == 1 else { return .malformed("expected exactly one `\(from)` line, found \(n)") }
        let out = text.replacingOccurrences(of: from, with: to)
        guard let after = parse(out) else { return .malformed("the rewritten text does not parse") }
        var expect = dict; expect[configKey] = value
        guard NSDictionary(dictionary: after).isEqual(to: expect) else { return .malformed("the rewrite changed another key") }
        return .rewrite(out, from: current)
    }

    private static func parse(_ text: String) -> [String: Any]? {
        guard let d = text.data(using: .utf8), let o = try? JSONSerialization.jsonObject(with: d) else { return nil }
        return o as? [String: Any]
    }

    // --- the profile ------------------------------------------------------

    enum Outcome: Equatable {
        case applied(from: String, to: String)
        case unchanged(String)
        case noConfig          // first launch of a new profile: Vivecraft default until it writes its config
        case keyMissing
        case skipped(String)   // unreadable / malformed; nothing written
        case writeFailed(String)

        var summary: String {
            switch self {
            case let .applied(f, t): return "applied \(configKey) \(f) -> \(t)"
            case .unchanged(let v): return "already \(configKey)=\(v)"
            case .noConfig: return "no config yet (Vivecraft default this launch)"
            case .keyMissing: return "config has no \(configKey) key; left alone"
            case .skipped(let why): return "skipped: \(why)"
            case .writeFailed(let why): return "FAILED to write: \(why)"
            }
        }
    }

    /// Apply `turning` to `<profileDir>/config/vivecraft-client-config.json`. Never throws.
    static func apply(_ turning: MCTurning, profileDir: String) -> Outcome {
        let fm = FileManager.default
        let cfg = profileDir + "/" + configRelative
        guard fm.fileExists(atPath: cfg) else { return .noConfig }
        guard let data = fm.contents(atPath: cfg), let text = String(data: data, encoding: .utf8) else {
            return .skipped("could not read the config")
        }
        switch decide(text, value: turning.configValue) {
        case .unchanged(let v): return .unchanged(v)
        case .keyMissing: return .keyMissing
        case .malformed(let why): return .skipped(why)
        case let .rewrite(out, from):
            do { try Data(out.utf8).write(to: URL(fileURLWithPath: cfg), options: .atomic) }
            catch { return .writeFailed(error.localizedDescription) }
            return .applied(from: from, to: turning.configValue)
        }
    }

    // --- fixtures ---------------------------------------------------------

    /// Gson shape, the live profile's spelling of the key, plus keys a rewrite
    /// must leave alone (same value "45.0" elsewhere, escapes, HUD keys).
    static let stockFixture = """
    {
      "mrMovingCamOffsetZ": "0.0",
      "vrWorldRotationIncrement": "45.0",
      "keyboardKeys": "`1234567890-=qwertyuiop[]\\\\asdfghjkl;':\\"zxcvbnm,./?<>",
      "QUICKCOMMAND_9": "take this!",
      "worldRotationXSensitivity": "1.0",
      "hudDistance": "1.25",
      "someOtherAngle": "45.0",
      "vrHudLockMode": "WRIST",
      "headHudScale": "1.0",
      "vrEnabled": "true"
    }
    """

    /// nil = every check passed.
    static func selfTest() -> String? {
        // 1. Smooth on a stock (45) config rewrites exactly the one line.
        guard case .rewrite(let out, let from) = decide(stockFixture, value: MCTurning.smooth.configValue), from == "45.0"
        else { return "smooth: not rewritten" }
        let expected = stockFixture.replacingOccurrences(of: "\"vrWorldRotationIncrement\": \"45.0\"",
                                                         with: "\"vrWorldRotationIncrement\": \"0.0\"")
        if out != expected { return "smooth: text differs" }
        guard let d = parse(out) else { return "smooth: output does not parse" }
        if d["someOtherAngle"] as? String != "45.0" { return "smooth: another 45.0 value was touched" }
        if d["worldRotationXSensitivity"] as? String != "1.0" { return "smooth: sensitivity touched" }
        if d["hudDistance"] as? String != "1.25" || d["vrHudLockMode"] as? String != "WRIST" { return "smooth: HUD keys touched" }
        if d["keyboardKeys"] as? String != "`1234567890-=qwertyuiop[]\\asdfghjkl;':\"zxcvbnm,./?<>" { return "smooth: escapes damaged" }
        if d.count != 10 { return "smooth: key count \(d.count)" }
        // 2. Idempotent: the result is unchanged for smooth, and snap goes back.
        if decide(out, value: MCTurning.smooth.configValue) != .unchanged("0.0") { return "idempotent smooth" }
        guard case .rewrite(let back, let f2) = decide(out, value: MCTurning.snap45.configValue), f2 == "0.0", back == stockFixture
        else { return "snap: did not restore the stock text" }
        if decide(stockFixture, value: MCTurning.snap45.configValue) != .unchanged("45.0") { return "idempotent snap" }
        // 3. A custom in-game value (30.0) is still replaced by the launcher choice, nothing else moves.
        let custom = stockFixture.replacingOccurrences(of: "\"vrWorldRotationIncrement\": \"45.0\"", with: "\"vrWorldRotationIncrement\": \"30.0\"")
        guard case .rewrite(let o3, let f3) = decide(custom, value: "0.0"), f3 == "30.0", parse(o3)?["someOtherAngle"] as? String == "45.0"
        else { return "custom: not rewritten cleanly" }
        // 4. Missing key, malformed and duplicate line are refused without a rewrite.
        let missing = stockFixture.replacingOccurrences(of: "  \"vrWorldRotationIncrement\": \"45.0\",\n", with: "")
        if decide(missing, value: "0.0") != .keyMissing { return "missing key: \(decide(missing, value: "0.0"))" }
        if case .malformed = decide("not json", value: "0.0") {} else { return "malformed accepted" }
        let twice = stockFixture.replacingOccurrences(of: "  \"someOtherAngle\": \"45.0\",\n", with: "  \"vrWorldRotationIncrement\": \"45.0\",\n")
        if case .malformed = decide(twice, value: "0.0") {} else { return "duplicate line accepted" }
        return nil
    }

    /// File half in a fresh temp dir: apply/idempotence/no-config/saves untouched.
    static func fileSelfTest(in dir: String) -> String? {
        let fm = FileManager.default
        func profile(_ name: String, _ text: String?) throws -> String {
            let p = dir + "/" + name
            try fm.createDirectory(atPath: p + "/config", withIntermediateDirectories: true)
            try fm.createDirectory(atPath: p + "/saves/World", withIntermediateDirectories: true)
            try Data("level".utf8).write(to: URL(fileURLWithPath: p + "/saves/World/level.dat"))
            try Data("guiScale:0\n".utf8).write(to: URL(fileURLWithPath: p + "/options.txt"))
            if let text { try Data(text.utf8).write(to: URL(fileURLWithPath: p + "/" + configRelative)) }
            return p
        }
        do {
            let a = try profile("stock", stockFixture)
            let cfg = a + "/" + configRelative
            if apply(.smooth, profileDir: a) != .applied(from: "45.0", to: "0.0") { return "apply smooth" }
            if apply(.smooth, profileDir: a) != .unchanged("0.0") { return "second apply not idempotent" }
            guard let now = try? String(contentsOfFile: cfg, encoding: .utf8), parse(now)?[configKey] as? String == "0.0" else { return "file not 0.0" }
            if apply(.snap45, profileDir: a) != .applied(from: "0.0", to: "45.0") { return "apply snap" }
            if (try? String(contentsOfFile: cfg, encoding: .utf8)) != stockFixture { return "snap did not restore the exact stock text" }
            if !fm.fileExists(atPath: a + "/saves/World/level.dat") { return "saves touched" }
            if (try? String(contentsOfFile: a + "/options.txt", encoding: .utf8)) != "guiScale:0\n" { return "options.txt touched" }
            let b = try profile("empty", nil)
            if apply(.smooth, profileDir: b) != .noConfig { return "noConfig" }
            if fm.fileExists(atPath: b + "/" + configRelative) { return "noConfig: a file was seeded" }
            let c = try profile("broken", "{ \"vrWorldRotationIncrement\": ")
            let before = try String(contentsOfFile: c + "/" + configRelative, encoding: .utf8)
            if case .skipped = apply(.smooth, profileDir: c) {} else { return "broken: \(apply(.smooth, profileDir: c))" }
            if (try? String(contentsOfFile: c + "/" + configRelative, encoding: .utf8)) != before { return "broken: file changed" }
        } catch { return "fixture setup failed: \(error)" }
        return nil
    }
}

#if MC_TURNING_TEST
@main
struct MCTurningTestMain {
    static func main() {
        if let err = MCTurning.selfTest() { fputs("FAIL decide: \(err)\n", stderr); exit(1) }
        let dir = NSTemporaryDirectory() + "mc-turning-\(getpid())"
        defer { try? FileManager.default.removeItem(atPath: dir) }
        if let err = MCTurning.fileSelfTest(in: dir) { fputs("FAIL apply: \(err)\n", stderr); exit(1) }
        print("ok")
    }
}
#endif
