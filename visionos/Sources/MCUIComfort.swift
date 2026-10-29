import Foundation

// One-time HUD comfort defaults for a Vivecraft profile (ui70, revision 2).
//
// The user's report, clarified: the HEALTH BAR, HOTBAR and ITEM slots are the
// problem — too close, clipped, and with the stock wrist lock possibly out of
// sight on the wrist. Inventory and in-world menu placement are to stay as
// they are. So this moves ONLY the HUD: from the wrist to a forward head lock
// at 2 m, slightly smaller.
//
// The profile's config/vivecraft-client-config.json (LIVE-EVIDENCE.json, the
// active profile) carries Vivecraft's stock HUD placement, all values as JSON
// strings:
//   "vrHudLockMode": "WRIST"   "hudDistance": "1.25"   "headHudScale": "1.0"
//
// Vivecraft (VivecraftMod 13eafae17a987521cc270e81cd774a4746ad9299 —
// GuiHandler.java, VRSettings.java; upstream-input/PROVENANCE.json):
//   GuiHandler.extractGui 539-568   with no screen open the HUD is drawn at the
//       lock position; HEAD = hmd + direction * hudDistance at scale hudScale
//       (config key headHudScale, VRSettings 591-594); WRIST/HAND attach it to
//       the controller (569-629).
//   VRSettings.loadOption 865-877   booleans parse as value.equals("true"),
//       floats as Float.parseFloat, enums via valueOf; HUDLock = {WRIST, HAND,
//       HEAD} (132-136). Ranges: HUD_DISTANCE 0.25..5.0, HUD_SCALE 0.35..2.5
//       (1575-1576). saveOptions writes every value as a JSON string (1283) and
//       keeps keys it does not know (preservedSettingMap, 1308).
// `forceGuiToHUD` and `menuAlwaysFollowFace` are deliberately NOT touched: they
// move inventory / in-world screens / the menu room, which the user did not
// ask for. They are preserved exactly, customized or not, like every other key.
//
// Rules of this migration:
//   * ONLY when all three HUD keys are present and exactly stock. Any
//     difference means the player (or another tool) chose; then nothing is
//     written except the marker, so the question is never asked again.
//   * Line-wise text replacement of `"key": "stock"` (the one-per-line shape
//     Gson's pretty printer writes, which kl_app.c's native_set_vr_config already
//     relies on), verified by re-parsing: every other key stays byte for byte.
//   * The original is copied once to <config>.minecraft-hud-comfort-v1.orig,
//     the new file is written atomically, and config/.minecraft-hud-comfort-v1
//     records the outcome, so this never runs again for the profile — a player
//     who later changes any of the three keeps the change.
//   * Never throws, never deletes, never touches saves/, options.txt (guiScale
//     stays the player's) or any key outside the three. Failures are an
//     Outcome the caller logs; the launch goes on either way.
//
// Fixtures: `swiftc -D MC_UI_COMFORT_TEST MCUIComfort.swift -o comfort && ./comfort`
// runs decide() on in-memory configs and apply() in a temporary directory, with
// no app code involved.

enum MCUIComfort {
    static let version = "v1"
    static let markerName = ".minecraft-hud-comfort-v1"
    static let backupSuffix = ".minecraft-hud-comfort-v1.orig"
    static let configRelative = "config/vivecraft-client-config.json"

    struct Rule: Equatable {
        let key: String
        let stock: String
        let comfort: String
    }

    /// The three HUD keys, in the order the marker lists them.
    static let rules: [Rule] = [
        Rule(key: "vrHudLockMode", stock: "WRIST", comfort: "HEAD"),
        Rule(key: "hudDistance",   stock: "1.25",  comfort: "2.0"),
        Rule(key: "headHudScale",  stock: "1.0",   comfort: "0.8"),
    ]

    // --- pure -------------------------------------------------------------

    enum Decision: Equatable {
        /// The rewritten text; `changed` names the keys that moved.
        case rewrite(String, changed: [String])
        /// At least one of the three is missing or not stock: the player's choice.
        case customized([String])
        /// Not a JSON object, or a shape the line-wise edit cannot handle safely.
        case malformed(String)
    }

    /// Decide on the config text alone. No file access.
    static func decide(_ text: String) -> Decision {
        guard let dict = parse(text) else { return .malformed("not a JSON object of settings") }
        let off = rules.filter { (dict[$0.key] as? String) != $0.stock }.map(\.key)
        if !off.isEmpty { return .customized(off) }

        var out = text
        for r in rules {
            let from = "\"\(r.key)\": \"\(r.stock)\""
            let to = "\"\(r.key)\": \"\(r.comfort)\""
            let n = out.components(separatedBy: from).count - 1
            guard n == 1 else { return .malformed("\(r.key): expected exactly one `\(from)`, found \(n)") }
            out = out.replacingOccurrences(of: from, with: to)
        }

        // Prove the edit touched nothing else: the result must parse, and must
        // equal the original with only the three values replaced.
        guard let after = parse(out) else { return .malformed("the rewritten text does not parse") }
        var expect = dict
        for r in rules { expect[r.key] = r.comfort }
        guard NSDictionary(dictionary: after).isEqual(to: expect) else {
            return .malformed("the rewrite changed a key outside the three")
        }
        return .rewrite(out, changed: rules.map(\.key))
    }

    private static func parse(_ text: String) -> [String: Any]? {
        guard let d = text.data(using: .utf8),
              let o = try? JSONSerialization.jsonObject(with: d) else { return nil }
        return o as? [String: Any]
    }

    // --- the profile ------------------------------------------------------

    enum Outcome: Equatable {
        case applied([String])
        /// The marker exists; its first line.
        case alreadyDone(String)
        /// Not stock: the marker was written, the config was not.
        case customized([String])
        /// No config yet (Vivecraft writes it on the first run); nothing recorded,
        /// the next launch looks again.
        case noConfig
        /// Could not read or safely edit; nothing written, the next launch looks again.
        case unreadable(String)
        /// Backup or atomic write failed; the config is as it was.
        case writeFailed(String)

        /// One log line.
        var summary: String {
            switch self {
            case .applied(let k): return "applied (\(k.joined(separator: ",")))"
            case .alreadyDone(let m): return "already decided: \(m)"
            case .customized(let k): return "left alone, customized (\(k.joined(separator: ",")))"
            case .noConfig: return "no config yet"
            case .unreadable(let why): return "skipped: \(why)"
            case .writeFailed(let why): return "FAILED to write: \(why)"
            }
        }
    }

    /// Apply to `<profileDir>/config/vivecraft-client-config.json`. Never throws.
    static func apply(profileDir: String, now: Date = Date()) -> Outcome {
        let fm = FileManager.default
        let cfg = profileDir + "/" + configRelative
        let marker = (cfg as NSString).deletingLastPathComponent + "/" + markerName
        if let m = try? String(contentsOfFile: marker, encoding: .utf8) {
            return .alreadyDone(String(m.split(whereSeparator: \.isNewline).first ?? ""))
        }
        guard fm.fileExists(atPath: cfg) else { return .noConfig }
        guard let data = fm.contents(atPath: cfg), let text = String(data: data, encoding: .utf8) else {
            return .unreadable("could not read the config")
        }
        let stamp = ISO8601DateFormatter().string(from: now)
        switch decide(text) {
        case .malformed(let why):
            return .unreadable(why)
        case .customized(let keys):
            writeMarker(marker, "\(version) skipped \(stamp): customized (\(keys.joined(separator: ",")))")
            return .customized(keys)
        case .rewrite(let out, let changed):
            let backup = cfg + backupSuffix
            do {
                if !fm.fileExists(atPath: backup) {
                    try data.write(to: URL(fileURLWithPath: backup), options: .atomic)
                }
                try Data(out.utf8).write(to: URL(fileURLWithPath: cfg), options: .atomic)
            } catch {
                return .writeFailed(error.localizedDescription)
            }
            writeMarker(marker, "\(version) applied \(stamp): \(changed.joined(separator: ","))\n"
                        + "original: \((backup as NSString).lastPathComponent)\n"
                        + rules.map { "\($0.key): \($0.stock) -> \($0.comfort)" }.joined(separator: "\n") + "\n")
            return .applied(changed)
        }
    }

    private static func writeMarker(_ path: String, _ text: String) {
        try? Data((text.hasSuffix("\n") ? text : text + "\n").utf8)
            .write(to: URL(fileURLWithPath: path), options: .atomic)
    }

    // --- fixtures ---------------------------------------------------------

    /// A config in the shape Gson writes (pretty, one key per line, strings),
    /// with the three HUD keys at stock, the two menu keys at stock (they must
    /// stay), and a few of the other keys the live profile has, including the
    /// escaped keyboard row.
    static let stockFixture = """
    {
      "mrMovingCamOffsetZ": "0.0",
      "forceGuiToHUD": "false",
      "keyboardKeys": "`1234567890-=qwertyuiop[]\\\\asdfghjkl;':\\"zxcvbnm,./?<>",
      "QUICKCOMMAND_9": "take this!",
      "hudDistance": "1.25",
      "hudPitchOffset": "-2.0",
      "guiScale": "0",
      "headHudScale": "1.0",
      "menuAlwaysFollowFace": "false",
      "hudMaxScale": "false",
      "vrHudLockMode": "WRIST",
      "vrHudWristOffset": "1.0",
      "someFutureKey": "WRIST",
      "vrEnabled": "true"
    }
    """

    /// nil = every check passed.
    static func selfTest() -> String? {
        // 1. Stock changes exactly the three HUD keys, everything else byte for byte.
        guard case .rewrite(let out, let changed) = decide(stockFixture) else { return "stock: not rewritten" }
        if changed != rules.map(\.key) { return "stock: changed \(changed)" }
        let expected = stockFixture
            .replacingOccurrences(of: "\"hudDistance\": \"1.25\"", with: "\"hudDistance\": \"2.0\"")
            .replacingOccurrences(of: "\"headHudScale\": \"1.0\"", with: "\"headHudScale\": \"0.8\"")
            .replacingOccurrences(of: "\"vrHudLockMode\": \"WRIST\"", with: "\"vrHudLockMode\": \"HEAD\"")
        if out != expected { return "stock: output differs from the expected text" }
        guard let d = parse(out) else { return "stock: output does not parse" }
        if d["forceGuiToHUD"] as? String != "false" { return "stock: forceGuiToHUD touched" }
        if d["menuAlwaysFollowFace"] as? String != "false" { return "stock: menuAlwaysFollowFace touched" }
        if d["someFutureKey"] as? String != "WRIST" { return "stock: unknown key with a stock-looking value was touched" }
        if d["vrHudWristOffset"] as? String != "1.0" { return "stock: a key sharing a value with the three was touched" }
        if d["keyboardKeys"] as? String != "`1234567890-=qwertyuiop[]\\asdfghjkl;':\"zxcvbnm,./?<>" { return "stock: escapes damaged" }
        if d["guiScale"] as? String != "0" { return "stock: guiScale touched" }
        if d.count != 14 { return "stock: key count \(d.count)" }

        // 2. The menu keys are preserved EXACTLY even when customized, and do not
        //    block the HUD migration.
        let menusCustom = stockFixture
            .replacingOccurrences(of: "\"forceGuiToHUD\": \"false\"", with: "\"forceGuiToHUD\": \"true\"")
            .replacingOccurrences(of: "\"menuAlwaysFollowFace\": \"false\"", with: "\"menuAlwaysFollowFace\": \"true\"")
        guard case .rewrite(let out2, _) = decide(menusCustom), let d2 = parse(out2) else { return "menus custom: not rewritten" }
        if d2["forceGuiToHUD"] as? String != "true" || d2["menuAlwaysFollowFace"] as? String != "true" {
            return "menus custom: a menu key was changed"
        }
        if d2["vrHudLockMode"] as? String != "HEAD" { return "menus custom: HUD not migrated" }

        // 3. A customized HUD tuple is left alone, and says which keys differ.
        let custom = stockFixture.replacingOccurrences(of: "\"hudDistance\": \"1.25\"", with: "\"hudDistance\": \"1.5\"")
        if decide(custom) != .customized(["hudDistance"]) { return "customized: \(decide(custom))" }
        let partial = stockFixture.replacingOccurrences(of: "  \"vrHudLockMode\": \"WRIST\",\n", with: "")
        if decide(partial) != .customized(["vrHudLockMode"]) { return "missing key: \(decide(partial))" }

        // 4. Idempotent: the result is not stock any more, so a second pass is a no-op.
        if decide(out) != .customized(rules.map(\.key)) { return "idempotency: \(decide(out))" }

        // 5. Malformed input is refused without a rewrite.
        if case .malformed = decide("not json") {} else { return "malformed: accepted" }
        if case .malformed = decide("[\"a\"]") {} else { return "malformed array: accepted" }
        let twice = stockFixture.replacingOccurrences(of: "  \"someFutureKey\": \"WRIST\",\n",
                                                      with: "  \"hudDistance\": \"1.25\",\n")
        if case .malformed = decide(twice) {} else { return "duplicate line: accepted (\(decide(twice)))" }
        return nil
    }

    /// The file half, in `dir` (a fresh temporary directory): apply, backup,
    /// marker, second apply is a no-op, customized profile untouched.
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
            let first = apply(profileDir: a)
            guard case .applied(let k) = first, k == rules.map(\.key) else { return "apply: \(first)" }
            let cfg = a + "/" + configRelative
            guard let now = try? String(contentsOfFile: cfg, encoding: .utf8),
                  case .customized = decide(now) else { return "apply: config not rewritten" }
            guard let orig = try? String(contentsOfFile: cfg + backupSuffix, encoding: .utf8), orig == stockFixture
            else { return "apply: backup missing or differs" }
            guard let m = try? String(contentsOfFile: a + "/config/" + markerName, encoding: .utf8),
                  m.hasPrefix("\(version) applied ") else { return "apply: marker missing" }
            if apply(profileDir: a) != .alreadyDone(String(m.split(whereSeparator: \.isNewline).first ?? "")) {
                return "second apply is not a no-op"
            }
            if (try? String(contentsOfFile: cfg, encoding: .utf8)) != now { return "second apply changed the file" }
            if !fm.fileExists(atPath: a + "/saves/World/level.dat") { return "saves touched" }
            if (try? String(contentsOfFile: a + "/options.txt", encoding: .utf8)) != "guiScale:0\n" { return "options.txt touched" }

            // The player adjusted after the migration: a later pass keeps it.
            let adjusted = now.replacingOccurrences(of: "\"hudDistance\": \"2.0\"", with: "\"hudDistance\": \"3.0\"")
            try Data(adjusted.utf8).write(to: URL(fileURLWithPath: cfg))
            if case .alreadyDone = apply(profileDir: a) {} else { return "post-adjust apply ran again" }
            if (try? String(contentsOfFile: cfg, encoding: .utf8)) != adjusted { return "post-adjust file changed" }

            let b = try profile("custom", stockFixture.replacingOccurrences(of: "\"vrHudLockMode\": \"WRIST\"",
                                                                             with: "\"vrHudLockMode\": \"HAND\""))
            let bc = b + "/" + configRelative
            let before = try String(contentsOfFile: bc, encoding: .utf8)
            if apply(profileDir: b) != .customized(["vrHudLockMode"]) { return "custom: \(apply(profileDir: b))" }
            if (try? String(contentsOfFile: bc, encoding: .utf8)) != before { return "custom: config changed" }
            if fm.fileExists(atPath: bc + backupSuffix) { return "custom: backup written" }
            guard let bm = try? String(contentsOfFile: b + "/config/" + markerName, encoding: .utf8),
                  bm.contains("customized (vrHudLockMode)") else { return "custom: marker missing" }

            let c = try profile("empty", nil)
            if apply(profileDir: c) != .noConfig { return "noConfig: \(apply(profileDir: c))" }
            if fm.fileExists(atPath: c + "/config/" + markerName) { return "noConfig: marker written" }

            let d = try profile("broken", "{ \"hudDistance\": ")
            if case .unreadable = apply(profileDir: d) {} else { return "broken: \(apply(profileDir: d))" }
            if fm.fileExists(atPath: d + "/config/" + markerName) { return "broken: marker written" }
        } catch {
            return "fixture setup failed: \(error)"
        }
        return nil
    }
}

#if MC_UI_COMFORT_TEST
@main
struct MCUIComfortTestMain {
    static func main() {
        if let err = MCUIComfort.selfTest() {
            fputs("FAIL decide: \(err)\n", stderr)
            exit(1)
        }
        let dir = NSTemporaryDirectory() + "mc-ui-comfort-\(getpid())"
        defer { try? FileManager.default.removeItem(atPath: dir) }
        if let err = MCUIComfort.fileSelfTest(in: dir) {
            fputs("FAIL apply: \(err)\n", stderr)
            exit(1)
        }
        print("ok")
    }
}
#endif
