import Foundation
import CryptoKit
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

// Microsoft/Mojang accounts for the native Minecraft guest (the vivecraft row).
//
// The game needs a real session to join an online-mode server or Realms:
// `--username/--uuid/--accessToken` of a Minecraft account, `--userType msa`.
// The access token lives 24 h; what persists is each account's Microsoft
// refresh token (90 days, sliding, rotated on every use). At boot,
// kl_msa_session (below, called by kl_app.c native_msa_args before klmc_main)
// picks ONE account and runs the launcher chain for it:
//
//   MSA refresh  (login.microsoftonline.com/consumers, the QCXR client id)
//   -> XBL       (user.auth.xboxlive.com, RPS ticket "d=<msa token>")
//   -> XSTS      (relying party rp://api.minecraftservices.com/)
//   -> Minecraft (/authentication/login_with_xbox -> 24 h access token)
//   -> profile   (/minecraft/profile -> name + uuid)
//
// A profile 404 means "no Java profile", not "no Java Edition": a paid Java
// Edition whose profile (player name) was never set up answers the same. So a
// 404 asks /entitlements/mcstore (same token, read-only): Java Edition listed
// = needsProfile (set it up at minecraft.net/msaprofile, then Check again);
// a list without it = notEntitled; no clear answer (network, 403, 429, 5xx,
// no list) = a service error that keeps whatever was stored before. Neither
// 404 outcome is ownership proof; only the profile 200 is.
//
// FREE accounts (profile 404, either outcome) play OFFLINE under their own
// name, MultiMC-style: a second XSTS exchange (relying party
// http://xboxlive.com, same XBL token) reads the gamertag (xui[0].gtg) and xid;
// the account is stored with ownsGame=false, name = the gamertag made a valid
// player name (playerName), uuid = the offline uuid every offline server
// derives from that name (offlineUUID). kl_app.c then sets only --username and
// --uuid (launch.txt's --accessToken 0 / offline userType stay). A failed
// refresh never blocks a free account (it plays with the stored name); a
// refresh that finds the profile (bought, or set up) upgrades the record. Only
// a notEntitled result is cached for the day; a needsProfile record, and a
// free record from before the entitlements were read, are asked again when
// the window opens and at boot.
//
// Accounts, PrismLauncher-style:
//   Library/klepton-msa/<tree>/accounts.json  {"default": <id>|"offline",
//       "accounts": [{id, name, uuid, xuid, refreshToken, accessToken,
//       expiresOn, added, lastError}]}, mode 0600. Never in Documents:
//       UIFileSharingEnabled exposes that to Files/Finder.
//   Documents/<tree>/msa-refresh.json  the INBOX stage_native.sh writes (the
//       Mac's tools/vivecraft_msa_login.py token). It ADDS an account, which
//       folds into an existing one once its profile resolves to the same uuid.
//   secrets.json/session.json (the first, single-account layout) migrate into
//       accounts.json on first run, become the default, and are deleted.
//
// Which account boots: KL_MSA_ACCOUNT=<name|uuid|offline> (env file) > the
// window's picker > the default. The flat window shows the picker, "Add
// account…" (Microsoft device-code sign-in), "Make default", "Remove", and a
// short countdown before the auto-boot that any touch on the panel holds.
//
// No token is ever logged or shown. KL_MSA=0 skips the session (kl_app.c) and
// does NOT skip the ownership gate. The gate is separate: the game will not
// start until an account that owns Minecraft: Java Edition has signed in on
// this device at least once. Proof is /minecraft/profile returning 200, which
// is what sets ownsGame (nil on a resolved legacy account means the same
// thing — free accounts did not exist yet). isMinecraft is NOT that signal;
// it only means this build is the vivecraft row. The unlock flag lives in
// accounts.json (`deviceUnlocked`) and is sticky: removing the owning account
// does not re-lock, and adding a free account does not clear it. Free
// accounts and Offline work only after that flag is set. The check that
// cannot be skipped is kl_ownership_allows_launch, called from kl_app.c
// immediately before the guest's main. Nothing in the environment turns it
// off. A Debug build can compile it out with the Swift condition
// KL_OWNERSHIP_GATE_OFF; a Release build that defines that condition does
// not compile.

private let msaClientID = "d17a73a2-707c-40f5-8c90-d3eda0956f10"
private let msaAuthority = "https://login.microsoftonline.com/consumers/oauth2/v2.0"
private let msaScopes = "XboxLive.SignIn XboxLive.offline_access"
private let msaUserAgent = "QCXR/6.0.0"
private let mcEntitlementsURL = "https://api.minecraftservices.com/entitlements/mcstore"

private struct MSAFail: Error { let reason: String }

private struct MCSession {
    var accessToken: String
    var expiresOn: Double      // ms since 1970
    var username: String
    var uuid: String
    var xuid: String
    var skin: String? = nil    // the ACTIVE skin's texture URL (launcher avatar)
    var owns = true            // false = free account: offline name + offline uuid, no token
    var gamertag: String? = nil
    var needsProfile = false   // free, but the entitlements list Java Edition (profile not set up)
    var secondsLeft: Double { expiresOn / 1000 - Date().timeIntervalSince1970 }
}

struct MSAAccount: Codable, Equatable {
    var id: String
    var name: String = ""          // "" until the profile resolves
    var uuid: String = ""
    var xuid: String = ""
    var refreshToken: String
    var accessToken: String = ""
    var expiresOn: Double = 0      // ms since 1970
    var added: Double = Date().timeIntervalSince1970
    var lastError: String = ""
    /// The ACTIVE skin's texture URL from /minecraft/profile (the launcher's
    /// player-head avatar). Optional so accounts.json files written before it
    /// existed still decode.
    var skin: String?
    /// nil = an account stored before free accounts existed (those all resolved
    /// through the profile, i.e. own the game). false = free: plays offline.
    var ownsGame: Bool?
    /// The Xbox gamertag as Microsoft spells it (free accounts; `name` is the
    /// player-name-safe form of it).
    var gamertag: String?
    /// The per-account game dir's name under <instance>/profiles/ (see
    /// MCProfiles). Assigned once, the first time the account launches, and
    /// never changed — not even when a free account buys the game.
    var profileKey: String?
    /// What the last decisive check found: an OwnershipGateLogic.JavaCheck
    /// raw value (owner, needsProfile, notEntitled). A string, not the enum,
    /// so a value this build does not know still decodes. nil = the
    /// entitlements were never asked (an owner from before, or a free record
    /// judged by the profile 404 alone, which is asked again).
    var javaCheck: String?
    var resolved: Bool { !uuid.isEmpty && !name.isEmpty }
    var owns: Bool { ownsGame ?? true }
    /// Java Edition is on the account, its Java profile is not set up yet.
    var needsJavaProfile: Bool { !owns && javaCheck == OwnershipGateLogic.JavaCheck.needsProfile.rawValue }
}

private struct MSAFile: Codable {
    var defaultID: String?
    var accounts: [MSAAccount] = []
    /// Sticky. Set the first time a resolved owning account is in this file
    /// (including one stored before this field existed). Never written back
    /// to false: Remove does not re-lock the device.
    var deviceUnlocked: Bool = false
    enum CodingKeys: String, CodingKey { case defaultID = "default", accounts, deviceUnlocked }

    init() {}

    // Hand-written so a file from before deviceUnlocked existed still decodes.
    // A synthesized decode would fail the whole file on the missing key, and
    // edit() would then replace it with an empty store.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        defaultID = try c.decodeIfPresent(String.self, forKey: .defaultID)
        accounts = try c.decodeIfPresent([MSAAccount].self, forKey: .accounts) ?? []
        deviceUnlocked = try c.decodeIfPresent(Bool.self, forKey: .deviceUnlocked) ?? false
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encodeIfPresent(defaultID, forKey: .defaultID)
        try c.encode(accounts, forKey: .accounts)
        try c.encode(deviceUnlocked, forKey: .deviceUnlocked)
    }
}

final class MSABox<T>: @unchecked Sendable { var v: T; init(_ v: T) { self.v = v } }

// --- HTTP -------------------------------------------------------------------

private let http: URLSession = {
    let c = URLSessionConfiguration.ephemeral
    c.timeoutIntervalForRequest = 10
    c.timeoutIntervalForResource = 20
    c.httpAdditionalHeaders = ["User-Agent": msaUserAgent, "Accept": "application/json"]
    return URLSession(configuration: c)
}()

/// One synchronous request. Callers are background threads, never main.
private func call(_ step: String, _ url: String, form: [String: String]? = nil,
                  json: [String: Any]? = nil, bearer: String? = nil) throws -> (Int, [String: Any]) {
    var req = URLRequest(url: URL(string: url)!)
    if let form {
        var allowed = CharacterSet.alphanumerics; allowed.insert(charactersIn: "-._~")
        let body = form.map { k, v in
            "\(k)=\(v.addingPercentEncoding(withAllowedCharacters: allowed) ?? "")"
        }.joined(separator: "&")
        req.httpMethod = "POST"
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        req.httpBody = Data(body.utf8)
    } else if let json {
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try JSONSerialization.data(withJSONObject: json)
    }
    if let bearer { req.setValue("Bearer " + bearer, forHTTPHeaderField: "Authorization") }
    let done = DispatchSemaphore(value: 0)
    let out = MSABox<(Int, [String: Any], Error?)>((0, [:], nil))
    http.dataTask(with: req) { data, resp, err in
        var body: [String: Any] = [:]
        if let data, let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] { body = obj }
        out.v = ((resp as? HTTPURLResponse)?.statusCode ?? 0, body, err)
        done.signal()
    }.resume()
    done.wait()
    let (status, body, netErr) = out.v
    if let e = netErr as? URLError { throw MSAFail(reason: "\(step): network (\(e.code.rawValue))") }
    if netErr != nil { throw MSAFail(reason: "\(step): network error") }
    if status == 429 { throw MSAFail(reason: "\(step): rate limited (HTTP 429)") }
    return (status, body)
}

// --- the chain ----------------------------------------------------------------

private func dashed(_ raw: String) -> String {
    let h = raw.replacingOccurrences(of: "-", with: "").lowercased()
    guard h.count == 32 else { return raw }
    let c = Array(h)
    return [0..<8, 8..<12, 12..<16, 16..<20, 20..<32].map { String(c[$0]) }.joined(separator: "-")
}

/// The Minecraft token is a JWT whose payload carries the Xbox user id.
private func xuidClaim(_ jwt: String) -> String {
    let parts = jwt.split(separator: ".")
    guard parts.count == 3 else { return "" }
    var p = parts[1].replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
    while p.count % 4 != 0 { p += "=" }
    guard let d = Data(base64Encoded: p),
          let obj = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] else { return "" }
    if let s = obj["xuid"] as? String { return s }
    if let n = obj["xuid"] as? NSNumber { return n.stringValue }
    return ""
}

/// The uuid Minecraft gives an offline player (and every offline-mode server
/// derives): Java's UUID.nameUUIDFromBytes(("OfflinePlayer:" + name) UTF-8) —
/// MD5, then version 3 and the IETF variant. Verified against Java on the Mac
/// (work/launcher/ACCOUNTS-PROFILES-REPORT.md).
func offlineUUID(_ name: String) -> String {
    var b = Array(Insecure.MD5.hash(data: Data(("OfflinePlayer:" + name).utf8)))
    b[6] = (b[6] & 0x0f) | 0x30
    b[8] = (b[8] & 0x3f) | 0x80
    return dashed(b.map { String(format: "%02x", $0) }.joined())
}

/// A gamertag as a Minecraft player name: the game's login refuses names over
/// 16 chars or with spaces/control chars (StringUtil.isValidPlayerName), and
/// offline servers commonly allow only [A-Za-z0-9_]. Spaces become '_', other
/// characters are dropped, 16 max; nothing left = "Player".
func playerName(_ gamertag: String) -> String {
    var out = ""
    for ch in gamertag.unicodeScalars {
        if ch == " " { out.append("_") }
        else if ch.isASCII, CharacterSet.alphanumerics.contains(ch) || ch == "_" { out.unicodeScalars.append(ch) }
        if out.count == 16 { break }
    }
    return out.isEmpty ? "Player" : out
}

private let xstsErrors: [Int: String] = [
    2148916233: "no Xbox profile on this account (sign in once at minecraft.net)",
    2148916235: "Xbox Live is not available in this country",
    2148916236: "an adult needs to verify the account",
    2148916237: "an adult needs to verify the account",
    2148916238: "child account not in a Family",
]

/// Refresh token -> (MSA access token, rotated refresh token).
private func msaRefresh(_ refresh: String) throws -> (String, String) {
    let (s1, msa) = try call("msa refresh", msaAuthority + "/token", form: [
        "client_id": msaClientID, "grant_type": "refresh_token",
        "refresh_token": refresh, "scope": msaScopes])
    guard let tok = msa["access_token"] as? String else {
        let e = msa["error"] as? String ?? "HTTP \(s1)"
        throw MSAFail(reason: e == "invalid_grant"
            ? "the refresh token was refused (expired or revoked) — remove the account and add it again"
            : "msa refresh failed (\(e))")
    }
    return (tok, msa["refresh_token"] as? String ?? refresh)
}

/// MSA access token -> Minecraft session (XBL, XSTS, login_with_xbox, profile).
private func minecraftSession(_ msaToken: String) throws -> MCSession {
    let (s2, xbl) = try call("xbl", "https://user.auth.xboxlive.com/user/authenticate", json: [
        "Properties": ["AuthMethod": "RPS", "SiteName": "user.auth.xboxlive.com", "RpsTicket": "d=" + msaToken],
        "RelyingParty": "http://auth.xboxlive.com", "TokenType": "JWT"])
    guard let xblToken = xbl["Token"] as? String else { throw MSAFail(reason: "xbl auth failed (HTTP \(s2))") }

    let (s3, xsts) = try call("xsts", "https://xsts.auth.xboxlive.com/xsts/authorize", json: [
        "Properties": ["SandboxId": "RETAIL", "UserTokens": [xblToken]],
        "RelyingParty": "rp://api.minecraftservices.com/", "TokenType": "JWT"])
    if s3 == 401 {
        let code = (xsts["XErr"] as? NSNumber)?.intValue ?? 0
        throw MSAFail(reason: "xsts: " + (xstsErrors[code] ?? "refused (XErr \(code))"))
    }
    guard let xstsToken = xsts["Token"] as? String,
          let claims = xsts["DisplayClaims"] as? [String: Any],
          let xui = claims["xui"] as? [[String: Any]], let uhs = xui.first?["uhs"] as? String else {
        throw MSAFail(reason: "xsts failed (HTTP \(s3))")
    }

    let (s4, mc) = try call("minecraft login", "https://api.minecraftservices.com/authentication/login_with_xbox",
                            json: ["identityToken": "XBL3.0 x=\(uhs);\(xstsToken)"])
    guard let mcToken = mc["access_token"] as? String else {
        throw MSAFail(reason: "minecraft login failed (HTTP \(s4))")
    }
    let expiresIn = (mc["expires_in"] as? NSNumber)?.doubleValue ?? 86400

    let (s5, prof) = try call("profile", "https://api.minecraftservices.com/minecraft/profile", bearer: mcToken)
    if s5 != 404 {
        let name = prof["name"] as? String ?? "", id = prof["id"] as? String ?? ""
        guard OwnershipGateLogic.classify(profileStatus: s5, profileHasIdentity: !name.isEmpty && !id.isEmpty,
                                          entitlementsStatus: nil, javaEntitled: nil) == .owner else {
            throw serviceFail("profile", s5)
        }
        let skins = prof["skins"] as? [[String: Any]] ?? []
        let skin = (skins.first { ($0["state"] as? String) == "ACTIVE" } ?? skins.first)?["url"] as? String
        return MCSession(accessToken: mcToken, expiresOn: (Date().timeIntervalSince1970 + expiresIn) * 1000,
                         username: name, uuid: dashed(id), xuid: xuidClaim(mcToken), skin: skin)
    }

    // No Java profile. Whether Java Edition is on the account is the
    // entitlements' answer; the reply carries signed JWTs and is never logged.
    let (s6, ent) = try call("entitlements", mcEntitlementsURL, bearer: mcToken)
    let items = (ent["items"] as? [[String: Any]]).map { $0.compactMap { $0["name"] as? String } }
    switch OwnershipGateLogic.classify(profileStatus: s5, profileHasIdentity: false, entitlementsStatus: s6,
                                       javaEntitled: OwnershipGateLogic.javaEntitled(items: items)) {
    case .needsProfile: return try freeSession(xblToken, needsProfile: true)
    case .notEntitled: return try freeSession(xblToken, needsProfile: false)
    case .owner, .serviceError: throw serviceFail("entitlements", s6, noList: s6 == 200 && items == nil)
    }
}

/// A reply that decides nothing. The caller keeps the stored license state.
private func serviceFail(_ step: String, _ status: Int, noList: Bool = false) -> MSAFail {
    MSAFail(reason: "\(step) check failed (HTTP \(status)\(noList ? ", no entitlement list" : "")): "
        + "a Minecraft services problem, not a license answer — try again")
}

/// A Microsoft account without a Java profile: its gamertag, through XSTS for
/// Xbox Live itself (the Minecraft relying party carries no gamertag). Plays
/// offline under that name; no Minecraft token is kept. `needsProfile` = the
/// entitlements list Java Edition (still not ownership proof: no profile).
private func freeSession(_ xblToken: String, needsProfile: Bool) throws -> MCSession {
    let (s, x) = try call("xsts (xboxlive)", "https://xsts.auth.xboxlive.com/xsts/authorize", json: [
        "Properties": ["SandboxId": "RETAIL", "UserTokens": [xblToken]],
        "RelyingParty": "http://xboxlive.com", "TokenType": "JWT"])
    if s == 401 {
        let code = (x["XErr"] as? NSNumber)?.intValue ?? 0
        throw MSAFail(reason: "xsts: " + (xstsErrors[code] ?? "refused (XErr \(code))"))
    }
    guard let claims = x["DisplayClaims"] as? [String: Any],
          let xui = (claims["xui"] as? [[String: Any]])?.first,
          let gtg = xui["gtg"] as? String, !gtg.trimmingCharacters(in: .whitespaces).isEmpty else {
        throw MSAFail(reason: needsProfile
            ? "Java Edition is on this account but its Java profile is not set up (minecraft.net/msaprofile), and Xbox gave no gamertag (HTTP \(s))"
            : "no Java Edition on this account, and Xbox gave no gamertag (HTTP \(s))")
    }
    let name = playerName(gtg)
    return MCSession(accessToken: "", expiresOn: (Date().timeIntervalSince1970 + 86400) * 1000,
                     username: name, uuid: offlineUUID(name), xuid: xui["xid"] as? String ?? "",
                     owns: false, gamertag: gtg, needsProfile: needsProfile)
}

// --- device-code sign-in (Add account…) ----------------------------------------

private struct DeviceCode {
    var deviceCode: String, userCode: String, uri: String, interval: Double, expiresAt: Date
}

private func deviceCodeStart() throws -> DeviceCode {
    let (s, d) = try call("device code", msaAuthority + "/devicecode",
                          form: ["client_id": msaClientID, "scope": msaScopes])
    guard let dc = d["device_code"] as? String, let uc = d["user_code"] as? String else {
        throw MSAFail(reason: "Microsoft did not return a code (\(d["error"] as? String ?? "HTTP \(s)"))")
    }
    return DeviceCode(deviceCode: dc, userCode: uc,
                      uri: d["verification_uri"] as? String ?? "https://www.microsoft.com/link",
                      interval: max((d["interval"] as? NSNumber)?.doubleValue ?? 5, 1),
                      expiresAt: Date().addingTimeInterval((d["expires_in"] as? NSNumber)?.doubleValue ?? 900))
}

private enum PollResult { case pending, slowDown, done(access: String, refresh: String) }

private func deviceCodePoll(_ dc: DeviceCode) throws -> PollResult {
    let (s, d) = try call("token poll", msaAuthority + "/token", form: [
        "client_id": msaClientID, "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
        "device_code": dc.deviceCode])
    if let a = d["access_token"] as? String, let r = d["refresh_token"] as? String {
        return .done(access: a, refresh: r)
    }
    switch d["error"] as? String ?? "" {
    case "authorization_pending": return .pending
    case "slow_down": return .slowDown
    case "expired_token", "bad_verification_code": throw MSAFail(reason: "the code expired before it was confirmed")
    case "authorization_declined", "access_denied": throw MSAFail(reason: "the sign-in was declined")
    case let e: throw MSAFail(reason: "token poll failed (\(e.isEmpty ? "HTTP \(s)" : e))")
    }
}

// --- the store ------------------------------------------------------------------

private func writePrivate(_ data: Data, _ url: URL) throws {
    let fm = FileManager.default
    try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true,
                           attributes: [.posixPermissions: 0o700])
    #if os(macOS)
    try data.write(to: url, options: [.atomic])
    #else
    try data.write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    #endif
    try? fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
}

private func readJSON(_ url: URL) -> [String: Any]? {
    guard let d = try? Data(contentsOf: url) else { return nil }
    return (try? JSONSerialization.jsonObject(with: d)) as? [String: Any]
}

final class MSAStore: @unchecked Sendable {
    static let offline = "offline"
    /// The window's picker, for this boot only (nil = use the default).
    nonisolated(unsafe) static var bootChoice: String?

    private static let lock = NSLock()
    nonisolated(unsafe) private static var stores: [String: MSAStore] = [:]
    static func forTree(_ tree: String) -> MSAStore {
        lock.lock(); defer { lock.unlock() }
        if let s = stores[tree] { return s }
        let s = MSAStore(tree: tree); stores[tree] = s; return s
    }

    let tree: String
    private let dir: URL
    private let fileLock = NSLock()
    private init(tree: String) {
        self.tree = tree
        dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("klepton-msa", isDirectory: true)
            .appendingPathComponent(tree, isDirectory: true)
    }
    private var fileURL: URL { dir.appendingPathComponent("accounts.json") }

    /// Load (migrating, emptying the inbox), mutate, save — under the lock.
    /// Never held across the network.
    private func edit<T>(_ body: (inout MSAFile) -> T) -> T {
        fileLock.lock(); defer { fileLock.unlock() }
        var f = MSAFile()
        if let d = try? Data(contentsOf: fileURL), let parsed = try? JSONDecoder().decode(MSAFile.self, from: d) {
            f = parsed
        } else {
            migrateLegacy(&f)
        }
        takeInbox(&f)
        let wasUnlocked = f.deviceUnlocked
        applyUnlock(&f)
        let r = body(&f)
        applyUnlock(&f)
        if !wasUnlocked && f.deviceUnlocked {
            // No account name: the line is the proof, and a screenshot or a
            // pasted log should not have to be scrubbed for one.
            NSLog("[gate] device unlocked: a Microsoft account that owns Minecraft: Java Edition has signed in on this device")
            fputs("[gate] device unlocked: a Microsoft account that owns Minecraft: Java Edition has signed in on this device\n", stderr)
        }
        if let d = try? JSONEncoder().encode(f) { try? writePrivate(d, fileURL) }
        for n in ["secrets.json", "session.json"] {   // migrated (or superseded)
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(n))
        }
        return r
    }

    /// The first layout: one secrets.json (refresh token) + session.json.
    private func migrateLegacy(_ f: inout MSAFile) {
        guard let rt = readJSON(dir.appendingPathComponent("secrets.json"))?["refresh_token"] as? String,
              !rt.isEmpty else { return }
        var a = MSAAccount(id: UUID().uuidString, refreshToken: rt)
        if let s = readJSON(dir.appendingPathComponent("session.json")) {
            a.name = s["username"] as? String ?? ""
            a.uuid = s["uuid"] as? String ?? ""
            a.xuid = s["xuid"] as? String ?? ""
            a.accessToken = s["accessToken"] as? String ?? ""
            a.expiresOn = (s["expiresOn"] as? NSNumber)?.doubleValue ?? 0
        }
        f.accounts.append(a)
        if f.defaultID == nil { f.defaultID = a.id }
    }

    private func takeInbox(_ f: inout MSAFile) {
        let inbox = URL(fileURLWithPath: Paths.container).appendingPathComponent(tree)
            .appendingPathComponent("msa-refresh.json")
        guard FileManager.default.fileExists(atPath: inbox.path) else { return }
        defer { try? FileManager.default.removeItem(at: inbox) }
        guard let rt = readJSON(inbox)?["refresh_token"] as? String, !rt.isEmpty,
              !f.accounts.contains(where: { $0.refreshToken == rt }) else { return }
        let a = MSAAccount(id: UUID().uuidString, refreshToken: rt)
        f.accounts.append(a)
        if f.defaultID == nil { f.defaultID = a.id }
    }

    /// Store a resolved session on account `id` (with its rotated refresh
    /// token), folding it into an existing account with the same profile uuid
    /// if there is one. Returns the surviving account's id.
    @discardableResult
    private func storeSession(_ s: MCSession, refresh: String, into id: String?, _ f: inout MSAFile) -> String {
        var a: MSAAccount
        if let id, let i = f.accounts.firstIndex(where: { $0.id == id }) { a = f.accounts.remove(at: i) }
        else { a = MSAAccount(id: UUID().uuidString, refreshToken: refresh) }
        if !s.owns, a.owns, a.resolved, let k = a.profileKey {
            NSLog("[msa] %@ (profile %@) no longer owns Java Edition: plays offline", a.name, k)
        }
        if s.owns, !a.owns, a.resolved { NSLog("[msa] %@ now owns Java Edition: upgraded to %@", a.name, s.username) }
        if s.needsProfile, !a.needsJavaProfile {
            NSLog("[msa] %@: Java Edition is on the account, its Java profile is not set up yet (minecraft.net/msaprofile)", s.username)
        }
        a.name = s.username; a.uuid = s.uuid; a.xuid = s.xuid
        a.refreshToken = refresh; a.accessToken = s.accessToken; a.expiresOn = s.expiresOn
        a.ownsGame = s.owns
        a.javaCheck = (s.owns ? OwnershipGateLogic.JavaCheck.owner
            : s.needsProfile ? .needsProfile : .notEntitled).rawValue
        a.gamertag = s.gamertag
        if let skin = s.skin { a.skin = skin } else if !s.owns { a.skin = nil }
        a.lastError = ""
        // The same person again: the same profile uuid, or (free <-> owned) the
        // same Xbox user id.
        if let j = f.accounts.firstIndex(where: {
            $0.uuid.lowercased() == s.uuid.lowercased() || (!s.xuid.isEmpty && $0.xuid == s.xuid)
        }) {
            // That profile is already listed: keep its id, and so the default
            // (and its game dir).
            let keep = f.accounts[j].id
            if f.defaultID == a.id { f.defaultID = keep }
            a.id = keep
            if a.profileKey == nil { a.profileKey = f.accounts[j].profileKey }
            f.accounts[j] = a
        } else {
            f.accounts.append(a)
        }
        if f.defaultID == nil { f.defaultID = a.id }
        return a.id
    }

    /// Sticky unlock. An owning account already in the file (legacy included)
    /// sets the flag on this read, so a device that signed in before the gate
    /// existed is not locked out. Removing that account later does not clear it.
    func isDeviceUnlocked() -> Bool {
        edit { $0.deviceUnlocked }
    }

    private func applyUnlock(_ f: inout MSAFile) {
        let accounts = f.accounts.map {
            OwnershipGateLogic.Account(resolved: $0.resolved, ownsGame: $0.ownsGame)
        }
        f.deviceUnlocked = OwnershipGateLogic.unlocked(stored: f.deviceUnlocked, accounts: accounts)
    }

    // --- what the UI reads and does ---

    struct Snapshot { var accounts: [MSAAccount]; var defaultID: String; var deviceUnlocked: Bool }
    /// Tokens blanked: the UI never holds one.
    func snapshot() -> Snapshot {
        edit { f in
            Snapshot(accounts: f.accounts.map { var a = $0; a.refreshToken = ""; a.accessToken = ""; return a },
                     defaultID: f.defaultID ?? MSAStore.offline,
                     deviceUnlocked: f.deviceUnlocked)
        }
    }
    func setDefault(_ id: String) { edit { f in f.defaultID = id } }
    func remove(_ id: String) {
        edit { f in
            f.accounts.removeAll { $0.id == id }
            if f.defaultID == id { f.defaultID = f.accounts.first(where: { $0.resolved })?.id ?? MSAStore.offline }
        }
    }
    /// Device-code result -> a resolved account. Network: call off main.
    func add(msaAccess: String, refresh: String) throws -> String {
        let s = try minecraftSession(msaAccess)
        return edit { f in
            let id = storeSession(s, refresh: refresh, into: nil, &f)
            return f.accounts.first(where: { $0.id == id })?.name ?? s.username
        }
    }

    /// A usable session for account `id`: the cached token while > 1 h is left,
    /// else the refresh chain (network outside the lock). (session, note).
    /// `force` (Check again) skips the cache.
    fileprivate func session(for id: String, force: Bool = false) -> (MCSession?, String) {
        guard let a = edit({ f in f.accounts.first(where: { $0.id == id }) }) else {
            return (nil, "account not found")
        }
        let cached: MCSession? = a.resolved && (!a.accessToken.isEmpty || !a.owns)
            ? MCSession(accessToken: a.accessToken, expiresOn: a.expiresOn, username: a.name, uuid: a.uuid, xuid: a.xuid,
                        owns: a.owns, gamertag: a.gamertag, needsProfile: a.needsJavaProfile)
            : nil
        // A free account the entitlements confirmed re-checks once a day
        // (expiresOn = resolve + 24 h), so a purchase upgrades it within a
        // day. One still to set up its profile, or judged by a 404 alone, is
        // asked every time.
        let trusted = OwnershipGateLogic.cacheTrusted(ownsGame: a.ownsGame, javaCheck: a.javaCheck)
        if !force, trusted, let c = cached, c.secondsLeft > 3600 { return (c, "cached") }
        do {
            let (access, newRefresh) = try msaRefresh(a.refreshToken)
            // Microsoft rotates the refresh token on every use: keep the new
            // one now, so a profile or entitlement failure below cannot lose it.
            if newRefresh != a.refreshToken {
                edit { f in
                    if let i = f.accounts.firstIndex(where: { $0.id == id }) { f.accounts[i].refreshToken = newRefresh }
                }
            }
            let s = try minecraftSession(access)
            edit { f in _ = storeSession(s, refresh: newRefresh, into: id, &f) }
            return (s, "refreshed")
        } catch {
            let reason = (error as? MSAFail)?.reason ?? "unexpected error"
            edit { f in
                if let i = f.accounts.firstIndex(where: { $0.id == id }) { f.accounts[i].lastError = reason }
            }
            // A network blip with a still-valid cached token is not a reason
            // to play offline.
            if let c = cached, c.secondsLeft > 120 { return (c, "cached (refresh failed: \(reason))") }
            // A free account plays offline anyway: a failed refresh only means
            // a purchase goes unnoticed this time.
            if let c = cached, !c.owns { return (c, "stored offline name (refresh failed: \(reason))") }
            return (nil, reason)
        }
    }

    /// The game-dir key for account `id` (MCProfiles): the stored one, else
    /// assigned now and saved — "xuid-<Xbox user id>" when the account has
    /// one (the same person re-added after a Remove finds their worlds
    /// again; a free account that buys the game keeps its dir), else
    /// "acct-<account id>". nil = no such account.
    func profileKey(for id: String) -> String? {
        edit { f in
            guard let i = f.accounts.firstIndex(where: { $0.id == id }) else { return nil }
            if let k = f.accounts[i].profileKey, !k.isEmpty { return k }
            let a = f.accounts[i]
            let raw = a.xuid.isEmpty ? "acct-" + a.id.lowercased() : "xuid-" + a.xuid
            let k = String(raw.unicodeScalars.filter { $0.isASCII && (CharacterSet.alphanumerics.contains($0) || $0 == "-") })
            f.accounts[i].profileKey = k
            return k
        }
    }

    /// Resolve every account that has never been signed in (inbox; a migration
    /// without a session). Network: off main.
    func resolvePending() {
        let ids = edit { f in f.accounts.filter { !$0.resolved && $0.lastError.isEmpty }.map(\.id) }
        for id in ids { _ = session(for: id) }
    }

    /// Ask again for every resolved account whose stored answer is not final
    /// (OwnershipGateLogic.recheckOnOpen). A failure only sets lastError: the
    /// stored license state stays. Network: off main.
    func recheckStale() {
        let ids = edit { f in
            f.accounts.filter {
                OwnershipGateLogic.recheckOnOpen(resolved: $0.resolved, ownsGame: $0.ownsGame, javaCheck: $0.javaCheck)
            }.map(\.id)
        }
        for id in ids { _ = session(for: id) }
    }

    /// Check again (the wearer asked): the whole chain now, cache or not. An
    /// unresolved account whose sign-in failed retries with its stored token.
    /// Network: off main.
    func recheck(_ id: String) {
        let (s, note) = session(for: id, force: true)
        guard let s, note == "refreshed" else { NSLog("[msa] check again failed: %@", note); return }
        NSLog("[msa] check again: %@", s.owns ? "owns Java Edition"
              : s.needsProfile ? "Java Edition, Java profile not set up yet" : "no Java Edition")
    }

    static func summary(_ snap: Snapshot) -> String {
        let def = snap.accounts.first { $0.id == snap.defaultID }
        return "\(snap.accounts.count) (default \(def.map { $0.resolved ? $0.name : "unresolved" } ?? "offline"))"
    }

    /// The account this boot uses: (id, or nil = offline; why; summary).
    func pickForBoot(override: String?) -> (String?, String, String) {
        let snap = snapshot()
        let summary = MSAStore.summary(snap)
        if let o = override?.trimmingCharacters(in: .whitespaces), !o.isEmpty {
            if o.lowercased() == MSAStore.offline { return (nil, "KL_MSA_ACCOUNT=offline", summary) }
            let key = o.lowercased().replacingOccurrences(of: "-", with: "")
            if let a = snap.accounts.first(where: {
                $0.name.lowercased() == o.lowercased()
                    || (!$0.uuid.isEmpty && $0.uuid.lowercased().replacingOccurrences(of: "-", with: "") == key)
            }) { return (a.id, "KL_MSA_ACCOUNT", summary) }
            return (nil, "KL_MSA_ACCOUNT=\(o) matches no account", summary)
        }
        let choice = MSAStore.bootChoice ?? snap.defaultID
        if choice == MSAStore.offline {
            return (nil, snap.accounts.isEmpty
                ? "no account (Add account… in the window, or tools/vivecraft_msa_login.py + stage_native.sh --msa)"
                : "Offline (Player) chosen", summary)
        }
        guard snap.accounts.contains(where: { $0.id == choice }) else {
            return (nil, "the chosen account is gone", summary)
        }
        return (choice, MSAStore.bootChoice != nil && choice != snap.defaultID ? "picked" : "default", summary)
    }
}

// --- the ownership gate -----------------------------------------------------------
//
// The UI reads the published flag. The launch itself asks the store, and so
// does kl_app.c (kl_ownership_allows_launch) right before the guest's main.
// KL_MSA=0, KL_LAUNCHER=0, KL_AUTOBOOT, KL_MC_PROFILES, the env file and a
// launcher script never get a vote.

#if KL_OWNERSHIP_GATE_OFF && !DEBUG
#error("KL_OWNERSHIP_GATE_OFF compiles the Minecraft ownership gate out. Release and public builds must not define it. Debug may, and only as a compile-time condition — there is no runtime switch.")
#endif

enum OwnershipGate {
    static let sentence = "Sign in with a Microsoft account that owns Minecraft: Java Edition to unlock this device."
    static let profileSetupSentence = "Minecraft: Java Edition is on a signed-in account, but its Java profile is not set up yet. Set it up at minecraft.net/msaprofile, then Check again to unlock this device."
    /// Microsoft's page for creating the Java profile (player name).
    static let profileSetupURL = URL(string: "https://www.minecraft.net/msaprofile")!

    /// The locked-device line: the profile-setup one when that is what stands
    /// between a signed-in account and the unlock. Wording only; the gate
    /// itself is isDeviceUnlocked.
    static func lockedSentence(for accounts: [MSAAccount]) -> String {
        accounts.contains { $0.needsJavaProfile } ? profileSetupSentence : sentence
    }

    /// False for any target that is not the Minecraft row, and for a Debug
    /// build that was compiled with KL_OWNERSHIP_GATE_OFF. Release cannot
    /// define that condition (the #error above).
    static var enforced: Bool {
        #if KL_OWNERSHIP_GATE_OFF
        return false
        #else
        return MSAAccounts.isMinecraft
        #endif
    }

    static func allowsLaunch() -> Bool {
        OwnershipGateProbe.runOnce()
        if !enforced { return true }
        return MSAStore.forTree("vivecraft").isDeviceUnlocked()
    }

    /// Fail closed until the store has been read into the UI.
    static func blocksLaunch(ready: Bool, unlocked: Bool) -> Bool {
        enforced && (!ready || !unlocked)
    }

    static func recordRefusal() {
        OwnershipGateLog.line("[gate] refused: this device has not been unlocked by a Microsoft account that owns Minecraft: Java Edition")
    }
}

private enum OwnershipGateLog {
    static func line(_ message: String) {
        let text = message.hasSuffix("\n") ? message : message + "\n"
        fputs(text, stderr)
        fflush(stderr)
        NSLog("%@", message)
        let url = URL(fileURLWithPath: Paths.container).appendingPathComponent("klepton-boot.log")
        guard let data = text.data(using: .utf8) else { return }
        if FileManager.default.fileExists(atPath: url.path),
           let handle = try? FileHandle(forWritingTo: url) {
            handle.seekToEndOfFile()
            try? handle.write(contentsOf: data)
            try? handle.close()
        } else {
            try? data.write(to: url)
        }
    }
}

/// Decodes real accounts.json shapes, including a file written before
/// deviceUnlocked existed. Runs once per process. A failure is logged; it
/// does not open the gate.
private enum OwnershipGateProbe {
    private static let lock = NSLock()
    private static var didRun = false

    static func runOnce() {
        lock.lock()
        let first = !didRun
        didRun = true
        lock.unlock()
        guard first else { return }
        if let err = run() {
            NSLog("[gate] selftest FAILED: %@", err)
            fputs("[gate] selftest FAILED: \(err)\n", stderr)
        } else {
            NSLog("[gate] selftest ok")
            fputs("[gate] selftest ok\n", stderr)
        }
    }

    private static func run() -> String? {
        if let err = OwnershipGateLogic.selfTest() { return err }
        if let err = jsonRoundTrip() { return err }
        return nil
    }

    private static func jsonRoundTrip() -> String? {
        var owner = MSAAccount(id: "id-owner", refreshToken: "rt-owner")
        owner.name = "Owner"
        owner.uuid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
        var free = MSAAccount(id: "id-free", refreshToken: "rt-free")
        free.name = "Free"
        free.uuid = "bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee"
        free.ownsGame = false
        var pending = MSAAccount(id: "id-pending", refreshToken: "rt-pending")

        var legacy = MSAFile()
        legacy.accounts = [owner]
        legacy.defaultID = owner.id
        guard let encoded = try? JSONEncoder().encode(legacy),
              var obj = (try? JSONSerialization.jsonObject(with: encoded)) as? [String: Any] else {
            return "encode failed"
        }
        obj.removeValue(forKey: "deviceUnlocked")
        guard let stripped = try? JSONSerialization.data(withJSONObject: obj),
              let decoded = try? JSONDecoder().decode(MSAFile.self, from: stripped) else {
            return "legacy file (no deviceUnlocked key) did not decode"
        }
        if decoded.deviceUnlocked { return "missing deviceUnlocked decoded as true" }
        if decoded.accounts.count != 1 || decoded.accounts[0].refreshToken != "rt-owner" {
            return "legacy decode dropped the account"
        }
        if !(decoded.accounts[0].resolved && decoded.accounts[0].owns) {
            return "legacy resolved account does not prove ownership"
        }
        let migrated = OwnershipGateLogic.unlocked(
            stored: decoded.deviceUnlocked,
            accounts: decoded.accounts.map { .init(resolved: $0.resolved, ownsGame: $0.ownsGame) })
        if !migrated { return "legacy owning account did not migrate to unlocked" }

        var locked = MSAFile()
        locked.accounts = [free, pending]
        let stillLocked = OwnershipGateLogic.unlocked(
            stored: locked.deviceUnlocked,
            accounts: locked.accounts.map { .init(resolved: $0.resolved, ownsGame: $0.ownsGame) })
        if stillLocked { return "free plus unresolved unlocked a fresh store" }

        var sticky = MSAFile()
        sticky.deviceUnlocked = true
        sticky.accounts = [free]
        guard let again = try? JSONEncoder().encode(sticky),
              let back = try? JSONDecoder().decode(MSAFile.self, from: again) else {
            return "sticky flag did not round-trip"
        }
        if !back.deviceUnlocked { return "deviceUnlocked true did not round-trip" }
        let kept = OwnershipGateLogic.unlocked(
            stored: back.deviceUnlocked,
            accounts: back.accounts.map { .init(resolved: $0.resolved, ownsGame: $0.ownsGame) })
        if !kept { return "removing the owner (flag already set, only a free account left) re-locked" }

        // javaCheck: absent (older files) and unknown values decode; a
        // profile still to be set up is free and proves nothing.
        var setup = free
        setup.javaCheck = OwnershipGateLogic.JavaCheck.needsProfile.rawValue
        guard let one = try? JSONEncoder().encode(setup),
              var acct = (try? JSONSerialization.jsonObject(with: one)) as? [String: Any] else {
            return "account encode failed"
        }
        guard let setupBack = try? JSONDecoder().decode(MSAAccount.self, from: one), setupBack.needsJavaProfile else {
            return "needsProfile did not round-trip"
        }
        if OwnershipGateLogic.Account(resolved: setupBack.resolved, ownsGame: setupBack.ownsGame).provesOwnership {
            return "a Java profile still to be set up proved ownership"
        }
        acct["javaCheck"] = "later-build"
        guard let odd = try? JSONSerialization.data(withJSONObject: acct),
              let oddBack = try? JSONDecoder().decode(MSAAccount.self, from: odd), !oddBack.needsJavaProfile else {
            return "unknown javaCheck value did not decode"
        }
        acct.removeValue(forKey: "javaCheck")
        guard let old = try? JSONSerialization.data(withJSONObject: acct),
              let oldBack = try? JSONDecoder().decode(MSAAccount.self, from: old), oldBack.javaCheck == nil,
              oldBack.refreshToken == "rt-free" else {
            return "account without javaCheck did not decode"
        }
        return nil
    }
}

// --- C entry ----------------------------------------------------------------------

private func put(_ s: String, _ buf: UnsafeMutableRawBufferPointer) -> Bool {
    let b = Array(s.utf8)
    guard b.count < buf.count else { return false }
    buf.copyBytes(from: b)
    buf[b.count] = 0
    return true
}

/// C entry (kl_app.c, native_msa_args): 0 and a filled session, or non-zero
/// with `note` = why the launch stays offline. `summary` is filled either way.
/// Blocks for the network chain (bounded by the request timeouts) — boot
/// thread only.
@_cdecl("kl_msa_session")
public func klMsaSession(_ treeC: UnsafePointer<CChar>?, _ out: UnsafeMutablePointer<kl_msa_session_t>?) -> Int32 {
    guard let out else { return 1 }
    withUnsafeMutableBytes(of: &out.pointee) { _ = $0.initializeMemory(as: UInt8.self, repeating: 0) }
    func offline(_ reason: String) -> Int32 {
        _ = withUnsafeMutableBytes(of: &out.pointee.note) { put(String(reason.prefix(250)), $0) }
        return 1
    }
    guard let treeC else { return offline("no tree") }
    if Thread.isMainThread { return offline("called on the main thread") }
    let store = MSAStore.forTree(String(cString: treeC))
    let (id, why, summary) = store.pickForBoot(override: ProcessInfo.processInfo.environment["KL_MSA_ACCOUNT"])
    _ = withUnsafeMutableBytes(of: &out.pointee.summary) { put(String(summary.prefix(150)), $0) }
    guard let id else { return offline(why) }
    let (s, note) = store.session(for: id)
    guard let s else { return offline(note) }
    var ok = withUnsafeMutableBytes(of: &out.pointee.name) { put(s.username, $0) }
    ok = ok && withUnsafeMutableBytes(of: &out.pointee.uuid) { put(s.uuid, $0) }
    // A free account: name + offline uuid only (kl_app.c keeps launch.txt's
    // offline token and user type, and passes no xuid).
    out.pointee.free_account = s.owns ? 0 : 1
    if s.owns {
        ok = ok && withUnsafeMutableBytes(of: &out.pointee.xuid) { put(s.xuid, $0) }
        ok = ok && withUnsafeMutableBytes(of: &out.pointee.token) { put(s.accessToken, $0) }
    }
    guard ok else {
        withUnsafeMutableBytes(of: &out.pointee) { _ = $0.initializeMemory(as: UInt8.self, repeating: 0) }
        return offline("session does not fit the C buffers")
    }
    _ = withUnsafeMutableBytes(of: &out.pointee.note) { put(String("\(note), \(why)".prefix(250)), $0) }
    out.pointee.hours = s.secondsLeft / 3600
    return 0
}

/// C entry (kl_app.c, native_run and kl_app_native_rerun): 1 = the guest may
/// start, 0 = the ownership gate refuses. Called on the boot thread, never
/// the main thread. The tree argument is the target's userdata directory;
/// the vivecraft row's accounts live under "vivecraft".
@_cdecl("kl_ownership_allows_launch")
public func klOwnershipAllowsLaunch(_ treeC: UnsafePointer<CChar>?) -> Int32 {
    OwnershipGateProbe.runOnce()
    #if KL_OWNERSHIP_GATE_OFF
    return 1
    #else
    let tree = treeC.map { String(cString: $0) } ?? ""
    // The launcher's accounts are the vivecraft store. A different target
    // (tree not vivecraft, and this bundle is not the Minecraft row) is not
    // this gate.
    guard MSAAccounts.isMinecraft || tree == "vivecraft" else { return 1 }
    return MSAStore.forTree("vivecraft").isDeviceUnlocked() ? 1 : 0
    #endif
}

// --- the window's account panel ---------------------------------------------------

@MainActor
final class MSAAccounts: ObservableObject {
    static let shared = MSAAccounts()
    let tree = "vivecraft"

    /// Shown only for the Minecraft row, and not when KL_MSA=0 is in the
    /// launch environment (an env-file KL_MSA=0 still forces offline at boot:
    /// the env file is read later, by kl_app_configure).
    static var wanted: Bool { isMinecraft && klEnvOn("KL_MSA", default: true) }

    /// The Minecraft row, whatever KL_MSA says (the themed launcher keys on it).
    nonisolated static var isMinecraft: Bool {
        // kl_app_target_name() is "(unconfigured)" until boot, so this asks
        // for the target the same way configure will: KL_TARGET, then the
        // build's compiled-in default. Not the bundle id: SideStore installs
        // under <id>.<team id>, and a suffix test then showed the generic
        // Klepton window instead of the launcher.
        String(cString: kl_app_target_now_name()) == "vivecraft"
    }

    enum AddState: Equatable {
        case idle, requesting
        case code(user: String, uri: String, expires: Date)
        case finishing
        case done(String), failed(String)
    }

    @Published var accounts: [MSAAccount] = []     // tokens blanked
    @Published var defaultID = MSAStore.offline
    @Published var selection = MSAStore.offline { didSet { MSAStore.bootChoice = selection } }
    /// Sticky device unlock (accounts.json deviceUnlocked). False until the
    /// first reload, and the launcher treats that as locked.
    @Published var deviceUnlocked = false
    @Published var gateReady = false
    @Published var hold = false            // the wearer touched the panel: no auto-boot
    @Published var countdown = 0
    @Published var add: AddState = .idle
    @Published var resolving = false
    /// The account a Check again is running for.
    @Published var rechecking: String?
    private var loaded = false
    private var staleChecked = false       // recheckStale: once per process
    private var addCancelled = MSABox(false)

    private var store: MSAStore { MSAStore.forTree(tree) }

    private func publish(_ snap: MSAStore.Snapshot) {
        accounts = snap.accounts
        defaultID = snap.defaultID
        deviceUnlocked = snap.deviceUnlocked
        gateReady = true
        let valid = selection == MSAStore.offline || snap.accounts.contains { $0.id == selection }
        if !loaded || !valid {
            selection = snap.accounts.contains { $0.id == snap.defaultID } ? snap.defaultID : MSAStore.offline
        }
        loaded = true
    }

    private func background(_ work: @escaping @Sendable (MSAStore) -> MSAStore.Snapshot) {
        let s = store
        Task {
            let snap = await Task.detached { work(s) }.value
            publish(snap)
        }
    }

    /// Load (migration + inbox) and show the list at once, then resolve any
    /// unresolved account in the background and show its name. The first
    /// load also asks again for any account whose stored answer is not final
    /// (a Java profile still to be set up; a free record from a 404 alone).
    func reload() async {
        let s = store
        let snap = await Task.detached { s.snapshot() }.value
        publish(snap)
        NSLog("[msa] accounts: %@", MSAStore.summary(snap))
        let stale = !staleChecked && snap.accounts.contains {
            OwnershipGateLogic.recheckOnOpen(resolved: $0.resolved, ownsGame: $0.ownsGame, javaCheck: $0.javaCheck)
        }
        staleChecked = true
        guard stale || snap.accounts.contains(where: { !$0.resolved && $0.lastError.isEmpty }) else { return }
        resolving = true
        let after = await Task.detached {
            s.resolvePending()
            if stale { s.recheckStale() }
            return s.snapshot()
        }.value
        resolving = false
        publish(after)
    }

    /// Check again for account `id`, in the background.
    func recheck(id: String) {
        hold = true
        guard id != MSAStore.offline, rechecking == nil, !resolving else { return }
        rechecking = id
        let s = store
        Task {
            let snap = await Task.detached { s.recheck(id); return s.snapshot() }.value
            rechecking = nil
            publish(snap)
        }
    }

    func makeDefault() { makeDefault(id: selection) }
    func makeDefault(id: String) {
        hold = true
        background { $0.setDefault(id); return $0.snapshot() }
    }
    func remove() { remove(id: selection) }
    func remove(id: String) {
        hold = true
        guard id != MSAStore.offline else { return }
        if selection == id { selection = MSAStore.offline }
        background { $0.remove(id); return $0.snapshot() }
    }

    /// The auto-boot's grace period: KL_MSA_CHOOSER_SECONDS (default 3) during
    /// which any touch on the panel holds the boot. Returns true to boot.
    func countdownThenBoot() async -> Bool {
        await reload()
        // KL_MSA_ADD=1 opens "Add account…" at launch (and so holds the boot):
        // the scripted way to reach the sign-in screen.
        if klEnvOn("KL_MSA_ADD", default: false) { startAdd() }
        let secs = Int(ProcessInfo.processInfo.environment["KL_MSA_CHOOSER_SECONDS"] ?? "") ?? 3
        var left = max(secs, 0)
        while left > 0, !hold {
            countdown = left
            try? await Task.sleep(for: .seconds(1))
            left -= 1
        }
        countdown = 0
        return !hold
    }

    func label(_ a: MSAAccount) -> String {
        if a.resolved { return a.name }
        return a.lastError.isEmpty ? "Signing in…" : "Sign-in failed"
    }

    var selectionLabel: String {
        accounts.first { $0.id == selection }.map(label) ?? "Offline (Player)"
    }

    var addBusy: Bool {
        switch add { case .requesting, .code, .finishing: return true; default: return false }
    }

    /// Device-code sign-in, on a thread of its own: the chain's calls are
    /// synchronous and the poll sleeps between them.
    func startAdd() {
        hold = true
        guard !addBusy else { return }
        add = .requesting
        let cancelled = MSABox(false); addCancelled = cancelled
        let s = store
        let set: @Sendable (AddState) -> Void = { st in
            Task { @MainActor in if !cancelled.v { MSAAccounts.shared.add = st } }
        }
        Thread.detachNewThread {
            do {
                let dc = try deviceCodeStart()
                NSLog("[msa] add account: code issued, polling every %.0f s", dc.interval)
                set(.code(user: dc.userCode, uri: dc.uri, expires: dc.expiresAt))
                var interval = dc.interval
                while true {
                    let wake = Date().addingTimeInterval(interval)
                    while Date() < wake { if cancelled.v { return }; Thread.sleep(forTimeInterval: 0.25) }
                    if cancelled.v { return }
                    if Date() > dc.expiresAt { throw MSAFail(reason: "the code expired before it was confirmed") }
                    switch try deviceCodePoll(dc) {
                    case .pending: continue
                    case .slowDown: interval += 5
                    case let .done(access, refresh):
                        if cancelled.v { return }
                        set(.finishing)
                        let name = try s.add(msaAccess: access, refresh: refresh)
                        let snap = s.snapshot()
                        let added = snap.accounts.first(where: { $0.name == name })
                        NSLog("[msa] account added: %@%@", name,
                              added?.needsJavaProfile == true ? " (Java Edition, Java profile not set up yet: plays offline)"
                              : added.map { !$0.owns } == true ? " (free account: no Java Edition, plays offline)" : "")
                        Task { @MainActor in
                            let m = MSAAccounts.shared
                            if cancelled.v { return }
                            m.publish(snap)
                            if let a = snap.accounts.first(where: { $0.name == name }) { m.selection = a.id }
                            m.add = .done(name)
                        }
                        return
                    }
                }
            } catch {
                let reason = (error as? MSAFail)?.reason ?? "unexpected error"
                NSLog("[msa] add account failed: %@", reason)
                set(.failed(reason))
            }
        }
    }

    func cancelAdd() { addCancelled.v = true; add = .idle }
}

/// The selected account's Java Edition status in words, with what to do next
/// (both account panels). Nothing for an owner whose last check went through.
struct MSAJavaStatusNote: View {
    @ObservedObject var m = MSAAccounts.shared
    var account: MSAAccount
    var light = false          // the launcher's dark panels

    static func text(_ a: MSAAccount) -> String? {
        let last = a.lastError.isEmpty ? "" : " Last check: \(a.lastError)."
        if !a.resolved { return a.lastError.isEmpty ? nil : "Sign-in did not finish: \(a.lastError)." }
        if a.needsJavaProfile {
            return "Minecraft: Java Edition is on this Microsoft account, but its Java profile (player name) is not set up yet. Set it up at minecraft.net/msaprofile, then Check again." + last
        }
        if !a.owns {
            if a.javaCheck == OwnershipGateLogic.JavaCheck.notEntitled.rawValue {
                return "Minecraft services list no Java Edition on this Microsoft account. If Java Edition is on a different Microsoft account, add that one. Check again after any change." + last
            }
            return "Java Edition is not confirmed for this account yet. Check again." + last
        }
        return a.lastError.isEmpty ? nil : "The last check did not go through: \(a.lastError). The saved sign-in is kept."
    }

    var body: some View {
        if let text = Self.text(account) {
            VStack(alignment: .leading, spacing: 6) {
                Label(text, systemImage: account.needsJavaProfile ? "person.badge.plus"
                      : account.lastError.isEmpty ? "info.circle" : "exclamationmark.triangle")
                    .foregroundStyle(light ? Color.white : Color.primary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 12) {
                    if account.needsJavaProfile {
                        Link("Set up Java profile", destination: OwnershipGate.profileSetupURL)
                    }
                    Button(m.rechecking == account.id ? "Checking…" : "Check again") { m.recheck(id: account.id) }
                        .disabled(m.rechecking != nil || m.resolving || m.addBusy)
                    if m.rechecking == account.id { ProgressView().controlSize(.small) }
                }
            }
            .font(.callout)
        }
    }
}

struct AccountsPanel: View {
    @ObservedObject var m = MSAAccounts.shared
    /// After boot the account is fixed for this run; adding still works (it
    /// is for the next launch).
    var locked: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                Image(systemName: "person.crop.circle")
                Picker("Account", selection: Binding(
                    get: { m.selection },
                    set: { m.selection = $0; m.hold = true })) {
                    ForEach(m.accounts, id: \.id) { a in
                        Text(a.id == m.defaultID ? "\(m.label(a)) (default)" : m.label(a)).tag(a.id)
                    }
                    Text(m.defaultID == MSAStore.offline ? "Offline (Player) (default)" : "Offline (Player)")
                        .tag(MSAStore.offline)
                }
                .pickerStyle(.menu)
                .disabled(locked)
                if m.resolving { ProgressView().controlSize(.small) }
                Button("Make default") { m.makeDefault() }
                    .disabled(m.selection == m.defaultID)
                Button("Remove") { m.remove() }
                    .disabled(m.selection == MSAStore.offline || locked)
                Button("Add account…") { m.startAdd() }
                    .disabled(m.addBusy)
                Spacer()
                if m.countdown > 0, !m.hold {
                    Text("Booting as \(m.selectionLabel) in \(m.countdown) s")
                        .font(.footnote).foregroundStyle(.secondary)
                    Button("Wait") { m.hold = true }
                }
            }
            addView
            if let a = m.accounts.first(where: { $0.id == m.selection }) { MSAJavaStatusNote(account: a) }
        }
        .font(.callout)
    }

    @ViewBuilder private var addView: some View {
        switch m.add {
        case .idle:
            EmptyView()
        case .requesting:
            HStack { ProgressView(); Text("Asking Microsoft for a sign-in code…"); Button("Cancel") { m.cancelAdd() } }
        case let .code(user, uri, expires):
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    Text("On a phone or computer, open").foregroundStyle(.secondary)
                    if let url = URL(string: uri) { Link(uri, destination: url) } else { Text(uri) }
                }
                HStack(spacing: 12) {
                    Text("and enter").foregroundStyle(.secondary)
                    Text(user).font(.system(.title, design: .monospaced).bold()).textSelection(.enabled)
                    Button { copy(user) } label: { Label("Copy", systemImage: "doc.on.doc") }
                }
                HStack(spacing: 12) {
                    ProgressView().controlSize(.small)
                    Text("Waiting for sign-in · code expires \(expires, style: .relative)")
                        .font(.footnote).foregroundStyle(.secondary)
                    Button("Cancel") { m.cancelAdd() }
                }
            }
            .padding(12)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
        case .finishing:
            HStack { ProgressView(); Text("Signed in — fetching the Minecraft profile…") }
        case let .done(name):
            HStack {
                Label("Added \(name)", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                Button("OK") { m.cancelAdd() }
            }
        case let .failed(reason):
            HStack {
                Label(reason, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
                Button("OK") { m.cancelAdd() }
            }
        }
    }

    private func copy(_ s: String) {
        #if canImport(UIKit)
        UIPasteboard.general.string = s
        #endif
    }
}
