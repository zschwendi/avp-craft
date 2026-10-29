import Foundation

// The game payload inside the .app (visionos/bundle_payload.sh).
//
// A SideStore install starts with an empty data container. devicectl is not
// there to run stage_native.sh. On first launch this copies what the game
// writes (options, config, the other small fgamedir files) into
// Documents/<tree>/ and points the read-only trees (jars, assets, mods,
// .fabric, resourcepacks) at klepton-payload/ in the bundle. Nothing in that
// 600 MB is copied. The bundle step strips saves and account files, so a
// fresh install has no world.
//
// A container that already has a real staged tree (Austin's headset, filled
// by stage_native.sh) is not touched. The marker records that:
//
//   Documents/<tree>/.payload-version
//     bundle\n<hash>     we created this container from the IPA
//     external\n<hash>   a staged container was already here; leave it
//
// A later build whose hash differs retargets the symlinks (the bundle path
// changes on update) and refreshes launch.txt and baked.cp. It does not
// replace options, config, saves, or profiles. A symlink is replaced only
// when it is a symlink. A real file or directory is left in place, so a
// devicectl stage that landed on top of a bundled container is not undone.
//
// No payload directory in the bundle: this does nothing, and the app still
// requires the staged container. That is the simulator edit loop
// (KL_BUNDLE_PAYLOAD is off there unless set).

enum KleptonPayload {
    static let payloadName = "klepton-payload"
    static let markerName = ".payload-version"
    /// Read-only children of the game dir. Profiles already symlink these.
    static let sharedGameDir = ["mods", ".fabric", "resourcepacks"]

    /// A bundled install may inherit a world that is actually in the payload.
    /// The bundle step ships none, so this is empty and Continue falls back
    /// to the title screen. A devicectl container (marker external, or no
    /// marker) does not inherit: that world belongs to the owning account.
    nonisolated static var sharesSeedWorld: Bool {
        guard let tree = bundledTree() else { return false }
        return markerKind(at: documents() + "/" + tree + "/" + markerName) == "bundle"
    }

    static func materialize() {
        do { try materializeThrowing() }
        catch {
            NSLog("[payload] FAILED: %@", error.localizedDescription)
        }
    }

    private static func materializeThrowing() throws {
        guard let payload = bundledPayload() else { return }
        let version = try String(contentsOfFile: payload + "/payload-version", encoding: .utf8)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !version.isEmpty else { return }
        let tree = (try? String(contentsOfFile: payload + "/tree", encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let treeName = (tree?.isEmpty == false) ? tree! : "vivecraft"
        let dest = documents() + "/" + treeName
        let marker = dest + "/" + markerName
        let fm = FileManager.default

        if markerKind(at: marker) == "external" {
            NSLog("[payload] staged container left as it is (%@)", treeName)
            return
        }
        if !fm.fileExists(atPath: marker), isLegacyStage(dest) {
            try writeMarker("external", version, at: marker)
            NSLog("[payload] existing staged container kept; worlds and options not touched")
            return
        }

        let gameRel = try gameDirRelative(payload + "/launch.txt")
        let bundleRoot = payload + "/root"
        let destRoot = dest + "/root"
        let current = markerKind(at: marker) == "bundle"
            && markerVersion(at: marker) == version
            && linksPoint(at: destRoot + "/" + assetsRelative(payload + "/launch.txt", gameRel: gameRel),
                          into: payload)
        if current {
            NSLog("[payload] %@ already materialized (%@)", treeName, String(version.prefix(12)))
            return
        }

        try fm.createDirectory(atPath: dest, withIntermediateDirectories: true)
        try replaceFile(payload + "/launch.txt", dest + "/launch.txt")
        try replaceFile(payload + "/baked.cp", dest + "/baked.cp")
        let counts = try materializeRoot(bundleRoot: bundleRoot, destRoot: destRoot, gameRel: gameRel)
        try writeMarker("bundle", version, at: marker)
        let line = "[payload] materialized \(treeName) \(version.prefix(12)) "
            + "linked \(counts.linked) copied \(counts.copied) kept \(counts.kept)"
        NSLog("%@", line)
        try? Data((line + "\n").utf8).write(to: URL(fileURLWithPath: dest + "/payload.log"), options: .atomic)
    }

    // --- the tree ----------------------------------------------------------

    private struct Counts { var linked = 0; var copied = 0; var kept = 0 }

    /// `gameRel` is launch.txt's --gameDir with @ROOT@/ removed
    /// (`inst-26.2/fgamedir`, or `fgamedir` on 1.21.5). Every other directory
    /// next to the game dir is jars or assets and becomes a symlink.
    private static func materializeRoot(bundleRoot: String, destRoot: String, gameRel: String) throws -> Counts {
        let fm = FileManager.default
        try fm.createDirectory(atPath: destRoot, withIntermediateDirectories: true)
        let parent = (gameRel as NSString).deletingLastPathComponent
        let leaf = (gameRel as NSString).lastPathComponent
        var counts = Counts()
        let scanBundle: String
        let scanDest: String
        if parent.isEmpty {
            scanBundle = bundleRoot
            scanDest = destRoot
        } else {
            let top = (parent as NSString).pathComponents[0]
            for kid in try kids(bundleRoot) where kid != top {
                note(try ensureLink(from: bundleRoot + "/" + kid, to: destRoot + "/" + kid), &counts)
            }
            scanBundle = bundleRoot + "/" + parent
            scanDest = destRoot + "/" + parent
            try fm.createDirectory(atPath: scanDest, withIntermediateDirectories: true)
        }
        for kid in try kids(scanBundle) {
            let from = scanBundle + "/" + kid
            let to = scanDest + "/" + kid
            if kid == leaf {
                try materializeGameDir(from: from, to: to, counts: &counts)
            } else {
                note(try ensureLink(from: from, to: to), &counts)
            }
        }
        return counts
    }

    private static func materializeGameDir(from src: String, to dest: String, counts: inout Counts) throws {
        let fm = FileManager.default
        try fm.createDirectory(atPath: dest, withIntermediateDirectories: true)
        for kid in try kids(src) {
            let s = src + "/" + kid
            let d = dest + "/" + kid
            if sharedGameDir.contains(kid) {
                note(try ensureLink(from: s, to: d), &counts)
            } else if isReal(d) || isLink(d) {
                counts.kept += 1
            } else {
                try fm.copyItem(atPath: s, toPath: d)
                counts.copied += 1
            }
        }
    }

    private enum Link { case linked, kept }

    /// A symlink is created or retargeted. A real file or directory stays.
    private static func ensureLink(from src: String, to dest: String) throws -> Link {
        let fm = FileManager.default
        if isLink(dest) {
            let cur = try fm.destinationOfSymbolicLink(atPath: dest)
            if cur == src { return .kept }
            try fm.removeItem(atPath: dest)
        } else if isReal(dest) {
            return .kept
        }
        try fm.createDirectory(atPath: (dest as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        try fm.createSymbolicLink(atPath: dest, withDestinationPath: src)
        return .linked
    }

    private static func note(_ link: Link, _ counts: inout Counts) {
        if link == .linked { counts.linked += 1 } else { counts.kept += 1 }
    }

    private static func replaceFile(_ src: String, _ dest: String) throws {
        let fm = FileManager.default
        if fm.fileExists(atPath: dest) { try fm.removeItem(atPath: dest) }
        try fm.copyItem(atPath: src, toPath: dest)
    }

    // --- what is already in the container ---------------------------------

    /// launch.txt plus a real (not symlinked) game dir: stage_native.sh has
    /// been here. Our own containers symlink mods, so they are not this.
    private static func isLegacyStage(_ dest: String) -> Bool {
        let fm = FileManager.default
        guard fm.fileExists(atPath: dest + "/launch.txt") else { return false }
        guard let rel = try? gameDirRelative(dest + "/launch.txt") else { return false }
        let mods = dest + "/root/" + rel + "/mods"
        return isReal(mods)
    }

    private static func gameDirRelative(_ launchPath: String) throws -> String {
        let text = try String(contentsOfFile: launchPath, encoding: .utf8)
        if let rel = launchValue("--gameDir", text), !rel.isEmpty { return rel }
        return "fgamedir"
    }

    private static func assetsRelative(_ launchPath: String, gameRel: String) -> String {
        if let text = try? String(contentsOfFile: launchPath, encoding: .utf8),
           let rel = launchValue("--assetsDir", text), !rel.isEmpty { return rel }
        let parent = (gameRel as NSString).deletingLastPathComponent
        return parent.isEmpty ? "assets" : parent + "/assets"
    }

    /// The path after @ROOT@/, or nil when the line is not a root placeholder.
    private static func launchValue(_ opt: String, _ text: String) -> String? {
        var prev = ""
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            if prev == opt {
                let prefix = "@ROOT@/"
                if line.hasPrefix(prefix) { return String(line.dropFirst(prefix.count)) }
                if line == "@ROOT@" { return "" }
                return nil
            }
            prev = line
        }
        return nil
    }

    private static func linksPoint(at path: String, into payload: String) -> Bool {
        guard let dest = try? FileManager.default.destinationOfSymbolicLink(atPath: path) else { return false }
        return dest.hasPrefix(payload + "/") || dest == payload
    }

    // --- marker ------------------------------------------------------------

    private static func writeMarker(_ kind: String, _ version: String, at path: String) throws {
        let dir = (path as NSString).deletingLastPathComponent
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        try Data("\(kind)\n\(version)\n".utf8).write(to: URL(fileURLWithPath: path), options: .atomic)
    }

    private static func markerKind(at path: String) -> String? {
        guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
        let line = text.split(whereSeparator: \.isNewline).first.map(String.init) ?? ""
        return (line == "bundle" || line == "external") ? line : nil
    }

    private static func markerVersion(at path: String) -> String? {
        guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
        let lines = text.split(whereSeparator: \.isNewline).map(String.init)
        return lines.count >= 2 ? lines[1] : nil
    }

    // --- files -------------------------------------------------------------

    /// Hidden names count: `.fabric` is a real directory, and the atPath
    /// listing has dropped dot-directories on some OS versions.
    private static func kids(_ dir: String) throws -> [String] {
        let urls = try FileManager.default.contentsOfDirectory(
            at: URL(fileURLWithPath: dir, isDirectory: true),
            includingPropertiesForKeys: nil,
            options: [])
        return urls.map(\.lastPathComponent).filter { $0 != ".DS_Store" }.sorted()
    }

    private static func isLink(_ path: String) -> Bool {
        (try? URL(fileURLWithPath: path).resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) == true
    }

    /// Exists and is not a symlink. attributesOfItem follows links, so a
    /// symlink is excluded by the check above rather than by the attributes.
    private static func isReal(_ path: String) -> Bool {
        !isLink(path) && FileManager.default.fileExists(atPath: path)
    }

    private static func documents() -> String {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
    }

    private static func bundledPayload() -> String? {
        let path = Bundle.main.bundlePath + "/" + payloadName
        return FileManager.default.fileExists(atPath: path + "/payload-version") ? path : nil
    }

    private static func bundledTree() -> String? {
        guard let payload = bundledPayload() else { return nil }
        let tree = (try? String(contentsOfFile: payload + "/tree", encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return (tree?.isEmpty == false) ? tree : "vivecraft"
    }
}
