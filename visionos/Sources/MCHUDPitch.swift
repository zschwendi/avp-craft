import Foundation

// One-time HUD pitch move for a Vivecraft profile: the stored hudPitchOffset
// "-2.0" (the old stock value) becomes "-15.0". Nothing else.
//
// Why the host does anything at all: the Java side (a separate patch, not in
// this file) now consumes hudPitchOffset and ships -15 as its default, next to
// the comfort placement HEAD / 2.0 m / 0.8. A default only reaches a profile
// with no stored value, though — a stored value wins — and every profile that
// ran the old build has the old stock "-2.0" that saveOptions wrote (all values
// as JSON strings, see MCUIComfort.swift). So there is no seed and no init
// here; this rewrites exactly that one stored value, once per profile:
//
//   * no config yet          nothing written, no marker. The game writes its
//                            config on the first run with its own default (-15);
//                            the next launch looks again.
//   * hudPitchOffset absent  marker written, config untouched. The new Java's
//                            default is what such a profile gets, so there is
//                            nothing to move — and recording it means a player
//                            who later picks -2.0 in game keeps it.
//   * exactly the string "-2.0", on a line of its own (Gson's pretty shape)
//                            rewritten to "-15.0": backup, atomic write, marker.
//   * anything else          "-10.0", the number -2, "-2", "-15.0", null...:
//                            the player's (or another tool's) choice. Marker,
//                            config untouched.
//   * duplicate top-level hudPitchOffset keys, not a JSON object, not the
//     one-key-per-line shape, over 1 MiB, not UTF-8, a symlink or not a regular
//     file (the config, or config/ itself): refused. Nothing written, no
//     marker; the next launch looks again.
//
// Independent of MCUIComfort: its own marker (config/.avpcraft-hud-pitch-v1)
// and backup (<config>.avpcraft-hud-pitch-v1.orig), and the comfort marker is
// never read — a profile comfort already moved (HEAD / 2.0 / 0.8, its marker
// present) still gets the pitch. It runs right after MCUIComfort on the same
// file, so it sees comfort's result.
//
// The edit is proved, not assumed: the output is the input with the 4 bytes
// "-2.0" replaced by "-15.0" on exactly one line, it re-parses, its dictionary
// equals the original's with only hudPitchOffset changed, and it still has
// exactly one top-level hudPitchOffset.
//
// Never throws; never deletes anything but its own temporary backup name;
// never touches saves/, options.txt, .account or any other key. No config value
// is logged or written to the marker. The outcome is one line the caller logs.
//
// Fixtures (Foundation only, no app code):
//   swiftc -parse-as-library -D MC_HUD_PITCH_TEST MCHUDPitch.swift -o hudpitch && ./hudpitch

enum MCHUDPitch {
    static let version = "v1"
    static let markerName = ".avpcraft-hud-pitch-v1"
    static let backupSuffix = ".avpcraft-hud-pitch-v1.orig"
    static let configRelative = "config/vivecraft-client-config.json"
    static let key = "hudPitchOffset"
    static let stock = "-2.0"
    static let target = "-15.0"
    /// A real Vivecraft config is a few KB.
    static let maxBytes = 1 << 20

    // --- pure -------------------------------------------------------------

    enum Decision: Equatable {
        /// The rewritten text.
        case rewrite(String)
        /// Present, but not exactly the string "-2.0": the player's choice.
        case customized
        /// No top-level hudPitchOffset: the game's own default applies.
        case absent
        /// Refused; the reason names no config value.
        case malformed(String)
    }

    /// Decide on the config text alone. No file access.
    static func decide(_ text: String) -> Decision {
        let src = Array(text.utf8)
        if src.count > maxBytes { return .malformed("larger than 1 MiB") }
        guard let dict = parse(text) else { return .malformed("not a JSON object of settings") }
        guard let n = topLevelCount(of: key, in: src) else { return .malformed("could not scan the keys") }
        if n > 1 { return .malformed("\(key) appears \(n) times") }
        if n == 0 { return dict[key] == nil ? .absent : .malformed("\(key): parser and scanner disagree") }
        guard (dict[key] as? String) == stock else { return .customized }

        // Line-wise: one occurrence, on a line holding only this key.
        let from = Array("\"\(key)\": \"\(stock)\"".utf8)
        let to = Array("\"\(key)\": \"\(target)\"".utf8)
        let at = occurrences(of: from, in: src)
        guard at.count == 1, let lo = at.first else {
            return .malformed("expected exactly one `\"\(key)\": \"\(stock)\"`, found \(at.count)")
        }
        let nl = UInt8(ascii: "\n")
        let pad: Set<UInt8> = [0x20, 0x09, 0x0D]
        var line = src[(src[..<lo].lastIndex(of: nl).map { $0 + 1 } ?? 0)..<(src[lo...].firstIndex(of: nl) ?? src.count)]
        while let c = line.first, pad.contains(c) { line = line.dropFirst() }
        while let c = line.last, pad.contains(c) { line = line.dropLast() }
        guard line.elementsEqual(from) || line.elementsEqual(from + [UInt8(ascii: ",")]) else {
            return .malformed("\(key) is not on a line of its own")
        }
        let outBytes = Array(src[..<lo]) + to + Array(src[(lo + from.count)...])

        // Prove the edit touched nothing else: exactly one line differs, the
        // result parses, equals the original with only the one value replaced,
        // and still has exactly one top-level key of that name.
        let a = src.split(separator: nl, omittingEmptySubsequences: false)
        let b = outBytes.split(separator: nl, omittingEmptySubsequences: false)
        guard a.count == b.count, zip(a, b).filter({ !$0.0.elementsEqual($0.1) }).count == 1 else {
            return .malformed("the rewrite changed more than one line")
        }
        let out = String(decoding: outBytes, as: UTF8.self)
        guard let after = parse(out) else { return .malformed("the rewritten text does not parse") }
        var expect = dict
        expect[key] = target
        guard NSDictionary(dictionary: after).isEqual(to: expect) else {
            return .malformed("the rewrite changed a key other than \(key)")
        }
        guard topLevelCount(of: key, in: outBytes) == 1 else { return .malformed("the rewrite changed the key count") }
        return .rewrite(out)
    }

    private static func parse(_ text: String) -> [String: Any]? {
        guard let o = try? JSONSerialization.jsonObject(with: Data(text.utf8)) else { return nil }
        return o as? [String: Any]
    }

    /// How many keys of the top-level object are `name` (escapes decoded).
    /// JSONSerialization keeps one of a duplicated key silently; this counts
    /// them all. nil = an unterminated string or an undecodable key.
    static func topLevelCount(of name: String, in b: [UInt8]) -> Int? {
        let quote = UInt8(ascii: "\""), backslash = UInt8(ascii: "\\"), colon = UInt8(ascii: ":")
        let space: [UInt8] = [0x20, 0x09, 0x0A, 0x0D]
        var i = 0, depth = 0, n = 0
        while i < b.count {
            let c = b[i]
            if c == quote {
                let start = i
                var escaped = false
                i += 1
                while i < b.count, b[i] != quote {
                    if b[i] == backslash { escaped = true; i += 1 }
                    i += 1
                }
                guard i < b.count else { return nil }
                let end = i
                i += 1
                // A key is a string in the top-level object followed by `:`.
                guard depth == 1 else { continue }
                var j = i
                while j < b.count, space.contains(b[j]) { j += 1 }
                guard j < b.count, b[j] == colon else { continue }
                let k: String
                if escaped {
                    guard let s = (try? JSONSerialization.jsonObject(with: Data(b[start...end]),
                                                                     options: .fragmentsAllowed)) as? String
                    else { return nil }
                    k = s
                } else {
                    k = String(decoding: b[(start + 1)..<end], as: UTF8.self)
                }
                if k == name { n += 1 }
                continue
            }
            switch c {
            case UInt8(ascii: "{"), UInt8(ascii: "["): depth += 1
            case UInt8(ascii: "}"), UInt8(ascii: "]"): depth -= 1
            default: break
            }
            i += 1
        }
        return n
    }

    private static func occurrences(of needle: [UInt8], in hay: [UInt8]) -> [Int] {
        guard !needle.isEmpty, hay.count >= needle.count else { return [] }
        var r: [Int] = []
        var i = 0
        while i <= hay.count - needle.count {
            if hay[i] == needle[0], hay[i..<(i + needle.count)].elementsEqual(needle) {
                r.append(i)
                i += needle.count
            } else {
                i += 1
            }
        }
        return r
    }

    // --- the profile ------------------------------------------------------

    enum Outcome: Equatable {
        /// "-2.0" -> "-15.0". `keptEarlierBackup`: a backup from an earlier,
        /// interrupted attempt was already there and was left as it was.
        case applied(keptEarlierBackup: Bool)
        /// The marker exists; its first line.
        case alreadyDone(String)
        /// Not the stock string: the marker was written, the config was not.
        case customized
        /// No hudPitchOffset key: the marker was written, the config was not.
        case absent
        /// No config yet; nothing recorded, the next launch looks again.
        case noConfig
        /// Could not read or safely edit; nothing written, the next launch looks again.
        case unreadable(String)
        /// Backup or atomic write failed; the config is as it was.
        case writeFailed(String)

        /// One log line. Names no config value.
        var summary: String {
            let key = MCHUDPitch.key
            switch self {
            case .applied(let kept):
                return "applied (\(key) \(MCHUDPitch.stock) -> \(MCHUDPitch.target))" + (kept ? "; earlier backup kept" : "")
            case .alreadyDone(let m): return "already decided: \(m)"
            case .customized: return "left alone, \(key) customized"
            case .absent: return "left alone, \(key) absent (the game's default applies)"
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
        let dir = (cfg as NSString).deletingLastPathComponent
        let marker = dir + "/" + markerName
        if exists(marker) {
            var first = ""
            if case .ok(let d) = readRegular(marker, limit: 4096) {
                first = String(String(decoding: d, as: UTF8.self).split(whereSeparator: \.isNewline).first ?? "")
            }
            return .alreadyDone(first)
        }
        // config/ must be the profile's own directory, not a link to a shared one.
        guard let kind = (try? fm.attributesOfItem(atPath: dir))?[.type] as? FileAttributeType else { return .noConfig }
        guard kind == .typeDirectory else { return .unreadable("config/ is not a plain directory") }
        let data: Data
        switch readRegular(cfg, limit: maxBytes) {
        case .missing: return .noConfig
        case .refused(let why): return .unreadable("the config " + why)
        case .ok(let d): data = d
        }
        // Byte-exact round trip, or the "every other byte stays" proof means nothing.
        guard let text = String(data: data, encoding: .utf8), Data(text.utf8) == data else {
            return .unreadable("the config is not UTF-8 text")
        }
        let stamp = ISO8601DateFormatter().string(from: now)
        switch decide(text) {
        case .malformed(let why):
            return .unreadable(why)
        case .customized:
            writeMarker(marker, "\(version) skipped \(stamp): \(key) customized (left as is)")
            return .customized
        case .absent:
            writeMarker(marker, "\(version) skipped \(stamp): \(key) absent (the game's default applies)")
            return .absent
        case .rewrite(let out):
            let backup = cfg + backupSuffix
            let kept: Bool
            do { kept = try backUpOnce(data, to: backup) } catch {
                return .writeFailed("the backup (code \((error as NSError).code))")
            }
            do { try Data(out.utf8).write(to: URL(fileURLWithPath: cfg), options: .atomic) } catch {
                return .writeFailed("the config (code \((error as NSError).code))")
            }
            writeMarker(marker, "\(version) applied \(stamp): \(key) \(stock) -> \(target)\n"
                        + "original: \((backup as NSString).lastPathComponent)"
                        + (kept ? " (from an earlier attempt, kept)" : "") + "\n")
            return .applied(keptEarlierBackup: kept)
        }
    }

    /// Copy the original once, never over an existing backup: written whole
    /// under a temporary name, then hard-linked into place (link(2) refuses an
    /// existing name). true = a backup was already there and is kept.
    private static func backUpOnce(_ data: Data, to backup: String) throws -> Bool {
        let fm = FileManager.default
        if exists(backup) { return true }
        let tmp = backup + ".tmp-\(getpid())"
        try data.write(to: URL(fileURLWithPath: tmp), options: .atomic)
        defer { try? fm.removeItem(atPath: tmp) }
        do {
            try fm.linkItem(atPath: tmp, toPath: backup)
        } catch {
            if exists(backup) { return true }
            throw error
        }
        return false
    }

    private enum FileRead { case ok(Data), missing, refused(String) }

    /// Read a regular file of at most `limit` bytes without following a final
    /// symlink and without blocking on a FIFO.
    private static func readRegular(_ path: String, limit: Int) -> FileRead {
        let fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        if fd < 0 {
            let e = errno
            if e == ENOENT { return .missing }
            if e == ELOOP { return .refused("is a symlink") }
            return .refused("cannot be opened (errno \(e))")
        }
        defer { close(fd) }
        var st = stat()
        guard fstat(fd, &st) == 0 else { return .refused("cannot be examined") }
        guard st.st_mode & S_IFMT == S_IFREG else { return .refused("is not a regular file") }
        guard st.st_size <= off_t(limit) else { return .refused("is larger than 1 MiB") }
        let data: Data
        do {
            data = try FileHandle(fileDescriptor: fd, closeOnDealloc: false).read(upToCount: limit + 1) ?? Data()
        } catch {
            return .refused("cannot be read")
        }
        guard data.count <= limit else { return .refused("is larger than 1 MiB") }
        return .ok(data)
    }

    /// exists, without following a final symlink.
    private static func exists(_ p: String) -> Bool {
        (try? FileManager.default.attributesOfItem(atPath: p)) != nil
    }

    private static func writeMarker(_ path: String, _ text: String) {
        try? Data((text.hasSuffix("\n") ? text : text + "\n").utf8)
            .write(to: URL(fileURLWithPath: path), options: .atomic)
    }
}

#if MC_HUD_PITCH_TEST
extension MCHUDPitch {
    /// The Gson shape (pretty, one key per line, strings), with the old stock
    /// HUD values, plus keys that must survive: an unknown key holding "-2.0",
    /// a typed number -2.0, a value and an array element spelling the key name,
    /// and the escaped keyboard row.
    static let stockFixture = """
    {
      "mrMovingCamOffsetZ": "0.0",
      "forceGuiToHUD": "false",
      "keyboardKeys": "`1234567890-=qwertyuiop[]\\\\asdfghjkl;':\\"zxcvbnm,./?<>",
      "QUICKCOMMAND_9": "take this!",
      "QUICKCOMMAND_8": "hudPitchOffset",
      "hudDistance": "1.25",
      "hudPitchOffset": "-2.0",
      "guiScale": "0",
      "headHudScale": "1.0",
      "menuAlwaysFollowFace": "false",
      "vrHudLockMode": "WRIST",
      "vrHudWristOffset": "1.0",
      "someFuturePitch": "-2.0",
      "futureTyped": -2.0,
      "futureList": ["hudPitchOffset", "-2.0"],
      "vrEnabled": "true"
    }
    """

    static let pitchLine = "\"hudPitchOffset\": \"-2.0\""
    static let movedLine = "\"hudPitchOffset\": \"-15.0\""

    /// The same profile after MCUIComfort moved the HUD (HEAD / 2.0 / 0.8).
    static var comfortFixture: String {
        edit(edit(edit(stockFixture, "\"vrHudLockMode\": \"WRIST\"", "\"vrHudLockMode\": \"HEAD\""),
                  "\"hudDistance\": \"1.25\"", "\"hudDistance\": \"2.0\""),
             "\"headHudScale\": \"1.0\"", "\"headHudScale\": \"0.8\"")
    }

    /// A fixture edit that must apply (a typo would otherwise test nothing).
    static func edit(_ s: String, _ a: String, _ b: String) -> String {
        precondition(s.contains(a), "fixture pattern not found: \(a)")
        return s.replacingOccurrences(of: a, with: b)
    }

    static func same(_ a: String, _ b: String) -> Bool { Array(a.utf8) == Array(b.utf8) }

    /// nil = every check passed.
    static func selfTest() -> String? {
        // 1. Old stock: exactly the one value moves, every other byte stays.
        if topLevelCount(of: key, in: Array(stockFixture.utf8)) != 1 { return "scan: stock count" }
        guard case .rewrite(let out) = decide(stockFixture) else { return "stock: \(decide(stockFixture))" }
        if !same(out, edit(stockFixture, pitchLine, movedLine)) { return "stock: output differs from the expected bytes" }
        guard let d = parse(out) else { return "stock: output does not parse" }
        if d[key] as? String != target { return "stock: not -15.0" }
        if d["someFuturePitch"] as? String != "-2.0" { return "stock: unknown key holding -2.0 touched" }
        if (d["futureTyped"] as? NSNumber)?.doubleValue != -2.0 || d["futureTyped"] is String { return "stock: typed value touched" }
        if d["futureList"] as? [String] != ["hudPitchOffset", "-2.0"] { return "stock: array touched" }
        if d["QUICKCOMMAND_8"] as? String != "hudPitchOffset" { return "stock: value spelling the key touched" }
        if d["keyboardKeys"] as? String != "`1234567890-=qwertyuiop[]\\asdfghjkl;':\"zxcvbnm,./?<>" { return "stock: escapes damaged" }
        for (k, v) in [("vrHudLockMode", "WRIST"), ("hudDistance", "1.25"), ("headHudScale", "1.0"),
                       ("forceGuiToHUD", "false"), ("menuAlwaysFollowFace", "false"), ("guiScale", "0")]
        where d[k] as? String != v { return "stock: \(k) touched" }
        if d.count != 16 { return "stock: key count \(d.count)" }

        // 2. Already comfort-moved (HEAD / 2.0 / 0.8): the pitch still moves, the rest stays.
        let comfort = comfortFixture
        guard case .rewrite(let outC) = decide(comfort) else { return "comfort: \(decide(comfort))" }
        if !same(outC, edit(comfort, pitchLine, movedLine)) { return "comfort: output differs" }

        // 3. Other line shapes Gson or an editor can produce.
        let crlf = stockFixture.replacingOccurrences(of: "\n", with: "\r\n")
        guard case .rewrite(let outR) = decide(crlf), same(outR, edit(crlf, pitchLine, movedLine))
        else { return "crlf: \(decide(crlf))" }
        let last = edit(edit(stockFixture, "  \(pitchLine),\n", ""),
                        "  \"vrEnabled\": \"true\"\n", "  \"vrEnabled\": \"true\",\n  \(pitchLine)\n")
        guard case .rewrite(let outL) = decide(last), same(outL, edit(last, pitchLine, movedLine))
        else { return "last key: \(decide(last))" }
        let tabbed = edit(stockFixture, "  \(pitchLine),", "\t\(pitchLine),  ")
        guard case .rewrite(let outT) = decide(tabbed), same(outT, edit(tabbed, pitchLine, movedLine))
        else { return "tabbed: \(decide(tabbed))" }

        // 4. Anything but the exact string "-2.0" is the player's: left alone.
        for v in ["\"-10.0\"", "-2", "-2.0", "\"-2\"", "\"-2.00\"", "\" -2.0\"", "\"-15.0\"", "null"] {
            let s = edit(stockFixture, pitchLine, "\"hudPitchOffset\": " + v)
            if decide(s) != .customized { return "custom \(v): \(decide(s))" }
        }

        // 5. Absent: nothing to move (the game's default applies).
        let absent = edit(stockFixture, "  \(pitchLine),\n", "")
        if decide(absent) != .absent { return "absent: \(decide(absent))" }
        if decide("{}") != .absent { return "empty object: \(decide("{}"))" }
        let nestedOnly = edit(absent, "  \"vrEnabled\": \"true\"\n",
                              "  \"vrEnabled\": \"true\",\n  \"futureObj\": {\n    \(pitchLine)\n  }\n")
        if decide(nestedOnly) != .absent { return "nested only: \(decide(nestedOnly))" }

        // 6. Idempotent: the result is no longer stock.
        if decide(out) != .customized { return "idempotency: \(decide(out))" }
        if decide(outC) != .customized { return "idempotency (comfort): \(decide(outC))" }

        // 7. Refused without a rewrite.
        let refused: [(String, String)] = [
            ("not json", "not json"),
            ("array", "[\"a\"]"),
            ("empty", ""),
            ("truncated", "{ \"hudPitchOffset\": "),
            ("duplicate", edit(stockFixture, "  \"someFuturePitch\": \"-2.0\",\n", "  \(pitchLine),\n")),
            ("duplicate mixed", edit(stockFixture, "  \"someFuturePitch\": \"-2.0\",\n", "  \"hudPitchOffset\": \"-10.0\",\n")),
            ("duplicate escaped", edit(stockFixture, "  \"someFuturePitch\": \"-2.0\",\n", "  \"hud\\u0050itchOffset\": \"-10.0\",\n")),
            ("escaped key", edit(stockFixture, pitchLine, "\"hud\\u0050itchOffset\": \"-2.0\"")),
            ("compact", "{\"vrEnabled\": \"true\", \(pitchLine)}"),
            ("nested and top", edit(stockFixture, "  \"vrEnabled\": \"true\"\n",
                                    "  \"vrEnabled\": \"true\",\n  \"futureObj\": {\n    \(pitchLine)\n  }\n")),
            ("oversize", edit(stockFixture, "\"take this!\"", "\"" + String(repeating: "x", count: maxBytes) + "\"")),
        ]
        for (name, s) in refused {
            if case .malformed = decide(s) {} else { return "\(name): accepted (\(decide(s)))" }
        }
        return nil
    }

    /// The file half, in `dir` (a fresh temporary directory).
    static func fileSelfTest(in dir: String) -> String? {
        let fm = FileManager.default
        func read(_ p: String) -> Data? { fm.contents(atPath: p) }
        func put(_ p: String, _ s: String) throws { try Data(s.utf8).write(to: URL(fileURLWithPath: p)) }
        func profile(_ name: String, _ config: String?) throws -> String {
            let p = dir + "/" + name
            try fm.createDirectory(atPath: p + "/config", withIntermediateDirectories: true)
            try fm.createDirectory(atPath: p + "/saves/World", withIntermediateDirectories: true)
            try put(p + "/saves/World/level.dat", "level")
            try put(p + "/options.txt", "guiScale:0\n")
            try put(p + "/.account", "Player\n")
            if let config { try put(p + "/" + configRelative, config) }
            return p
        }
        func kept(_ p: String) -> Bool {
            read(p + "/saves/World/level.dat") == Data("level".utf8)
                && read(p + "/options.txt") == Data("guiScale:0\n".utf8)
                && read(p + "/.account") == Data("Player\n".utf8)
        }
        func marker(_ p: String) -> String? { try? String(contentsOfFile: p + "/config/" + markerName, encoding: .utf8) }
        func hasMarker(_ p: String) -> Bool { exists(p + "/config/" + markerName) }
        let stockData = Data(stockFixture.utf8)
        let movedData = Data(edit(stockFixture, pitchLine, movedLine).utf8)
        do {
            // Old stock: applied, backup is the original, marker, second pass a no-op.
            let a = try profile("stock", stockFixture)
            let cfg = a + "/" + configRelative
            if apply(profileDir: a) != .applied(keptEarlierBackup: false) { return "stock: not applied" }
            if read(cfg) != movedData { return "stock: config bytes differ" }
            if read(cfg + backupSuffix) != stockData { return "stock: backup missing or differs" }
            guard let m = marker(a), m.hasPrefix("\(version) applied ") else { return "stock: marker missing" }
            if m.contains("take this!") { return "stock: marker carries a config value" }
            let first = String(m.split(whereSeparator: \.isNewline).first ?? "")
            if apply(profileDir: a) != .alreadyDone(first) { return "stock: second apply is not a no-op" }
            if read(cfg) != movedData || read(cfg + backupSuffix) != stockData { return "stock: second apply changed a file" }
            if !kept(a) { return "stock: saves/options.txt/.account touched" }
            if try fm.contentsOfDirectory(atPath: a + "/config").contains(where: { $0.contains(".tmp") }) {
                return "stock: temporary file left"
            }
            // The player picks -2.0 in game afterwards: kept.
            try put(cfg, stockFixture)
            if case .alreadyDone = apply(profileDir: a) {} else { return "stock: ran again after the player's choice" }
            if read(cfg) != stockData { return "stock: the player's later -2.0 was changed" }

            // Even with the marker lost, a re-run changes nothing.
            let r = try profile("rerun", stockFixture)
            if apply(profileDir: r) != .applied(keptEarlierBackup: false) { return "rerun: not applied" }
            try fm.removeItem(atPath: r + "/config/" + markerName)
            if apply(profileDir: r) != .customized { return "rerun: \(apply(profileDir: r))" }
            if read(r + "/" + configRelative) != movedData || read(r + "/" + configRelative + backupSuffix) != stockData {
                return "rerun: a file changed"
            }

            // Comfort already applied, its marker present: the pitch still moves,
            // the comfort marker and backup are not touched.
            let b = try profile("comfort", comfortFixture)
            let oldMarker = b + "/config/.minecraft-hud-comfort-v1"
            let oldBackup = b + "/" + configRelative + ".minecraft-hud-comfort-v1.orig"
            try put(oldMarker, "v1 applied 2026-10-01T00:00:00Z: vrHudLockMode,hudDistance,headHudScale\n")
            try put(oldBackup, stockFixture)
            let oldMarkerData = read(oldMarker)
            if apply(profileDir: b) != .applied(keptEarlierBackup: false) { return "comfort: not applied" }
            if read(b + "/" + configRelative) != Data(edit(comfortFixture, pitchLine, movedLine).utf8) {
                return "comfort: config bytes differ"
            }
            if read(oldMarker) != oldMarkerData || read(oldBackup) != stockData { return "comfort: old comfort files touched" }
            if !hasMarker(b) || !kept(b) { return "comfort: marker missing or profile touched" }

            // Custom values, string and number: config untouched, no backup, marker without the value.
            for (name, v) in [("custom-string", "\"-10.0\""), ("custom-number", "-2")] {
                let text = edit(stockFixture, pitchLine, "\"hudPitchOffset\": " + v)
                let p = try profile(name, text)
                let c = p + "/" + configRelative
                if apply(profileDir: p) != .customized { return "\(name): \(apply(profileDir: p))" }
                if read(c) != Data(text.utf8) { return "\(name): config changed" }
                if exists(c + backupSuffix) { return "\(name): backup written" }
                guard let pm = marker(p), pm.contains("customized"), !pm.contains("-10.0") else { return "\(name): marker" }
                if !kept(p) { return "\(name): profile touched" }
            }

            // Absent: recorded, untouched (the new Java's default covers it).
            let absentText = edit(stockFixture, "  \(pitchLine),\n", "")
            let e = try profile("absent", absentText)
            if apply(profileDir: e) != .absent { return "absent: \(apply(profileDir: e))" }
            if read(e + "/" + configRelative) != Data(absentText.utf8) { return "absent: config changed" }
            if exists(e + "/" + configRelative + backupSuffix) { return "absent: backup written" }
            guard let em = marker(e), em.contains("absent") else { return "absent: marker missing" }

            // No config, or no config dir: defer, write nothing.
            let c = try profile("empty", nil)
            if apply(profileDir: c) != .noConfig { return "noConfig: \(apply(profileDir: c))" }
            if hasMarker(c) { return "noConfig: marker written" }
            let bare = dir + "/bare"
            try fm.createDirectory(atPath: bare, withIntermediateDirectories: true)
            if apply(profileDir: bare) != .noConfig { return "no config dir: \(apply(profileDir: bare))" }
            if exists(bare + "/config") { return "no config dir: config/ created" }

            // Refused files: nothing written, no marker, no backup, the next launch looks again.
            func refuse(_ name: String, _ p: String, _ check: () -> Bool = { true }) -> String? {
                if case .unreadable = apply(profileDir: p) {} else { return "\(name): \(apply(profileDir: p))" }
                if hasMarker(p) { return "\(name): marker written" }
                if exists(p + "/" + configRelative + backupSuffix) { return "\(name): backup written" }
                return check() ? nil : "\(name): a file changed"
            }
            let brokenText = "{ \"hudPitchOffset\": "
            let br = try profile("broken", brokenText)
            if let err = refuse("broken", br, { read(br + "/" + configRelative) == Data(brokenText.utf8) }) { return err }
            let dupText = edit(stockFixture, "  \"someFuturePitch\": \"-2.0\",\n", "  \(pitchLine),\n")
            let du = try profile("duplicate", dupText)
            if let err = refuse("duplicate", du, { read(du + "/" + configRelative) == Data(dupText.utf8) }) { return err }

            let elsewhere = dir + "/elsewhere.json"
            try put(elsewhere, stockFixture)
            let ln = try profile("symlink", nil)
            try fm.createSymbolicLink(atPath: ln + "/" + configRelative, withDestinationPath: elsewhere)
            if let err = refuse("symlink", ln, { read(elsewhere) == stockData }) { return err }

            let dc = try profile("dirconfig", nil)
            try fm.createDirectory(atPath: dc + "/" + configRelative, withIntermediateDirectories: false)
            if let err = refuse("directory", dc) { return err }

            let ff = try profile("fifo", nil)
            if mkfifo(ff + "/" + configRelative, 0o600) != 0 { return "fifo: mkfifo failed" }
            if let err = refuse("fifo", ff) { return err }

            let bigText = edit(stockFixture, "\"take this!\"", "\"" + String(repeating: "x", count: maxBytes) + "\"")
            let bg = try profile("oversize", bigText)
            if let err = refuse("oversize", bg, { read(bg + "/" + configRelative) == Data(bigText.utf8) }) { return err }

            let nu = try profile("not-utf8", nil)
            try Data([0x7B, 0xFF, 0x7D]).write(to: URL(fileURLWithPath: nu + "/" + configRelative))
            if let err = refuse("not utf8", nu) { return err }

            let shared = dir + "/shared-config"
            try fm.createDirectory(atPath: shared, withIntermediateDirectories: true)
            try put(shared + "/vivecraft-client-config.json", stockFixture)
            let ld = dir + "/linkdir"
            try fm.createDirectory(atPath: ld, withIntermediateDirectories: true)
            try fm.createSymbolicLink(atPath: ld + "/config", withDestinationPath: shared)
            if case .unreadable = apply(profileDir: ld) {} else { return "config dir symlink: \(apply(profileDir: ld))" }
            if read(shared + "/vivecraft-client-config.json") != stockData || exists(shared + "/" + markerName) {
                return "config dir symlink: the shared dir was written"
            }

            // An earlier backup is never overwritten.
            let k = try profile("backup-kept", stockFixture)
            let kc = k + "/" + configRelative
            try put(kc + backupSuffix, "older\n")
            if apply(profileDir: k) != .applied(keptEarlierBackup: true) { return "backup kept: \(apply(profileDir: k))" }
            if read(kc + backupSuffix) != Data("older\n".utf8) { return "backup kept: backup overwritten" }
            if read(kc) != movedData { return "backup kept: config not moved" }
        } catch {
            return "fixture setup failed: \(error)"
        }
        return nil
    }
}

@main
struct MCHUDPitchTestMain {
    static func main() {
        if let err = MCHUDPitch.selfTest() {
            fputs("FAIL decide: \(err)\n", stderr)
            exit(1)
        }
        let dir = NSTemporaryDirectory() + "mc-hud-pitch-\(getpid())"
        defer { try? FileManager.default.removeItem(atPath: dir) }
        if let err = MCHUDPitch.fileSelfTest(in: dir) {
            fputs("FAIL apply: \(err)\n", stderr)
            exit(1)
        }
        print("ok")
    }
}
#endif
