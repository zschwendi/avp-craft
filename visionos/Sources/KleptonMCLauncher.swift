import SwiftUI
import Foundation
import AVFoundation
import Compression
#if canImport(UIKit)
import UIKit
#endif

// The Minecraft launcher window (the vivecraft row only): pick an account,
// pick what to play, pick a resolution, answer the microphone prompt, Launch.
// It replaces BootView's generic report for this row. Nothing in this window
// navigates to that report (ui70): About has credits and an export of the
// diagnostics files; a failed launch shows a plain sentence, a short code and
// the same export. `onDetails` stays in the signature for BootView and the
// script's `details` step only; BootView is being changed to show this window
// for the Minecraft row regardless of KL_LAUNCHER or `details`.
//
// Product copy only (ui70): nothing on the normal path names a KL_* knob, the
// env file, the boot, an APK or JNI. Failures are never hidden: the actual
// status is written verbatim to a private launch report in Documents
// (MCFailureReport), which Export shares together with the runtime log; on
// screen it is a short code the player can quote. The loading state is a
// compact progress strip whose stage text comes from the milestones kl_app.c
// itself prints into the boot log (MCLaunchProgress).
//
// Nothing Mojang ships is bundled. The look — panorama, logo, stone buttons,
// the pixel font, the Steve head — is read at RUNTIME from the install the
// player staged (Documents/vivecraft): launch.txt names the game dir, the
// assets dir + index and the client jar, exactly as kl_app.c reads them, so the
// launcher shows the same instance the boot will run. Any piece that is missing
// falls back to SF Symbols on a dark background; the launcher never fails
// because a texture did.
//
// What Launch does (MCLauncherModel.prepareForLaunch), then BootView.boot():
//   account      MSAAccounts.selection (= MSAStore.bootChoice), already set by
//                the card tap, which also makes it the default.
//   play target  KL_MC_LAUNCH_ARGV, one arg per line, setenv()'d here and read
//                by kl_app.c native_launcher_args():
//                  Continue    --quickPlaySingleplayer <save folder>
//                  Title       (nothing)
//                  Join server --quickPlayMultiplayer <host>
//   resolution   KL_OVRP_EYE_MAX=<n> in Documents/klepton-env.txt (which wins
//                over the row); Sharp removes the line so the row's 4864 applies.
//                Every other line of that file is kept as it was.
//
// Autoboot: NONE for this row (build 71). The game starts only from the
// labelled Play action; KL_AUTOBOOT is ignored for Minecraft in
// BootView.autobootIfAsked. A deliberate scripted test uses KL_LAUNCHER_SCRIPT's
// explicit `mode=…;launch`, which takes the same Play path.
//
// Play mode (build 71, MCPlayMode.swift): Windowed or Immersive VR, chosen in
// the Play panel BEFORE the labelled Play action, windowed by default. The last
// choice is remembered as a PRESELECTION only — the panel says "default" or
// "remembered" — and the player presses "Play in VR" each session. The choice
// is exported as KL_IMMERSIVE (environment + klepton-env.txt) at Launch, so no
// path enters the space without it. Fixed for the process once a game has
// booted; the panel says so.

// --- env: the launch environment and Documents/klepton-env.txt ------------------

enum MCEnv {
    static var envFileURL: URL { URL(fileURLWithPath: Paths.container).appendingPathComponent("klepton-env.txt") }

    /// NAME=VALUE lines of the env file (comments/blank skipped), last one wins —
    /// the same reading kl_app.c's kl_env_file_apply does.
    static func fileValue(_ name: String) -> String? {
        guard let t = try? String(contentsOf: envFileURL, encoding: .utf8) else { return nil }
        var v: String?
        for raw in t.split(whereSeparator: \.isNewline) {
            let l = raw.trimmingCharacters(in: .whitespaces)
            guard !l.hasPrefix("#"), let eq = l.firstIndex(of: "="), eq != l.startIndex else { continue }
            if l[..<eq] == name { v = String(l[l.index(after: eq)...]) }
        }
        return v
    }

    /// The env file wins over the launch environment, as it does in C.
    static func value(_ name: String) -> String? {
        fileValue(name) ?? ProcessInfo.processInfo.environment[name]
    }

    static func on(_ name: String, default dflt: Bool) -> Bool {
        guard let v = value(name) else { return dflt }
        return !["", "0", "no", "off", "false"].contains(v.lowercased())
    }

    /// Replace every `name=` line with `name=value` (nil = just remove them),
    /// keeping all other lines byte-for-byte. A file left with nothing in it
    /// that this created is removed.
    static func setFileValue(_ name: String, _ value: String?) {
        let url = envFileURL
        let old = (try? String(contentsOf: url, encoding: .utf8))
        var lines = (old ?? "").components(separatedBy: "\n")
        if lines.last == "" { lines.removeLast() }
        lines.removeAll { l in
            let t = l.trimmingCharacters(in: .whitespaces)
            return t.hasPrefix(name + "=")
        }
        if let value { lines.append("\(name)=\(value)") }
        let out = lines.isEmpty ? "" : lines.joined(separator: "\n") + "\n"
        if out == (old ?? "") { return }
        if out.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            try? FileManager.default.removeItem(at: url)
        } else {
            try? out.write(to: url, atomically: true, encoding: .utf8)
        }
    }
}

enum MCLauncher {
    /// The themed window: the Minecraft row, unless KL_LAUNCHER=0.
    static var enabled: Bool { MSAAccounts.isMinecraft && MCEnv.on("KL_LAUNCHER", default: true) }
    /// The launcher boots on its own only when asked to, explicitly.
    static var autoboot: Bool { MCEnv.on("KL_AUTOBOOT", default: false) }
}

// --- bytes: raw deflate, gzip, a zip central directory ----------------------------

enum MCBytes {
    /// Raw DEFLATE (zip method 8, the gzip payload) -> `size` bytes.
    static func inflate(_ src: Data, size: Int) -> Data? {
        guard size > 0, size < 256 << 20 else { return nil }
        var out = Data(count: size)
        let n = out.withUnsafeMutableBytes { dst in
            src.withUnsafeBytes { s in
                compression_decode_buffer(dst.bindMemory(to: UInt8.self).baseAddress!, size,
                                          s.bindMemory(to: UInt8.self).baseAddress!, src.count,
                                          nil, COMPRESSION_ZLIB)
            }
        }
        return n == size ? out : nil
    }

    static func gunzip(_ d: Data) -> Data? {
        let b = [UInt8](d)
        guard b.count > 18, b[0] == 0x1f, b[1] == 0x8b, b[2] == 8 else { return nil }
        let flg = b[3]
        var p = 10
        if flg & 4 != 0 { guard p + 2 <= b.count else { return nil }; p += 2 + (Int(b[p]) | Int(b[p + 1]) << 8) }
        if flg & 8 != 0 { while p < b.count, b[p] != 0 { p += 1 }; p += 1 }
        if flg & 16 != 0 { while p < b.count, b[p] != 0 { p += 1 }; p += 1 }
        if flg & 2 != 0 { p += 2 }
        guard p < b.count - 8 else { return nil }
        let n = b.count
        let isize = Int(b[n - 4]) | Int(b[n - 3]) << 8 | Int(b[n - 2]) << 16 | Int(b[n - 1]) << 24
        return inflate(Data(b[p..<(n - 8)]), size: isize)
    }
}

/// Just enough of a zip reader for a jar: the central directory, then stored
/// or deflated entries by name. Indexes only names under `prefix`.
final class MCZip {
    private let data: Data
    private var entries: [String: (off: Int, csize: Int, usize: Int, method: Int)] = [:]

    init?(path: String, prefix: String) {
        guard let d = try? Data(contentsOf: URL(fileURLWithPath: path), options: .alwaysMapped) else { return nil }
        data = d
        let ok: Bool = d.withUnsafeBytes { raw -> Bool in
            let n = raw.count
            func u16(_ o: Int) -> Int { Int(raw.loadUnaligned(fromByteOffset: o, as: UInt16.self).littleEndian) }
            func u32(_ o: Int) -> Int { Int(raw.loadUnaligned(fromByteOffset: o, as: UInt32.self).littleEndian) }
            guard n >= 22 else { return false }
            var e = n - 22
            let stop = max(0, n - 22 - 65535)
            while e >= stop, u32(e) != 0x06054b50 { e -= 1 }
            guard e >= stop else { return false }
            let cdSize = u32(e + 12), cdOff = u32(e + 16)
            guard cdOff + cdSize <= n else { return false }
            var p = cdOff
            let pre = Array(prefix.utf8)
            while p + 46 <= cdOff + cdSize, u32(p) == 0x02014b50 {
                let method = u16(p + 10), csize = u32(p + 20), usize = u32(p + 24)
                let nl = u16(p + 28), el = u16(p + 30), cl = u16(p + 32), lo = u32(p + 42)
                if nl >= pre.count, p + 46 + nl <= n,
                   memcmp(raw.baseAddress! + p + 46, pre, pre.count) == 0,
                   let name = String(bytes: UnsafeRawBufferPointer(rebasing: raw[(p + 46)..<(p + 46 + nl)]),
                                     encoding: .utf8) {
                    entries[name] = (lo, csize, usize, method)
                }
                p += 46 + nl + el + cl
            }
            return true
        }
        if !ok { return nil }
    }

    func entry(_ name: String) -> Data? {
        guard let e = entries[name], e.off + 30 <= data.count else { return nil }
        let start: Int = data.withUnsafeBytes { raw in
            let nl = Int(raw.loadUnaligned(fromByteOffset: e.off + 26, as: UInt16.self).littleEndian)
            let el = Int(raw.loadUnaligned(fromByteOffset: e.off + 28, as: UInt16.self).littleEndian)
            return e.off + 30 + nl + el
        }
        guard start + e.csize <= data.count else { return nil }
        let body = data.subdata(in: start..<(start + e.csize))
        switch e.method {
        case 0: return body
        case 8: return MCBytes.inflate(body, size: e.usize)
        default: return nil
        }
    }
}

// --- the staged instance: what launch.txt points at ---------------------------------

struct MCInstance {
    var root: String          // <container>/vivecraft/root
    var gameDir: String?
    var assetsDir: String?
    var assetIndex: String?
    var gameJar: String?
    /// launch.txt's `--version` (the Minecraft version the boot runs), for the
    /// build-identity footer only. nil when launch.txt is absent or has none.
    var version: String?

    static func read(tree: String = "vivecraft") -> MCInstance {
        let base = Paths.container + "/" + tree
        var i = MCInstance(root: base + "/root")
        guard let t = try? String(contentsOfFile: base + "/launch.txt", encoding: .utf8) else { return i }
        func expand(_ s: String) -> String { s.hasPrefix("@ROOT@") ? i.root + s.dropFirst(6) : s }
        var prev = ""
        for raw in t.split(whereSeparator: \.isNewline) {
            let l = raw.trimmingCharacters(in: .whitespaces)
            if l.isEmpty || l.hasPrefix("#") { continue }
            switch prev {
            case "--gameDir": i.gameDir = expand(l)
            case "--assetsDir": i.assetsDir = expand(l)
            case "--assetIndex": i.assetIndex = l
            case "--version": i.version = l
            default: break
            }
            if l.hasPrefix("-Dfabric.gameJarPath=") { i.gameJar = expand(String(l.dropFirst(21))) }
            prev = l
        }
        // kl_app.c's own fallback when launch.txt names no game dir.
        if i.gameDir == nil { i.gameDir = i.root + "/fgamedir" }
        return i
    }
}

struct MCWorld: Equatable {
    var folder: String
    var name: String
    var lastPlayed: Date
    var iconPath: String?
}

enum MCSaves {
    /// Every save under <gameDir>/saves with a readable level.dat, newest first.
    static func scan(_ inst: MCInstance) -> [MCWorld] {
        guard let gd = inst.gameDir else { return [] }
        return scan(saves: gd + "/saves")
    }

    static func scan(saves: String) -> [MCWorld] {
        let fm = FileManager.default
        guard let dirs = try? fm.contentsOfDirectory(atPath: saves) else { return [] }
        var out: [MCWorld] = []
        for f in dirs where !f.hasPrefix(".") {
            let ld = "\(saves)/\(f)/level.dat"
            guard let raw = fm.contents(atPath: ld) else { continue }
            let mtime = (try? fm.attributesOfItem(atPath: ld)[.modificationDate] as? Date) ?? .distantPast
            var w = MCWorld(folder: f, name: f, lastPlayed: mtime)
            if let nbt = MCBytes.gunzip(raw) {
                if let n = nbtString(nbt, "LevelName"), !n.isEmpty { w.name = n }
                if let ms = nbtLong(nbt, "LastPlayed"), ms > 0 { w.lastPlayed = Date(timeIntervalSince1970: Double(ms) / 1000) }
            }
            let icon = "\(saves)/\(f)/icon.png"
            if fm.fileExists(atPath: icon) { w.iconPath = icon }
            out.append(w)
        }
        return out.sorted { $0.lastPlayed > $1.lastPlayed }
    }

    /// The first named tag (type + u16 name length + name) in the NBT stream.
    /// level.dat's Data compound holds LevelName/LastPlayed at its top level,
    /// before anything that could repeat the names.
    private static func find(_ d: [UInt8], type: UInt8, _ name: String) -> Int? {
        let n = Array(name.utf8)
        let pat = [type, UInt8(n.count >> 8), UInt8(n.count & 0xff)] + n
        guard d.count >= pat.count else { return nil }
        var i = 0
        while i + pat.count <= d.count {
            if d[i] == pat[0], Array(d[i..<(i + pat.count)]) == pat { return i + pat.count }
            i += 1
        }
        return nil
    }
    static func nbtString(_ data: Data, _ name: String) -> String? {
        let d = [UInt8](data)
        guard let p = find(d, type: 8, name), p + 2 <= d.count else { return nil }
        let len = Int(d[p]) << 8 | Int(d[p + 1])
        guard p + 2 + len <= d.count else { return nil }
        return String(bytes: d[(p + 2)..<(p + 2 + len)], encoding: .utf8)
    }
    static func nbtLong(_ data: Data, _ name: String) -> Int64? {
        let d = [UInt8](data)
        guard let p = find(d, type: 4, name), p + 8 <= d.count else { return nil }
        return d[p..<(p + 8)].reduce(Int64(0)) { $0 << 8 | Int64($1) }
    }
}

// --- the theme: textures from the staged install --------------------------------------

/// A CGImage's RGBA bytes, for cropping glyphs and skin faces pixel-exactly.
struct MCPixels {
    let w: Int, h: Int
    var px: [UInt8]
    init?(_ img: CGImage) {
        w = img.width; h = img.height
        px = [UInt8](repeating: 0, count: w * h * 4)
        let ok = px.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                      bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        if !ok { return nil }
    }
    init(w: Int, h: Int) { self.w = w; self.h = h; px = [UInt8](repeating: 0, count: w * h * 4) }
    func alpha(_ x: Int, _ y: Int) -> UInt8 { px[(y * w + x) * 4 + 3] }
    mutating func put(_ x: Int, _ y: Int, _ r: UInt8, _ g: UInt8, _ b: UInt8, _ a: UInt8) {
        guard x >= 0, y >= 0, x < w, y < h else { return }
        let i = (y * w + x) * 4
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a
    }
    func copy(from s: MCPixels, sx: Int, sy: Int, sw: Int, sh: Int, dx: Int, dy: Int, over: Bool) -> MCPixels {
        var o = self
        for y in 0..<sh { for x in 0..<sw {
            guard sx + x < s.w, sy + y < s.h else { continue }
            let i = ((sy + y) * s.w + sx + x) * 4
            if over && s.px[i + 3] == 0 { continue }
            o.put(dx + x, dy + y, s.px[i], s.px[i + 1], s.px[i + 2], s.px[i + 3])
        } }
        return o
    }
    var cgImage: CGImage? {
        let data = Data(px) as CFData
        guard let prov = CGDataProvider(data: data) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                       space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: prov, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    /// A skin's head: face (8,8) with the hat layer (40,8) over it.
    static func head(fromSkin skin: CGImage) -> CGImage? {
        guard let s = MCPixels(skin), s.w >= 48, s.h >= 16 else { return nil }
        return MCPixels(w: 8, h: 8).copy(from: s, sx: 8, sy: 8, sw: 8, sh: 8, dx: 0, dy: 0, over: false)
            .copy(from: s, sx: 40, sy: 8, sw: 8, sh: 8, dx: 0, dy: 0, over: true).cgImage
    }
}

/// The 8x8 bitmap ASCII font (textures/font/ascii.png), drawn the game's way:
/// glyph width from its rightmost lit column, 1 px spacing, drop shadow at
/// (+1,+1) in a quarter of the colour.
struct MCFont {
    let atlas: MCPixels
    let cell: Int
    private var widths: [Int] = []

    init?(_ img: CGImage) {
        guard let p = MCPixels(img), p.w == p.h, p.w % 16 == 0 else { return nil }
        atlas = p; cell = p.w / 16
        for c in 0..<256 {
            let gx = (c % 16) * cell, gy = (c / 16) * cell
            var wmax = 0
            for x in 0..<cell { for y in 0..<cell where p.alpha(gx + x, gy + y) > 0 { wmax = max(wmax, x + 1); break } }
            widths.append(c == 32 ? cell / 2 - 1 : wmax)
        }
    }

    func render(_ text: String, rgb: (UInt8, UInt8, UInt8) = (255, 255, 255)) -> CGImage? {
        let codes = text.unicodeScalars.map { $0.value < 256 && $0.value >= 32 ? Int($0.value) : 63 }
        let width = max(1, codes.reduce(0) { $0 + widths[$1] + 1 } + 1)
        var out = MCPixels(w: width, h: cell + 1)
        let sh = (rgb.0 / 4, rgb.1 / 4, rgb.2 / 4)
        for pass in 0..<2 {
            var x0 = 0
            for c in codes {
                let gx = (c % 16) * cell, gy = (c / 16) * cell
                for y in 0..<cell { for x in 0..<widths[c] where atlas.alpha(gx + x, gy + y) > 0 {
                    if pass == 0 { out.put(x0 + x + 1, y + 1, sh.0, sh.1, sh.2, 255) }
                    else { out.put(x0 + x, y, rgb.0, rgb.1, rgb.2, 255) }
                } }
                x0 += widths[c] + 1
            }
        }
        return out.cgImage
    }
}

@MainActor
final class MCTheme: ObservableObject {
    static let shared = MCTheme()

    @Published var panorama: [UIImage] = []      // faces 0...3 (the horizontal ring)
    @Published var logo: UIImage?
    @Published var edition: UIImage?
    @Published var button: UIImage?
    @Published var buttonHi: UIImage?
    @Published var buttonOff: UIImage?
    @Published var dirt: UIImage?
    @Published var steve: UIImage?
    @Published var font: MCFont?
    /// The staged instance's Minecraft version (launch.txt --version), read with
    /// the theme so the build-identity footer can say "Minecraft 26.2" once it is
    /// known. nil until the theme has loaded, or when launch.txt has no version.
    @Published var instanceVersion: String?
    private var loaded = false
    private var textCache: [String: UIImage] = [:]

    func load() {
        guard !loaded else { return }
        loaded = true
        Task {
            let t0 = Date()
            let r = await Task.detached { MCTheme.read() }.value
            panorama = r.pano; logo = r.logo; edition = r.edition
            button = r.btn; buttonHi = r.btnHi; buttonOff = r.btnOff
            dirt = r.dirt; steve = r.steve; font = r.font
            instanceVersion = r.version
            NSLog("[launcher] theme: panorama %d/4, logo %@, buttons %@, font %@, steve %@ (%.2f s)",
                  r.pano.count, r.logo != nil ? "yes" : "no", r.btn != nil ? "yes" : "no",
                  r.font != nil ? "yes" : "no", r.steve != nil ? "yes" : "no", Date().timeIntervalSince(t0))
        }
    }

    private struct Loaded {
        var pano: [UIImage] = []
        var logo, edition, btn, btnHi, btnOff, dirt, steve: UIImage?
        var font: MCFont?
        var version: String?
    }

    nonisolated private static func read() -> Loaded {
        var r = Loaded()
        let inst = MCInstance.read()
        r.version = inst.version
        func png(_ d: Data?) -> UIImage? { d.flatMap { UIImage(data: $0) } }
        // Panorama: the asset index (hash per version) -> objects/<h2>/<hash>.
        if let ad = inst.assetsDir, let idx = inst.assetIndex,
           let d = FileManager.default.contents(atPath: "\(ad)/indexes/\(idx).json"),
           let objs = ((try? JSONSerialization.jsonObject(with: d)) as? [String: Any])?["objects"] as? [String: Any] {
            for i in 0..<4 {
                guard let o = objs["minecraft/textures/gui/title/background/panorama_\(i).png"] as? [String: Any],
                      let h = o["hash"] as? String, h.count > 2,
                      let img = png(FileManager.default.contents(atPath: "\(ad)/objects/\(h.prefix(2))/\(h)"))
                else { r.pano = []; break }
                r.pano.append(img)
            }
        }
        // Logo, buttons, font, dirt, Steve: the client jar.
        if let jar = inst.gameJar, let z = MCZip(path: jar, prefix: "assets/minecraft/textures/") {
            let t = "assets/minecraft/textures/"
            r.logo = png(z.entry(t + "gui/title/minecraft.png"))
            r.edition = png(z.entry(t + "gui/title/edition.png"))
            r.btn = png(z.entry(t + "gui/sprites/widget/button.png"))
            r.btnHi = png(z.entry(t + "gui/sprites/widget/button_highlighted.png"))
            r.btnOff = png(z.entry(t + "gui/sprites/widget/button_disabled.png"))
            r.dirt = png(z.entry(t + "block/dirt.png"))
            if let f = png(z.entry(t + "font/ascii.png"))?.cgImage { r.font = MCFont(f) }
            if let s = png(z.entry(t + "entity/player/wide/steve.png"))?.cgImage,
               let h = MCPixels.head(fromSkin: s) { r.steve = UIImage(cgImage: h) }
        }
        // A panorama of a stub (the jar's 69-byte placeholders) is not one.
        if r.pano.contains(where: { $0.size.width < 64 }) { r.pano = [] }
        return r
    }

    /// Pixel-font text, cached per string+colour.
    func text(_ s: String, rgb: (UInt8, UInt8, UInt8) = (255, 255, 255)) -> UIImage? {
        let key = "\(rgb.0),\(rgb.1),\(rgb.2):\(s)"
        if let c = textCache[key] { return c }
        guard let f = font, let cg = f.render(s, rgb: rgb) else { return nil }
        let img = UIImage(cgImage: cg)
        textCache[key] = img
        return img
    }
}

// --- player heads ---------------------------------------------------------------------

@MainActor
final class MCHeads: ObservableObject {
    static let shared = MCHeads()
    @Published private(set) var heads: [String: UIImage] = [:]   // by uuid (lowercased, undashed)
    private var asked: Set<String> = []

    private static var cacheDir: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("klepton-mc-heads", isDirectory: true)
    }
    static func key(_ uuid: String) -> String { uuid.lowercased().replacingOccurrences(of: "-", with: "") }

    func head(for a: MSAAccount) -> UIImage? {
        guard a.resolved else { return nil }
        let k = MCHeads.key(a.uuid)
        if let h = heads[k] { return h }
        if !asked.contains(k) { asked.insert(k); fetch(k, skin: a.skin) }
        return nil
    }

    private func fetch(_ k: String, skin: String?) {
        let file = MCHeads.cacheDir.appendingPathComponent(k + ".png")
        Task {
            // The cached head first (no network), refreshed in the background.
            if let d = try? Data(contentsOf: file), let img = UIImage(data: d) { heads[k] = img }
            let got: Data? = await Task.detached { () -> Data? in
                var url = skin.flatMap(URL.init(string:))
                if url == nil {
                    // Accounts stored before the skin URL was: the public
                    // profile (no token) carries it, base64 in "textures".
                    guard let pu = URL(string: "https://sessionserver.mojang.com/session/minecraft/profile/\(k)"),
                          let pr = try? await URLSession.shared.data(from: pu),
                          let obj = (try? JSONSerialization.jsonObject(with: pr.0)) as? [String: Any],
                          let props = obj["properties"] as? [[String: Any]],
                          let v = props.first(where: { $0["name"] as? String == "textures" })?["value"] as? String,
                          let td = Data(base64Encoded: v),
                          let tex = (try? JSONSerialization.jsonObject(with: td)) as? [String: Any],
                          let s = ((tex["textures"] as? [String: Any])?["SKIN"] as? [String: Any])?["url"] as? String
                    else { return nil }
                    url = URL(string: s)
                }
                // textures.minecraft.net answers https too; ATS refuses plain http.
                if let u = url, u.scheme == "http" {
                    url = URL(string: u.absoluteString.replacingOccurrences(of: "http://", with: "https://"))
                }
                guard let u = url, let sr = try? await URLSession.shared.data(from: u),
                      let cg = UIImage(data: sr.0)?.cgImage, let h = MCPixels.head(fromSkin: cg) else { return nil }
                return UIImage(cgImage: h).pngData()
            }.value
            if let got, let img = UIImage(data: got) {
                heads[k] = img
                try? FileManager.default.createDirectory(at: MCHeads.cacheDir, withIntermediateDirectories: true)
                try? got.write(to: file, options: .atomic)
            }
        }
    }
}

// --- the model: play target, resolution, microphone --------------------------------------

@MainActor
final class MCLauncherModel: ObservableObject {
    static let shared = MCLauncherModel()

    enum Play: String { case resume, title, server }
    enum Resolution: Int, CaseIterable {
        case balanced = 4608, sharp = 4864, max = 5088
        var label: String { self == .balanced ? "Balanced" : self == .sharp ? "Sharp" : "Max" }
    }
    enum Mic { case undeclared, undetermined, asking, granted, denied }

    @Published var play: Play { didSet { UserDefaults.standard.set(play.rawValue, forKey: "mclauncher.play") } }
    @Published var server: String { didSet { UserDefaults.standard.set(server, forKey: "mclauncher.server") } }
    /// Windowed or Immersive VR — the player's explicit choice, windowed until
    /// chosen otherwise (MCPlayMode.swift). Exported at every Launch.
    @Published var playMode: MCPlayMode { didSet { MCPlayMode.store(playMode) } }
    @Published var resolution: Resolution?       // nil = the env file holds another value
    @Published var customEye: String?
    @Published var worlds: [MCWorld] = []
    @Published var mic: Mic = .undetermined
    @Published var scanned = false
    /// BootView shows its report instead of the launcher ("Details").
    @Published var details = false
    var scriptStarted = false
    /// Games that have ended back to this window (quit to the launcher).
    @Published var gamesEnded = 0
    /// The game closed its window and the run is ending (the space is already gone).
    @Published var quitting = false

    /// The game returned and the window is the launcher again: the saves may
    /// have changed (Continue's newest world), and so may the env file.
    func gameEnded() {
        gamesEnded += 1
        MCLaunchProgress.shared.end()
        refresh()
    }

    private init() {
        play = Play(rawValue: UserDefaults.standard.string(forKey: "mclauncher.play") ?? "") ?? .title
        server = UserDefaults.standard.string(forKey: "mclauncher.server") ?? ""
        playMode = MCPlayMode.stored
        readResolution()
    }

    /// The mode this process is committed to, once a game has booted: the
    /// chooser locks to it and the caption says how to change it.
    var playModeFixed: Bool { MCPlayMode.applied != nil }

    var newest: MCWorld? { worlds.first }

    func refresh() {
        readResolution()
        rescanWorlds()
        checkMic()
    }

    /// The SELECTED card's worlds (its profile's saves — MCProfiles.savesDir).
    func rescanWorlds() {
        let sel = MSAAccounts.shared.selection, tree = MSAAccounts.shared.tree
        Task {
            let (dir, w) = await Task.detached { () -> (String?, [MCWorld]) in
                let d = MCProfiles.savesDir(selection: sel, tree: tree)
                return (d, d.map { MCSaves.scan(saves: $0) } ?? [])
            }.value
            guard sel == MSAAccounts.shared.selection else { return }   // a newer pick is scanning
            worlds = w
            scanned = true
            if play == .resume, w.isEmpty {
                play = .title
                NSLog("[launcher] no saved world; Continue falls back to the title screen")
            }
            NSLog("[launcher] worlds: %d%@ in %@", w.count, w.first.map { " (newest \"\($0.name)\" in saves/\($0.folder))" } ?? "",
                  dir.map { ($0 as NSString).abbreviatingWithTildeInPath } ?? "(no profile yet)")
        }
    }

    private func readResolution() {
        if let v = MCEnv.fileValue("KL_OVRP_EYE_MAX") {
            resolution = Int(v).flatMap(Resolution.init(rawValue:))
            customEye = resolution == nil ? v : nil
        } else {
            resolution = .sharp; customEye = nil
        }
    }

    func setResolution(_ r: Resolution) {
        resolution = r; customEye = nil
        // Written now, not at Launch, so the file always says what the window shows.
        MCEnv.setFileValue("KL_OVRP_EYE_MAX", r == .sharp ? nil : String(r.rawValue))
        NSLog("[launcher] resolution %@ -> klepton-env.txt %@", r.label,
              r == .sharp ? "(line removed; row default 4864)" : "KL_OVRP_EYE_MAX=\(r.rawValue)")
    }

    // The prompt comes HERE, with the window up, rather than from
    // KleptonAudio.start() after boot, where it would land on top of the game.
    func checkMic() {
        guard Bundle.main.object(forInfoDictionaryKey: "NSMicrophoneUsageDescription") != nil else {
            mic = .undeclared; return
        }
        switch AVAudioApplication.shared.recordPermission {
        case .granted: mic = .granted
        case .denied: mic = .denied
        default:
            guard mic != .asking else { return }
            mic = .asking
            NSLog("[launcher] asking for the microphone before Launch")
            AVAudioApplication.requestRecordPermission { granted in
                NSLog("[launcher] record permission %@", granted ? "granted" : "DENIED")
                Task { @MainActor in MCLauncherModel.shared.mic = granted ? .granted : .denied }
            }
        }
    }

    var micAnswered: Bool { mic == .granted || mic == .denied || mic == .undeclared }

    var serverHost: String { server.trimmingCharacters(in: .whitespacesAndNewlines) }

    /// What Launch hands the boot. `applyPlay` is false for an explicit
    /// KL_AUTOBOOT, which keeps the dev run's own KL_MC_ARGS.
    func prepareForLaunch(applyPlay: Bool) {
        var argv: [String] = []
        if applyPlay {
            switch play {
            case .resume:
                if let w = newest {
                    argv = ["--quickPlaySingleplayer", w.folder]
                } else {
                    // play=resume with an empty list must not hand the game a
                    // missing world. The title screen is where Create New World is.
                    play = .title
                    NSLog("[launcher] no saved world; Launch opens the title screen")
                }
            case .title: break
            case .server: if !serverHost.isEmpty { argv = ["--quickPlayMultiplayer", serverHost] }
            }
        }
        // The account's own game dir (KleptonMCProfiles.swift): re-prepared on
        // every Launch, BEFORE the play target is read (a first launch may
        // have just moved the worlds in).
        MCProfiles.prepareForLaunch()
        if argv.isEmpty { unsetenv("KL_MC_LAUNCH_ARGV") }
        else { setenv("KL_MC_LAUNCH_ARGV", argv.joined(separator: "\n"), 1) }
        // The play mode, on EVERY path — a dev autoboot included — so the
        // runtime never falls back to its compiled-in VR default. A process
        // already committed to a mode re-exports that one; the chooser follows.
        let mode = MCPlayMode.apply(playMode)
        if mode != playMode { playMode = mode }
        let m = MSAAccounts.shared
        NSLog("[launcher] Launch: account %@, play %@, mode %@ (%@), eye %@", m.selectionLabel,
              argv.isEmpty ? "title screen" : argv.joined(separator: " "), mode.rawValue,
              MCPlayMode.isRemembered ? "remembered preselection, Play pressed" : "default, Play pressed",
              MCEnv.fileValue("KL_OVRP_EYE_MAX") ?? "row default")
    }
}

// --- launch progress (ui70) -------------------------------------------------------------
//
// The compact loading strip under Launch. BootView owns the boot thread and
// the full log; this reads only the TAIL of the same file (klepton-boot.log in
// the container — kl_app.c's native_configure names it, OwnershipGateLog
// appends to it) twice a second while a launch is starting, and turns the
// milestones kl_app.c prints into a sentence a player can read. Display only:
// nothing here is read by the launch, the gate or the boot, and a milestone
// that never appears simply leaves the previous sentence up.

/// (a line kl_app.c / the game prints, what the player reads). The marker
/// whose LAST occurrence is furthest down the tail wins, so a repeated line
/// cannot move the stage backwards. Two markers that start at the same place
/// ("[msa] offline-owned account" is also "[msa] offline") resolve to the one
/// listed FIRST — keep the longer one first. The first group is kl_app.c's own
/// text (kl_app_boot, native_boot, native_profile_gamedir, native_msa_args,
/// native_launcher_args, native_build_argv); the second is the game's standard
/// console lines. File scope so the nonisolated scanner may read it.
private let mcLaunchMilestones: [(marker: String, text: String)] = [
    ("=== Klepton on visionOS", "Starting the runtime…"),
    ("=== the native guest", "Loading the game engine…"),
    (".dylib mapped in", "Game engine loaded…"),
    ("[native] game dir", "Preparing your profile…"),
    ("[msa] session for", "Signed in to your Microsoft account…"),
    ("[msa] offline-owned account", "Playing offline under your account name…"),
    ("[msa] offline", "Playing offline…"),
    ("[launcher] play target from the launcher window", "Opening your world…"),
    ("[native] command line", "Starting Minecraft…"),
    ("Setting user:", "Starting Minecraft…"),
    ("Backend library", "Starting the renderer…"),
    ("Reloading ResourceManager", "Loading resources…"),
    ("OpenAL initialized", "Starting audio…"),
]

@MainActor
final class MCLaunchProgress: ObservableObject {
    static let shared = MCLaunchProgress()

    @Published private(set) var stage = "Starting Minecraft…"
    @Published private(set) var started: Date?
    private var task: Task<Void, Never>?

    /// kl_app.c's boot log (native_configure names it); MCFailureReport exports it.
    nonisolated static var logURL: URL { URL(fileURLWithPath: Paths.container).appendingPathComponent("klepton-boot.log") }

    /// Idempotent: a second call while running is a no-op (the view calls it
    /// on appear as well as on the running edge, because "details" and back
    /// rebuild the view).
    func begin() {
        guard task == nil else { return }
        started = Date()
        stage = "Starting Minecraft…"
        let url = MCLaunchProgress.logURL
        task = Task { [weak self] in
            // Bounded: the window is hidden once the space is up, and the log
            // keeps growing for the whole session. Two minutes covers a load.
            let deadline = Date().addingTimeInterval(120)
            while !Task.isCancelled, Date() < deadline {
                let tail = await Task.detached { MCLaunchProgress.tail(url, bytes: 32 << 10) }.value
                if let tail, let s = MCLaunchProgress.stage(in: tail), let self, s != self.stage {
                    self.stage = s
                    NSLog("[launcher] progress: %@", s)
                }
                try? await Task.sleep(for: .milliseconds(500))
            }
            self?.task = nil
        }
    }

    func end() {
        task?.cancel()
        task = nil
        started = nil
    }

    nonisolated static func tail(_ url: URL, bytes: Int) -> String? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        guard let end = try? h.seekToEnd() else { return nil }
        let from = end > UInt64(bytes) ? end - UInt64(bytes) : 0
        guard (try? h.seek(toOffset: from)) != nil, let d = try? h.readToEnd() else { return nil }
        return String(decoding: d, as: UTF8.self)
    }

    nonisolated static func stage(in text: String) -> String? {
        var best: (pos: String.Index, text: String)?
        for m in mcLaunchMilestones {
            guard let r = text.range(of: m.marker, options: .backwards) else { continue }
            if best == nil || r.lowerBound > best!.pos { best = (r.lowerBound, m.text) }
        }
        return best?.text
    }
}

// --- a failed launch: the short code on screen, the whole truth in a file ----------------
//
// kl_app_status() is the real reason ("missing staged launch.txt (run
// stage_assets.sh): /…", "the native guest library did not load…"). It is not
// product copy, and a configure failure never reaches klepton-boot.log (the
// log is opened by boot, after configure), so the status would otherwise live
// only in a SwiftUI label. This writes it, verbatim, with the build identity
// and the time, to Documents/launch-report.txt — private to the device until
// the player taps Export — and shows a short code derived from it.

enum MCFailureReport {
    static var url: URL { URL(fileURLWithPath: Paths.container).appendingPathComponent("launch-report.txt") }

    /// What Export shares: the launch report and the runtime log, whichever exist.
    static var exportItems: [URL] {
        [url, MCLaunchProgress.logURL].filter { FileManager.default.fileExists(atPath: $0.path) }
    }

    /// "E-FILES-3fa2": a family from kl_app.c's own status families, and four
    /// hex digits of the exact text so the report line can be matched to it.
    static func code(for status: String) -> String {
        let s = status.lowercased()
        let family: String
        if s.hasPrefix("missing ") { family = "FILES" }                        // kl_app.c missing()
        else if s.contains("did not load") || s.contains("dlopen") { family = "ENGINE" }
        else if s.contains("already run") || s.contains("was not called") || s.contains("not booted") { family = "STATE" }
        else if s.contains("log file") { family = "LOG" }
        else if s.contains("thread") { family = "THREAD" }
        else { family = "START" }
        var h: UInt32 = 0x811c9dc5                                           // FNV-1a
        for b in status.utf8 { h = (h ^ UInt32(b)) &* 0x01000193 }
        return "E-\(family)-" + String(format: "%04x", h & 0xffff)
    }

    /// The sentence beside the code, by family.
    static func hint(for status: String) -> String {
        switch code(for: status).split(separator: "-")[1] {
        case "FILES": return "Some of the game's files are missing. Install the latest update, then try again."
        case "ENGINE": return "Minecraft could not start. Close and reopen the app, then try again."
        case "THREAD", "LOG": return "The device ran short of resources while starting. Close other apps and try again."
        default: return "Close and reopen the app to try again."
        }
    }

    /// Append one record. Never throws; display state does not depend on it.
    static func record(status: String) {
        let line = "\(ISO8601DateFormatter().string(from: Date()))  \(MCBuildIdentity.short) (\(MCBuildIdentity.bundleID))\n"
            + "code: \(code(for: status))\nstatus: \(status)\n\n"
        guard let data = line.data(using: .utf8) else { return }
        if let h = try? FileHandle(forWritingTo: url) {
            defer { try? h.close() }
            _ = try? h.seekToEnd()
            try? h.write(contentsOf: data)
        } else {
            try? data.write(to: url, options: .atomic)
        }
        NSLog("[launcher] launch failed: %@ — %@ (launch-report.txt)", code(for: status), status)
    }
}

// --- views ------------------------------------------------------------------------------

/// Pixel-font text when the font loaded, a heavy rounded system font otherwise.
struct MCText: View {
    @ObservedObject private var theme = MCTheme.shared
    let text: String
    var size: CGFloat = 22
    var rgb: (UInt8, UInt8, UInt8) = (255, 255, 255)

    init(_ text: String, size: CGFloat = 22, rgb: (UInt8, UInt8, UInt8) = (255, 255, 255)) {
        self.text = text; self.size = size; self.rgb = rgb
    }

    var body: some View {
        if let img = theme.text(text, rgb: rgb) {
            Image(uiImage: img).resizable().interpolation(.none)
                .frame(width: img.size.width * size / img.size.height, height: size)
                .accessibilityLabel(text)
        } else {
            Text(text).font(.system(size: size * 0.8, weight: .heavy, design: .rounded))
                .foregroundStyle(Color(red: Double(rgb.0) / 255, green: Double(rgb.1) / 255, blue: Double(rgb.2) / 255))
                .shadow(color: .black.opacity(0.6), radius: 0, x: 2, y: 2)
        }
    }
}

/// The game's stone button: the 200x20 nine-slice (3 px border) at 4 pt per
/// texel, tiled, nearest-neighbour. Falls back to a dark rounded plate.
struct MCButtonStyle: ButtonStyle {
    var selected = false
    var height: CGFloat = 56
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        MCButtonBody(label: configuration.label, lit: selected || configuration.isPressed,
                     enabled: enabled, height: height)
    }
}

private struct MCButtonBody<L: View>: View {
    @ObservedObject private var theme = MCTheme.shared
    let label: L
    let lit: Bool
    let enabled: Bool
    let height: CGFloat

    var body: some View {
        label
            .frame(maxWidth: .infinity, minHeight: height, maxHeight: height)
            .padding(.horizontal, 12)
            .background { plate }
            .contentShape(Rectangle())
            .hoverEffect(.highlight)
    }

    @ViewBuilder private var plate: some View {
        let src = !enabled ? theme.buttonOff : lit ? theme.buttonHi : theme.button
        if let src, let cg = src.cgImage {
            // 1 texel = height/20 points, so the border keeps its proportions.
            let s = max(1, height / 20)
            Image(uiImage: UIImage(cgImage: cg, scale: 1 / s, orientation: .up))
                .resizable(capInsets: EdgeInsets(top: 3 * s, leading: 3 * s, bottom: 3 * s, trailing: 3 * s),
                           resizingMode: .tile)
                .interpolation(.none)
        } else {
            RoundedRectangle(cornerRadius: 6)
                .fill(Color(white: lit ? 0.42 : 0.28).opacity(enabled ? 1 : 0.45))
                .overlay(RoundedRectangle(cornerRadius: 6).stroke(lit ? Color.white : Color.black.opacity(0.7), lineWidth: 2))
        }
    }
}

/// The title screen's backdrop: the four horizontal panorama faces as one
/// strip drifting left (the game's slow turn, flattened), blurred a little and
/// darkened. Without a panorama: tiled dirt, the old options background.
private struct MCBackdrop: View {
    @ObservedObject private var theme = MCTheme.shared
    @State private var drift = false

    var body: some View {
        GeometryReader { g in
            let h = g.size.height * 1.15
            ZStack {
                Color(red: 0.08, green: 0.07, blue: 0.06)
                if theme.panorama.count == 4 {
                    HStack(spacing: 0) {
                        ForEach(0..<5, id: \.self) { i in
                            Image(uiImage: theme.panorama[i % 4]).resizable().frame(width: h, height: h)
                        }
                    }
                    .frame(width: h * 5, height: h, alignment: .leading)
                    .offset(x: drift ? -h * 4 : 0)
                    .frame(width: g.size.width, height: g.size.height, alignment: .leading)
                    .blur(radius: 3)
                    .onAppear {
                        withAnimation(.linear(duration: 240).repeatForever(autoreverses: false)) { drift = true }
                    }
                } else if let d = theme.dirt, let cg = d.cgImage {
                    Image(uiImage: UIImage(cgImage: cg, scale: 0.25, orientation: .up))
                        .resizable(resizingMode: .tile).interpolation(.none)
                        .colorMultiply(Color(white: 0.3))
                }
                LinearGradient(colors: [.black.opacity(0.0), .black.opacity(0.35)], startPoint: .top, endPoint: .bottom)
            }
            .clipped()
        }
        .ignoresSafeArea()
    }
}

/// A translucent black slab with a thin light edge — the game's list panels.
private struct MCPanel<C: View>: View {
    let title: String
    @ViewBuilder var content: C
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            MCText(title, size: 22, rgb: (255, 255, 160))
            content
        }
        .padding(18)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Color.black.opacity(0.45), in: RoundedRectangle(cornerRadius: 4))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.white.opacity(0.25), lineWidth: 2))
    }
}

// --- build identity (ui68) -------------------------------------------------------------
//
// Which binary a screenshot shows. Read from the bundle Xcode wrote
// (CFBundleVersion = KLEPTON_BUILD_VERSION, CFBundleShortVersionString =
// KLEPTON_MARKETING_VERSION), never a literal, so the label cannot disagree with
// the installed app. The Minecraft version is appended only when launch.txt in
// the container names one (MCTheme reads it with the textures); otherwise the
// label is just the build and the app version. Display only: nothing here is
// read by the launch, the gate, or the boot.

enum MCBuildIdentity {
    static let build: String = info("CFBundleVersion") ?? "?"
    static let version: String = info("CFBundleShortVersionString") ?? "?"
    static let bundleID: String = Bundle.main.bundleIdentifier ?? "?"
    /// What the product calls itself: the bundle's CFBundleDisplayName
    /// ("Minecraft Compiled"), or "Minecraft" when the Info.plist has none —
    /// the same rule BootView.reportTitle applies to the diagnostics heading.
    static let product: String = info("CFBundleDisplayName") ?? "Minecraft"

    /// "Build 68 · v0.4.0"
    static var short: String { "Build \(build) · v\(version)" }

    /// `short`, plus " · Minecraft 26.2" when the instance version is known.
    static func label(minecraft: String?) -> String {
        guard let mc = minecraft?.trimmingCharacters(in: .whitespaces), !mc.isEmpty else { return short }
        return short + " · Minecraft " + mc
    }

    /// One line in the log, the first time a footer is drawn.
    static let logged: Void = {
        NSLog("[build] %@ (%@)", short, bundleID)
    }()

    private static func info(_ key: String) -> String? {
        guard let v = Bundle.main.object(forInfoDictionaryKey: key) as? String,
              !v.trimmingCharacters(in: .whitespaces).isEmpty else { return nil }
        return v
    }
}

/// The small footer line. `id` names the screen for the accessibility audit
/// ("build-identity.<id>"); `light` picks the dark-backdrop colour (launcher)
/// or the secondary colour (the plain report window).
struct MCBuildFooter: View {
    @ObservedObject private var theme = MCTheme.shared
    let id: String
    var light = true

    init(id: String, light: Bool = true) { self.id = id; self.light = light }

    var body: some View {
        let text = MCBuildIdentity.label(minecraft: theme.instanceVersion)
        Text(text)
            .font(.footnote.monospacedDigit())
            .foregroundStyle(light ? Color.white.opacity(0.85) : Color.secondary)
            .lineLimit(1)
            .accessibilityIdentifier("build-identity." + id)
            .accessibilityLabel(text)
            .onAppear { _ = MCBuildIdentity.logged }
    }
}

struct MCLauncherView: View {
    var running: Bool
    var finished: Bool
    var succeeded: Bool
    var status: String
    var onLaunch: () -> Void
    var onDetails: () -> Void

    @ObservedObject private var m = MSAAccounts.shared
    @ObservedObject private var model = MCLauncherModel.shared
    @ObservedObject private var theme = MCTheme.shared
    @ObservedObject private var heads = MCHeads.shared
    @ObservedObject private var progress = MCLaunchProgress.shared
    /// The About sheet (credits, licensing, the way into diagnostics).
    @State private var showAbout = false
    /// A failed launch's "Technical details" disclosure.
    @State private var showFailureDetails = false

    private var locked: Bool { running || finished }
    /// A launch that did not go through: the boot thread ended without a game.
    private var failed: Bool { finished && !succeeded }
    private var accountsReady: Bool {
        !MSAAccounts.wanted || m.selection == MSAStore.offline || m.accounts.contains { $0.id == m.selection }
    }
    private var playReady: Bool { model.play != .server || !model.serverHost.isEmpty }
    private var gateShut: Bool {
        OwnershipGate.blocksLaunch(ready: m.gateReady, unlocked: m.deviceUnlocked)
    }
    private var canLaunch: Bool {
        !locked && accountsReady && model.micAnswered && playReady && !m.addBusy && m.rechecking == nil && !gateShut
    }

    var body: some View {
        ZStack {
            MCBackdrop()
            VStack(spacing: 18) {
                logo
                HStack(alignment: .top, spacing: 20) {
                    MCPanel(title: "Account") { accountSection }
                        .frame(maxWidth: .infinity)
                    MCPanel(title: "Play") { playSection }
                        .frame(width: 470)
                }
                .frame(maxHeight: .infinity)
                launchRow
            }
            .padding(28)
            // About — credits, licensing, diagnostics — out of the play path,
            // in the corner. The report is behind it, not beside Launch.
            Button { showAbout = true } label: { Image(systemName: "info.circle") }
                .buttonStyle(.bordered)
                .accessibilityLabel("About")
                .accessibilityIdentifier("launcher.about")
                .padding(16)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
        }
        .sheet(isPresented: $showAbout) { MCAboutView() }
        .onAppear {
            theme.load()
            model.refresh()
            if running { progress.begin() }   // the view was rebuilt mid-launch
            if failed { MCFailureReport.record(status: status) }
        }
        .onChange(of: running) { _, now in
            if now { progress.begin() }
        }
        .onChange(of: finished) { _, now in
            if now, !succeeded { MCFailureReport.record(status: status) }
        }
        .onChange(of: model.quitting) { _, now in
            if now { progress.end() }
        }
        // Each card has its own worlds (per-account game dirs).
        .onChange(of: m.selection) { model.rescanWorlds() }
        .onChange(of: m.accounts) { model.rescanWorlds() }
        .onAppear {
            // Unstructured and once per process: "details" takes this view
            // away, which would cancel (and "back" restart) a view-owned task.
            guard !model.scriptStarted else { return }
            model.scriptStarted = true
            Task { @MainActor in await runScript() }
        }
    }

    /// KL_LAUNCHER_SCRIPT: the simulator has no pointer for a headless run, so
    /// a scripted run drives the same actions a tap would, `;`-separated:
    /// account=<name|offline>  play=resume|title|server[:host]  eye=4608|4864|5088
    /// details  back  wait=<s>  launch  ended[=<s>] (until the game quits back
    /// here). Unset (always, on device) = nothing.
    private func runScript() async {
        guard let script = MCEnv.value("KL_LAUNCHER_SCRIPT"), !script.isEmpty else { return }
        try? await Task.sleep(for: .seconds(2))
        var endsSeen = 0
        for step in script.split(separator: ";").map({ $0.trimmingCharacters(in: .whitespaces) }) {
            let kv = step.split(separator: "=", maxSplits: 1).map(String.init)
            let arg = kv.count > 1 ? kv[1] : ""
            NSLog("[launcher] script: %@", step)
            switch kv[0] {
            case "account":
                if arg == "offline" { m.selection = MSAStore.offline; m.makeDefault(id: MSAStore.offline) }
                else if let a = m.accounts.first(where: { $0.name.lowercased() == arg.lowercased() }) {
                    m.selection = a.id; m.makeDefault(id: a.id)
                }
            case "play":
                if arg.hasPrefix("server") {
                    model.play = .server
                    if let c = arg.firstIndex(of: ":") { model.server = String(arg[arg.index(after: c)...]) }
                } else if arg == "resume", model.scanned, model.worlds.isEmpty {
                    model.play = .title
                    NSLog("[launcher] script: no saved world, play=title")
                } else if let p = MCLauncherModel.Play(rawValue: arg) { model.play = p }
            case "eye":
                if let n = Int(arg), let r = MCLauncherModel.Resolution(rawValue: n) { model.setResolution(r) }
            case "mode":
                // windowed|immersive — a script chooses the way a person does; it
                // cannot bypass the per-process lock either.
                if let mode = MCPlayMode(rawValue: arg), !model.playModeFixed { model.playMode = mode }
                else { NSLog("[launcher] script: play mode %@ not applied (fixed=%d)", arg, model.playModeFixed ? 1 : 0) }
            case "details": model.details = true
            case "back": model.details = false
            case "wait": try? await Task.sleep(for: .seconds(Double(arg) ?? 1))
            case "ended":
                // Until the running game has quit back to this window (or the
                // timeout, seconds, default 600).
                let deadline = Date().addingTimeInterval(Double(arg) ?? 600)
                while model.gamesEnded <= endsSeen, Date() < deadline {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                endsSeen = model.gamesEnded
                NSLog("[launcher] script: ended -> %d game(s) ended", endsSeen)
            case "launch":
                // A script is not a bypass. If the gate is shut, Launch is
                // taken and refused (the log line), rather than skipped.
                if !OwnershipGate.allowsLaunch() { onLaunch() }
                else if canLaunch { onLaunch() }
                else { NSLog("[launcher] script: Launch is disabled (%@)", footer) }
            default: NSLog("[launcher] script: unknown step %@", step)
            }
            try? await Task.sleep(for: .milliseconds(300))
        }
    }

    // --- logo ---

    @ViewBuilder private var logo: some View {
        if let l = theme.logo {
            ZStack(alignment: .bottom) {
                Image(uiImage: l).resizable().interpolation(.none).aspectRatio(contentMode: .fit)
                    .frame(width: 560)
                if let e = theme.edition {
                    Image(uiImage: e).resizable().interpolation(.none).aspectRatio(contentMode: .fit)
                        .frame(width: 280).offset(y: 6)
                }
            }
            .frame(height: 148)
            .shadow(color: .black.opacity(0.5), radius: 8)
        } else {
            HStack(spacing: 14) {
                Image(systemName: "cube.fill").font(.system(size: 54))
                Text("MINECRAFT").font(.system(size: 64, weight: .black, design: .rounded))
            }
            .foregroundStyle(.white).shadow(color: .black, radius: 0, x: 3, y: 3)
            .frame(height: 120)
        }
    }

    // --- account ---

    @ViewBuilder private var accountSection: some View {
        // KL_MSA=0 hides accounts once the device is unlocked (the launch
        // plays offline). While the gate is shut, Add account stays, because
        // that is how an owning sign-in unlocks the device.
        let showAccounts = MSAAccounts.wanted || gateShut
        if !showAccounts {
            // A developer run (KL_MSA=0); the sentence stays product copy anyway.
            Label("Accounts are turned off for this launch. The game plays offline.", systemImage: "person.slash")
                .foregroundStyle(.white.opacity(0.8))
        } else {
            ScrollView {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 150, maximum: 180), spacing: 14)], spacing: 14) {
                    ForEach(m.accounts, id: \.id) { a in
                        if a.resolved && !a.owns {
                            // No Java profile (none bought, or not set up yet): plays offline under its own name.
                            card(id: a.id, name: m.label(a), subtitle: a.needsJavaProfile ? "Set up Java profile" : "Offline",
                                 head: theme.steve, removable: true, grey: true)
                        } else {
                            card(id: a.id, name: m.label(a), subtitle: a.resolved ? (a.lastError.isEmpty ? "Microsoft" : "Offline if refresh fails") : (a.lastError.isEmpty ? "Signing in…" : a.lastError),
                                 head: heads.head(for: a), removable: true)
                        }
                    }
                    card(id: MSAStore.offline, name: "Offline", subtitle: "No online servers", head: theme.steve, removable: false)
                    addCard
                }
                .padding(4)
            }
            addStatus
            if let a = m.accounts.first(where: { $0.id == m.selection }) { MSAJavaStatusNote(account: a, light: true) }
        }
    }

    private func card(id: String, name: String, subtitle: String, head: UIImage?, removable: Bool,
                      grey: Bool = false) -> some View {
        let selected = m.selection == id
        return Button {
            m.selection = id
            if m.defaultID != id { m.makeDefault(id: id) }
        } label: {
            VStack(spacing: 8) {
                Group {
                    if let head {
                        Image(uiImage: head).resizable().interpolation(.none)
                    } else {
                        Image(systemName: id == MSAStore.offline ? "person.fill" : "person.crop.square.fill")
                            .resizable().scaledToFit().padding(10).foregroundStyle(.white.opacity(0.7))
                            .background(Color(white: 0.25))
                    }
                }
                .frame(width: 72, height: 72)
                .shadow(color: .black.opacity(0.5), radius: 0, x: 3, y: 3)
                Text(name).font(.headline).foregroundStyle(.white).lineLimit(1).minimumScaleFactor(0.6)
                Text(grey ? subtitle : id == m.defaultID ? "Default" : subtitle).font(.caption)
                    .foregroundStyle(grey ? Color(white: 0.6) : id == m.defaultID ? Color(red: 0.55, green: 1, blue: 0.4) : .white.opacity(0.6))
                    .lineLimit(1)
            }
            .padding(12)
            .frame(maxWidth: .infinity, minHeight: 160)
            .background(Color.black.opacity(selected ? 0.6 : 0.35))
            .overlay(Rectangle().stroke(selected ? Color.white : Color.white.opacity(0.15), lineWidth: selected ? 3 : 1))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .hoverEffect(.highlight)
        .disabled(locked)
        .contextMenu {
            if removable {
                Button(role: .destructive) { m.remove(id: id) } label: { Label("Remove account", systemImage: "trash") }
            }
        }
    }

    private var addCard: some View {
        Button { m.startAdd() } label: {
            VStack(spacing: 8) {
                Image(systemName: "plus").font(.system(size: 34, weight: .bold)).foregroundStyle(.white.opacity(0.8))
                    .frame(width: 72, height: 72)
                    .overlay(Rectangle().stroke(style: StrokeStyle(lineWidth: 2, dash: [6])).foregroundStyle(.white.opacity(0.4)))
                Text("Add account").font(.headline).foregroundStyle(.white)
                Text("Microsoft sign-in").font(.caption).foregroundStyle(.white.opacity(0.6))
            }
            .padding(12)
            .frame(maxWidth: .infinity, minHeight: 160)
            .background(Color.black.opacity(0.25))
            .overlay(Rectangle().stroke(Color.white.opacity(0.15), lineWidth: 1))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .hoverEffect(.highlight)
        .disabled(m.addBusy)
    }

    @ViewBuilder private var addStatus: some View {
        switch m.add {
        case .idle:
            if m.resolving { HStack { ProgressView().controlSize(.small); Text("Signing in…") }.foregroundStyle(.white) }
        case .requesting:
            HStack { ProgressView(); Text("Asking Microsoft for a sign-in code…"); Button("Cancel") { m.cancelAdd() } }
                .foregroundStyle(.white)
        case let .code(user, uri, expires):
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 6) {
                    Text("On a phone or computer, open").foregroundStyle(.white.opacity(0.7))
                    if let url = URL(string: uri) { Link(uri, destination: url) } else { Text(uri) }
                    Text("and enter").foregroundStyle(.white.opacity(0.7))
                }
                HStack(spacing: 12) {
                    Text(user).font(.system(.title, design: .monospaced).bold()).foregroundStyle(.white)
                        .textSelection(.enabled)
                    Button { UIPasteboard.general.string = user } label: { Label("Copy", systemImage: "doc.on.doc") }
                    Spacer()
                    ProgressView().controlSize(.small)
                    Text("expires \(expires, style: .relative)").font(.footnote).foregroundStyle(.white.opacity(0.6))
                    Button("Cancel") { m.cancelAdd() }
                }
            }
            .padding(12)
            .background(Color.black.opacity(0.5), in: RoundedRectangle(cornerRadius: 4))
        case .finishing:
            HStack { ProgressView(); Text("Signed in — fetching the Minecraft profile…") }.foregroundStyle(.white)
        case let .done(name):
            HStack {
                Label("Added \(name)", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                Button("OK") { m.cancelAdd() }
            }
        case let .failed(reason):
            HStack {
                Label(reason, systemImage: "xmark.octagon.fill").foregroundStyle(.red).lineLimit(2)
                Button("OK") { m.cancelAdd() }
            }
        }
    }

    // --- play / resolution / mic ---

    private var playSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Button {
                if model.newest == nil { model.play = .title } else { model.play = .resume }
            } label: {
                HStack(spacing: 10) {
                    if let p = model.newest?.iconPath, let img = UIImage(contentsOfFile: p) {
                        Image(uiImage: img).resizable().interpolation(.none).frame(width: 36, height: 36)
                    }
                    VStack(alignment: .leading, spacing: 2) {
                        MCText(model.newest.map { "Continue \($0.name)" } ?? "Create world", size: 18)
                        if let w = model.newest {
                            Text("Last played \(w.lastPlayed, style: .relative) ago").font(.caption2)
                                .foregroundStyle(.white.opacity(0.75))
                        } else {
                            Text("No saves yet. Opens the title screen.").font(.caption2)
                                .foregroundStyle(.white.opacity(0.75))
                        }
                    }
                    Spacer(minLength: 0)
                }
            }
            .buttonStyle(MCButtonStyle(selected: model.newest == nil ? model.play == .title : model.play == .resume, height: 60))
            .disabled(locked)

            HStack(spacing: 12) {
                Button { model.play = .title } label: { MCText("Title screen", size: 18) }
                    .buttonStyle(MCButtonStyle(selected: model.play == .title, height: 48))
                Button { model.play = .server } label: { MCText("Join server", size: 18) }
                    .buttonStyle(MCButtonStyle(selected: model.play == .server, height: 48))
            }
            .disabled(locked)

            if model.play == .server {
                TextField("Server address (host or host:port)", text: $model.server)
                    .textInputAutocapitalization(.never).autocorrectionDisabled()
                    .keyboardType(.URL)
                    .textFieldStyle(.roundedBorder)
                    .disabled(locked)
            }

            Divider().overlay(Color.white.opacity(0.2))
            playModeRow

            if model.playMode == .immersive {
                Divider().overlay(Color.white.opacity(0.2))
                MCText("Resolution", size: 18, rgb: (255, 255, 160))
                HStack(spacing: 8) {
                    ForEach(MCLauncherModel.Resolution.allCases, id: \.self) { r in
                        Button { model.setResolution(r) } label: {
                            VStack(spacing: 0) {
                                MCText(r.label, size: 16)
                                Text("\(r.rawValue)").font(.caption2.monospacedDigit()).foregroundStyle(.white.opacity(0.75))
                            }
                        }
                        .buttonStyle(MCButtonStyle(selected: model.resolution == r, height: 50))
                    }
                }
                .disabled(locked)
                // A custom value comes from a hand-edited settings file (a dev run);
                // the player just needs to know one is set and that a tap replaces it.
                Text(model.customEye.map { "A custom resolution of \($0) pixels per eye is set. Picking one here replaces it." }
                     ?? "Pixels per eye. Sharp is the tested default; Max may judder.")
                    .font(.caption).foregroundStyle(.white.opacity(0.7))
            }

            Divider().overlay(Color.white.opacity(0.2))
            micRow
        }
    }

    /// Windowed / Immersive VR, chosen BEFORE Play (build 71). Windowed is the
    /// default and stays the default until the player picks VR; once a game has
    /// booted the process is committed to its mode and the row says how to
    /// change it. The per-eye Resolution row applies to VR only, so it follows.
    private var playModeRow: some View {
        VStack(alignment: .leading, spacing: 8) {
            MCText("Play mode", size: 18, rgb: (255, 255, 160))
            HStack(spacing: 8) {
                ForEach(MCPlayMode.allCases, id: \.self) { mode in
                    Button { model.playMode = mode } label: {
                        VStack(spacing: 0) {
                            MCText(mode.title, size: 16)
                            Text(mode.subtitle).font(.caption2).foregroundStyle(.white.opacity(0.75))
                        }
                    }
                    .buttonStyle(MCButtonStyle(selected: model.playMode == mode, height: 50))
                    .accessibilityIdentifier("launcher.playmode." + mode.rawValue)
                }
            }
            .disabled(locked || model.playModeFixed)
            Text(model.playModeFixed
                 ? "This session is set to \(model.playMode.title.lowercased()). Close and reopen the app to change the play mode."
                 : model.playMode.caption + " " + MCPlayMode.originSentence)
                .font(.caption).foregroundStyle(.white.opacity(0.7))
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("launcher.playmode.caption")
        }
    }

    private var micRow: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: model.mic == .granted ? "mic.fill" : model.mic == .denied ? "mic.slash.fill" : "mic")
                .font(.title3)
                .foregroundStyle(model.mic == .granted ? .green : model.mic == .denied ? .orange : .white)
                .frame(width: 26)
            VStack(alignment: .leading, spacing: 2) {
                switch model.mic {
                case .granted: Text("Microphone on").foregroundStyle(.white)
                case .denied: Text("Microphone off — allow it in Settings › Privacy › Microphone").foregroundStyle(.white)
                case .undeclared: Text("Voice chat is not built into this app").foregroundStyle(.white)
                case .undetermined, .asking: Text("Answer the microphone prompt to continue").foregroundStyle(.white)
                }
                Text("Voice chat push-to-talk: left thumbstick click").font(.caption).foregroundStyle(.white.opacity(0.7))
            }
            .font(.callout)
            Spacer(minLength: 0)
            if model.mic == .undetermined { Button("Ask") { model.checkMic() } }
        }
    }

    // --- launch ---

    private var launchRow: some View {
        VStack(spacing: 12) {
            if OwnershipGate.enforced && m.gateReady && !m.deviceUnlocked {
                Text(OwnershipGate.lockedSentence(for: m.accounts))
                    .font(.body)
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity)
            }
            if failed { failureCard }
            else if let why = refusal { refusalNote(why) }
            HStack(alignment: .center, spacing: 18) {
                // Left: what the launch is doing (compact), or nothing when idle.
                progressStrip
                    .frame(width: 300, alignment: .leading)
                Spacer()
                // The labelled Play action: it names where the game will appear
                // (build 71), so immersive is never a surprise.
                Button { onLaunch() } label: {
                    HStack(spacing: 12) {
                        if running || model.quitting { ProgressView().tint(.white) }
                        MCText(model.quitting ? "Quitting…" : running ? "Launching…"
                               : finished ? (succeeded ? "Playing" : "Stopped") : model.playMode.playButtonTitle, size: 28)
                    }
                }
                .buttonStyle(MCButtonStyle(height: 72))
                .frame(width: 440)
                .disabled(!canLaunch)
                .accessibilityIdentifier("launcher.launch")
                .accessibilityLabel(model.playMode.playButtonTitle)
                Spacer()
                Text(footer).font(.footnote).foregroundStyle(.white.opacity(0.8))
                    .multilineTextAlignment(.trailing)
                    .frame(width: 300, alignment: .trailing)
            }
            // Build identity (ui68): under the Launch row on every launcher state —
            // idle, Launching…, Playing, Quitting… — so any screenshot of this
            // window names the build. Display only.
            MCBuildFooter(id: "launcher")
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    /// Launching: a thin indeterminate bar, the stage sentence, the elapsed
    /// time. Playing: one line saying so. Idle, stopped, quitting: nothing —
    /// the Launch button already says the word.
    @ViewBuilder private var progressStrip: some View {
        if running {
            VStack(alignment: .leading, spacing: 6) {
                ProgressView().progressViewStyle(.linear).tint(.white)
                HStack(spacing: 8) {
                    Text(progress.stage).font(.callout).foregroundStyle(.white).lineLimit(1)
                        .accessibilityIdentifier("launcher.progress")
                    Spacer(minLength: 0)
                    if let t = progress.started {
                        Text(t, style: .timer).font(.callout.monospacedDigit()).foregroundStyle(.white.opacity(0.75))
                    }
                }
            }
        } else if finished, succeeded, !model.quitting {
            Label(model.playMode.playingSentence, systemImage: "play.circle.fill")
                .font(.callout).foregroundStyle(.white)
                .fixedSize(horizontal: false, vertical: true)
        } else {
            Color.clear.frame(height: 1)
        }
    }

    /// A launch that stopped before the game ran. The plain sentence and a
    /// hint by family first; a short code behind one tap (never the raw
    /// status, never a framework report); Export shares the launch report
    /// (which holds the status verbatim) and the runtime log. BootView does
    /// not re-arm a finished boot in the same process, so the honest next step
    /// is a relaunch of the app.
    private var failureCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("Minecraft couldn't start.", systemImage: "exclamationmark.triangle.fill")
                .font(.headline).foregroundStyle(.white)
            Text(MCFailureReport.hint(for: status))
                .font(.callout).foregroundStyle(.white.opacity(0.85))
                .fixedSize(horizontal: false, vertical: true)
            DisclosureGroup(isExpanded: $showFailureDetails) {
                HStack(spacing: 10) {
                    Text(MCFailureReport.code(for: status))
                        .font(.system(.callout, design: .monospaced).bold())
                        .foregroundStyle(.white)
                        .textSelection(.enabled)
                        .accessibilityIdentifier("launcher.failure.code")
                    Text("Quote this code if you ask for help. The exported report has the full detail.")
                        .font(.caption).foregroundStyle(.white.opacity(0.75))
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(.top, 4)
            } label: {
                Text("Technical code").font(.callout).foregroundStyle(.white)
            }
            .tint(.white)
            let items = MCFailureReport.exportItems
            if !items.isEmpty {
                ShareLink(items: items) { Label("Export diagnostics", systemImage: "square.and.arrow.up") }
                    .buttonStyle(.bordered)
                    .tint(.white)
                    .accessibilityIdentifier("launcher.failure.export")
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.orange.opacity(0.8), lineWidth: 2))
        .accessibilityIdentifier("launcher.failure")
    }

    /// A Launch after Quit Game that was refused (kl_app_native_rerun: the
    /// previous run has not ended, or no re-run in this process). BootView
    /// leaves the launcher idle with the reason in `status`; it was only in the
    /// log before. Shown until the next state change, in plain words.
    private var refusal: String? {
        guard !running, !finished, model.gamesEnded > 0, !model.quitting else { return nil }
        let s = status.trimmingCharacters(in: .whitespaces)
        guard !s.isEmpty, s != "idle", !s.hasPrefix("game ended") else { return nil }
        if s.contains("has not ended") { return "The previous game is still closing. Try again in a moment." }
        if s.contains("no re-run") { return "This app can run one game per session. Close and reopen the app to play again." }
        return "Launch didn't go through. Close and reopen the app to try again."
    }

    private func refusalNote(_ why: String) -> some View {
        Label(why, systemImage: "info.circle")
            .font(.callout).foregroundStyle(.white)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityIdentifier("launcher.refusal")
    }

    private var footer: String {
        if failed { return "Stopped" }
        if locked { return "as \(m.selectionLabel)" }
        if !model.micAnswered { return "Waiting for the microphone prompt" }
        if !playReady { return "Type a server address" }
        if m.addBusy { return "Finish adding the account" }
        if m.rechecking != nil { return "Checking the account…" }
        return MSAAccounts.wanted ? "as \(m.selectionLabel)" : "offline"
    }
}

// --- About: credits, licensing, the diagnostics export (ui70) ----------------------------
//
// The upstream runtime is credited HERE rather than on the product screen,
// and as ONE item of the Made-with list rather than a heading: the heading is
// the product's name (MCBuildIdentity.product). Everything stated is what the
// source says this build is made of; a bundled CREDITS/LICENSES text, when the
// bundle carries one, is shown verbatim beneath it so licensing can be
// completed without a code change (MCCredits.bundledText). There is no way
// from here into the runtime's report window; the diagnostics are files the
// player can export.

enum MCCredits {
    /// The components, as the host names them (kl_app.c, KleptonMCProfiles.swift).
    static let components: [(name: String, role: String)] = [
        ("Minecraft: Java Edition", "the game, by Mojang Studios — owned by the signed-in Microsoft account"),
        ("Vivecraft", "the VR mod the game runs with"),
        ("Fabric Loader", "the mod loader"),
        ("GraalVM Native Image", "the ahead-of-time compilation of the game"),
        ("ANGLE", "OpenGL ES on Metal"),
        ("Klepton", "the visionOS runtime: OpenXR, graphics, audio, input and the launcher"),
    ]

    static let notice = "Minecraft is a trademark of Microsoft/Mojang. This app is not affiliated with or endorsed by Microsoft, Mojang Studios or the Vivecraft project. Playing requires a Microsoft account that owns Minecraft: Java Edition."

    /// `CREDITS.txt` or `LICENSES.txt` at the bundle root, if the build ships one.
    static var bundledText: String? {
        for name in ["LICENSES", "CREDITS", "Credits", "Licenses"] {
            if let p = Bundle.main.path(forResource: name, ofType: "txt"),
               let t = try? String(contentsOfFile: p, encoding: .utf8),
               !t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return t }
        }
        return nil
    }
}

struct MCAboutView: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(MCBuildIdentity.product).font(.largeTitle.bold())
                        MCBuildFooter(id: "about", light: false)
                    }

                    VStack(alignment: .leading, spacing: 8) {
                        Text("Made with").font(.headline)
                        ForEach(MCCredits.components, id: \.name) { c in
                            HStack(alignment: .firstTextBaseline, spacing: 8) {
                                Text(c.name).bold().frame(width: 220, alignment: .leading)
                                Text(c.role).foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                            .font(.callout)
                        }
                    }

                    Text(MCCredits.notice).font(.footnote).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)

                    if let text = MCCredits.bundledText {
                        Divider()
                        Text("Licenses").font(.headline)
                        Text(text)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }

                    let items = MCFailureReport.exportItems
                    if !items.isEmpty {
                        Divider()
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Diagnostics").font(.headline)
                            Text("The diagnostics files are stored only on this device. They name files, worlds and account names, so share them only with someone you trust.")
                                .font(.callout).foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                            ShareLink(items: items) { Label("Export diagnostics", systemImage: "square.and.arrow.up") }
                                .accessibilityIdentifier("about.export")
                        }
                    }
                }
                .padding(24)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .navigationTitle("About")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        .frame(minWidth: 640, minHeight: 520)
    }
}
