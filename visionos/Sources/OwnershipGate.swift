import Foundation

// Whether this device may run Minecraft. Pure on purpose: the MSA store and
// the launcher call it, and `swiftc -D OWNERSHIP_GATE_TEST` runs the checks
// in this file alone.
//
// Proof of ownership is an account that has already resolved through
// /minecraft/profile (HTTP 200). ownsGame == nil is a legacy resolved account
// from before free accounts existed; those all came back from that 200.
// ownsGame == false is a 404: no Java profile, which is either no Java
// Edition or a paid one whose profile is not set up yet (JavaCheck below);
// neither unlocks. An unresolved account — a
// refresh token that has not been exchanged yet — proves nothing.
//
// Once `stored` is true it stays true. Removing the owning account does not
// re-lock the device. Adding a free account does not clear it. The rule is
// "an owning account has signed in on this device at least once."
enum OwnershipGateLogic {
    struct Account {
        var resolved: Bool
        var ownsGame: Bool?
        /// Resolved and owning. A missing ownsGame counts only because the
        /// account resolved (legacy files); an unresolved account with a
        /// missing ownsGame does not.
        var provesOwnership: Bool { resolved && (ownsGame ?? true) }
    }

    static func unlocked(stored: Bool, accounts: [Account]) -> Bool {
        if stored { return true }
        return accounts.contains { $0.provesOwnership }
    }

    // --- what a profile 404 means ---
    //
    // /minecraft/profile 404 says only "no Java profile (player name) on this
    // account". A paid Java Edition that never set up its profile answers the
    // same 404, so the entitlements decide what to tell the user. Neither
    // answer is ownership proof: only the profile 200 is (no name, no uuid,
    // no online session without it). A gamertag or a working Microsoft
    // sign-in proves nothing either.

    /// The outcome of one check. Stored in accounts.json (MSAAccount.javaCheck)
    /// as the raw value; an unknown or missing value reads as nil.
    enum JavaCheck: String {
        case owner                    // profile 200 with a name and id
        case needsProfile             // profile 404, entitlements list Java Edition
        case notEntitled              // profile 404, entitlements 200 without Java Edition
        case serviceError             // anything else: keep what was stored before
    }

    /// Java Edition entitlement names (exact: the Bedrock items are
    /// product_minecraft_bedrock / game_minecraft_bedrock).
    static let javaEntitlements: Set<String> = ["game_minecraft", "product_minecraft"]

    /// `items` = the names in /entitlements/mcstore "items"; nil = the body had
    /// no items array (not a decision).
    static func javaEntitled(items: [String]?) -> Bool? {
        guard let items else { return nil }
        return items.contains { javaEntitlements.contains($0) }
    }

    /// `entitlementsStatus` nil = not asked (or the request never answered).
    static func classify(profileStatus: Int, profileHasIdentity: Bool,
                         entitlementsStatus: Int?, javaEntitled: Bool?) -> JavaCheck {
        if profileStatus == 200 { return profileHasIdentity ? .owner : .serviceError }
        guard profileStatus == 404, entitlementsStatus == 200, let javaEntitled else { return .serviceError }
        return javaEntitled ? .needsProfile : .notEntitled
    }

    /// Whether a stored session may be reused without asking again (besides
    /// its expiry). Owners: yes. A free result is trusted only when the
    /// entitlements said so; a 404-only record (javaCheck nil, written before
    /// the entitlements were read) and a profile still to be set up are asked
    /// again.
    static func cacheTrusted(ownsGame: Bool?, javaCheck: String?) -> Bool {
        if ownsGame ?? true { return true }
        return javaCheck == JavaCheck.notEntitled.rawValue
    }

    /// Re-checked when the window opens (as well as at boot and on Check again).
    static func recheckOnOpen(resolved: Bool, ownsGame: Bool?, javaCheck: String?) -> Bool {
        resolved && !cacheTrusted(ownsGame: ownsGame, javaCheck: javaCheck)
    }

    /// nil means every check passed.
    static func selfTest() -> String? {
        if let err = javaCheckSelfTest() { return err }
        let owner = Account(resolved: true, ownsGame: true)
        let legacy = Account(resolved: true, ownsGame: nil)
        let free = Account(resolved: true, ownsGame: false)
        let pending = Account(resolved: false, ownsGame: nil)
        let checks: [(String, Bool, Bool)] = [
            ("empty store stays locked", unlocked(stored: false, accounts: []), false),
            ("unresolved token does not unlock", unlocked(stored: false, accounts: [pending]), false),
            ("free account does not unlock", unlocked(stored: false, accounts: [free]), false),
            ("profile-200 account unlocks", unlocked(stored: false, accounts: [owner]), true),
            ("legacy resolved account unlocks", unlocked(stored: false, accounts: [legacy]), true),
            ("owner among free accounts unlocks", unlocked(stored: false, accounts: [free, pending, owner]), true),
            ("stored flag survives an empty list", unlocked(stored: true, accounts: []), true),
            ("stored flag survives free accounts", unlocked(stored: true, accounts: [free]), true),
            ("stored flag survives a pending token", unlocked(stored: true, accounts: [pending]), true),
        ]
        for (name, got, want) in checks where got != want {
            return "\(name): got \(got), want \(want)"
        }
        return nil
    }

    /// Network-free fixtures for the profile/entitlement split.
    private static func javaCheckSelfTest() -> String? {
        let paid = javaEntitled(items: ["product_minecraft", "game_minecraft"])
        let bedrock = javaEntitled(items: ["product_minecraft_bedrock", "game_minecraft_bedrock"])
        let none = javaEntitled(items: [])
        let missing = javaEntitled(items: nil)
        let checks: [(String, JavaCheck, JavaCheck)] = [
            ("profile 200 is the owner", classify(profileStatus: 200, profileHasIdentity: true,
                                                  entitlementsStatus: nil, javaEntitled: nil), .owner),
            ("profile 200 without name/id is not proof", classify(profileStatus: 200, profileHasIdentity: false,
                                                                  entitlementsStatus: nil, javaEntitled: nil), .serviceError),
            ("404 + Java entitlement needs a profile", classify(profileStatus: 404, profileHasIdentity: false,
                                                                entitlementsStatus: 200, javaEntitled: paid), .needsProfile),
            ("404 + Bedrock only is not entitled", classify(profileStatus: 404, profileHasIdentity: false,
                                                            entitlementsStatus: 200, javaEntitled: bedrock), .notEntitled),
            ("404 + no items is not entitled", classify(profileStatus: 404, profileHasIdentity: false,
                                                        entitlementsStatus: 200, javaEntitled: none), .notEntitled),
            ("404 + body without items is a service error", classify(profileStatus: 404, profileHasIdentity: false,
                                                                     entitlementsStatus: 200, javaEntitled: missing), .serviceError),
            ("404 + entitlements unanswered is a service error", classify(profileStatus: 404, profileHasIdentity: false,
                                                                          entitlementsStatus: nil, javaEntitled: nil), .serviceError),
            ("404 + entitlements 403", classify(profileStatus: 404, profileHasIdentity: false,
                                                entitlementsStatus: 403, javaEntitled: nil), .serviceError),
            ("404 + entitlements 429", classify(profileStatus: 404, profileHasIdentity: false,
                                                entitlementsStatus: 429, javaEntitled: nil), .serviceError),
            ("404 + entitlements 503", classify(profileStatus: 404, profileHasIdentity: false,
                                                entitlementsStatus: 503, javaEntitled: paid), .serviceError),
            ("profile 403", classify(profileStatus: 403, profileHasIdentity: false,
                                     entitlementsStatus: 200, javaEntitled: paid), .serviceError),
            ("profile 500", classify(profileStatus: 500, profileHasIdentity: false,
                                     entitlementsStatus: nil, javaEntitled: nil), .serviceError),
            ("no answer at all", classify(profileStatus: 0, profileHasIdentity: false,
                                          entitlementsStatus: nil, javaEntitled: nil), .serviceError),
        ]
        for (name, got, want) in checks where got != want {
            return "\(name): got \(got), want \(want)"
        }
        // A Java profile still to be set up is stored ownsGame=false: it must
        // not unlock, and it must not be cached as a final answer.
        let needsProfile = Account(resolved: true, ownsGame: false)
        let bools: [(String, Bool, Bool)] = [
            ("profile setup does not unlock", unlocked(stored: false, accounts: [needsProfile]), false),
            ("owner cache trusted", cacheTrusted(ownsGame: true, javaCheck: JavaCheck.owner.rawValue), true),
            ("legacy owner cache trusted", cacheTrusted(ownsGame: nil, javaCheck: nil), true),
            ("verified free cache trusted", cacheTrusted(ownsGame: false, javaCheck: JavaCheck.notEntitled.rawValue), true),
            ("404-only free record re-checked", cacheTrusted(ownsGame: false, javaCheck: nil), false),
            ("profile setup re-checked", cacheTrusted(ownsGame: false, javaCheck: JavaCheck.needsProfile.rawValue), false),
            ("unknown stored value re-checked", cacheTrusted(ownsGame: false, javaCheck: "later-build"), false),
            ("re-check on open: 404-only free", recheckOnOpen(resolved: true, ownsGame: false, javaCheck: nil), true),
            ("no re-check on open: verified free", recheckOnOpen(resolved: true, ownsGame: false,
                                                                  javaCheck: JavaCheck.notEntitled.rawValue), false),
            ("no re-check on open: owner", recheckOnOpen(resolved: true, ownsGame: true, javaCheck: nil), false),
            ("no re-check on open: unresolved (resolvePending's job)", recheckOnOpen(resolved: false, ownsGame: nil,
                                                                                      javaCheck: nil), false),
        ]
        for (name, got, want) in bools where got != want {
            return "\(name): got \(got), want \(want)"
        }
        return nil
    }
}

#if OWNERSHIP_GATE_TEST
@main
struct OwnershipGateTestMain {
    static func main() {
        if let err = OwnershipGateLogic.selfTest() {
            fputs("FAIL \(err)\n", stderr)
            exit(1)
        }
        print("ok")
    }
}
#endif
