#!/usr/bin/env python3
"""Source contract for the head HUD pitch callsite and the four new-profile defaults.

Checks the real GuiHandler.extractGui branch structure, that the pitch is applied once, only in the
head-locked HUD branch, after guipos is computed, gated on "no screen" and "not seated alternate aim",
and never in the controller-attached, room/menu or menu-room branches. Checks the exact four
VRSettings defaults with their annotations. When an original tree is available, also checks that
GuiHandler only gained one inserted hunk and VRSettings changed exactly the four default lines.

usage: python3 -I tests/hud_pitch_source_contract.py [CANDIDATE_ROOT [ORIGINAL_ROOT]]

CANDIDATE_ROOT is a Vivecraft repo root (containing common/src/main/java); defaults to this
workspace's candidate/. ORIGINAL_ROOT defaults to this workspace's original/ and the diff checks
are skipped (reported) if it does not exist. Standard library only, bounded input sizes.
"""

import difflib
import re
import sys
from pathlib import Path

MAX_BYTES = 2_000_000
JAVA = Path("common/src/main/java/org/vivecraft/client_vr")
GUI_HANDLER = JAVA / "gameplay/screenhandlers/GuiHandler.java"
HELPER = JAVA / "gameplay/screenhandlers/HeadHudPitch.java"
VR_SETTINGS = JAVA / "settings/VRSettings.java"

WORKSPACE = Path(__file__).resolve().parent.parent

failures = []
passed = 0


def check(condition, why):
    global passed
    if condition:
        passed += 1
    else:
        failures.append(why)
    return condition


def read(root, rel):
    path = root / rel
    if not path.is_file():
        raise SystemExit(f"FAIL missing file {path}")
    if path.stat().st_size > MAX_BYTES:
        raise SystemExit(f"FAIL file too large {path}")
    return path.read_text(encoding="utf-8")


def mask(src):
    """blanks comments and string/char literal contents, keeping offsets and newlines"""
    out = list(src)
    n = len(src)
    i = 0

    def blank(a, b):
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif src.startswith('"""', i):
            j = src.find('"""', i + 3)
            j = n if j < 0 else j
            blank(i + 3, j)
            i = j + 3
        elif src[i] in "\"'":
            quote = src[i]
            j = i + 1
            while j < n and src[j] != quote and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            blank(i + 1, j)
            i = j + 1
        else:
            i += 1
    return "".join(out)


def match(code, open_index, opening, closing):
    depth = 0
    for k in range(open_index, len(code)):
        if code[k] == opening:
            depth += 1
        elif code[k] == closing:
            depth -= 1
            if depth == 0:
                return k
    raise SystemExit(f"FAIL unbalanced {opening}{closing} at offset {open_index}")


def block_at(code, start):
    """(open, close) of the first {...} block at or after start"""
    open_index = code.index("{", start)
    return open_index, match(code, open_index, "{", "}")


def find_once(code, needle, lo, hi, why):
    first = code.find(needle, lo, hi)
    check(first >= 0, f"{why}: missing `{needle}`")
    if first >= 0:
        check(code.find(needle, first + 1, hi) < 0, f"{why}: `{needle}` appears more than once")
    return first


def norm(text):
    return " ".join(text.split())


def split_args(text):
    args, depth, current = [], 0, []
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(norm("".join(current)))
            current = []
        else:
            current.append(ch)
    args.append(norm("".join(current)))
    return args


def check_gui_handler(src):
    code = mask(src)
    tag = "GuiHandler"

    check("org.objectweb.asm" not in code, f"{tag}: no ASM")
    method = find_once(code, "public static void extractGui(ScreenRenderState screenState) {", 0, len(code),
                       f"{tag}: extractGui")
    if method < 0:
        return
    body_open, body_close = block_at(code, method)

    def once(needle, lo=body_open, hi=body_close, why=f"{tag}.extractGui"):
        return find_once(code, needle, lo, hi, why)

    # enclosing branch conditions, verbatim from upstream
    once("boolean forceGuiToHUD = DH.vrSettings.forceGuiToHUD && MC.level != null && !MethodHolder.isInMenuRoom();")
    hud_if = once("if (GUI_POS_ROOM == null || forceGuiToHUD) {")
    level_if = once("if (MC.level != null && (MC.gui.screen() == null || forceGuiToHUD || !DH.vrSettings.floatInventory)) {")
    head_if = once("if (DH.vrSettings.seated || DH.vrSettings.vrHudLockMode == VRSettings.HUDLock.HEAD) {")
    menu_room_if = once("if ((DH.vrSettings.seated || DH.vrSettings.menuAlwaysFollowFace) && MethodHolder.isInMenuRoom()) {")
    if min(hud_if, level_if, head_if, menu_room_if) < 0:
        return

    hud_open, hud_close = block_at(code, hud_if)
    level_open, level_close = block_at(code, level_if)
    head_open, head_close = block_at(code, head_if)
    check(hud_open < level_open and level_close < hud_close, f"{tag}: level/screen gate nested in HUD branch")
    check(level_open < head_open and head_close < level_close, f"{tag}: head branch nested in level/screen gate")

    # controller attached HUD: the else of the head branch
    after_head = code[head_close + 1:head_close + 40]
    check(norm(after_head).startswith("else {"), f"{tag}: head branch is followed by the controller else")
    ctrl_open, ctrl_close = block_at(code, head_close + 1)
    check("// attach to controller" in src[ctrl_open:ctrl_open + 80], f"{tag}: else branch is the controller HUD")

    # room/menu branch: the else of the HUD branch
    check(norm(code[hud_close + 1:hud_close + 40]).startswith("else {"), f"{tag}: HUD branch has the room else")
    room_open, room_close = block_at(code, hud_close + 1)
    force_if = once("if (forceGuiToHUD) {", level_open, level_close)
    menu_open, menu_close = block_at(code, menu_room_if)

    # head branch contents the pitch depends on
    position_decl = once("Vec3 position = DH.vrPlayer.vrdata_world_render.hmd.getPosition();", head_open, head_close)
    fresh = once("guirot = new Matrix4f().rotationY(DH.vrPlayer.vrdata_world_render.rotation_radians);",
                 head_open, head_close)
    alt_if = once("if (DH.vrSettings.seated && DH.vrSettings.seatedHudAltMode) {", head_open, head_close)
    guipos_at = once("guipos = new Vec3(", head_open, head_close)
    scale_at = once("scale = DH.vrSettings.hudScale;", head_open, head_close)
    if min(position_decl, fresh, alt_if, guipos_at, scale_at) < 0:
        return
    alt_close = block_at(code, alt_if)[1]
    alt_else_open, alt_else_close = block_at(code, alt_close + 1)
    guipos_paren = code.index("(", guipos_at)
    guipos_end = code.index(";", match(code, guipos_paren, "(", ")"))

    # the call: exactly once in the whole file, only in the head branch, after guipos
    check(len(re.findall(r"\bpitchInGameHud\s*\(", code)) == 1, f"{tag}: pitchInGameHud called exactly once")
    check(len(re.findall(r"\bhudPitchOffset\b", code)) == 1, f"{tag}: hudPitchOffset read exactly once")
    check(not re.search(r"\bHeadHudPitch\s*\.\s*pitch\s*\(", code), f"{tag}: ungated pitch() not used")
    call = re.search(r"HeadHudPitch\.Placement\s+(\w+)\s*=\s*HeadHudPitch\.pitchInGameHud\s*\(", code)
    if not check(call is not None, f"{tag}: pitchInGameHud result assigned to a Placement"):
        return
    var = call.group(1)
    call_at = call.start()
    check(head_open < call_at < head_close, f"{tag}: call inside the head-locked HUD branch")
    check(call_at > guipos_end, f"{tag}: call after the guipos computation")
    check(call_at > alt_else_close, f"{tag}: call after guirot got its eye/aim rotation")
    check(position_decl < call_at and fresh < call_at, f"{tag}: call after hmd position and fresh guirot")
    for name, (lo, hi) in {"controller HUD": (ctrl_open, ctrl_close), "room/menu screen": (room_open, room_close),
                           "menu room": (menu_open, menu_close), "forceGuiToHUD write back": (force_if, level_close)}.items():
        check(not (lo <= call_at <= hi), f"{tag}: call not in the {name} branch")
        check("HeadHudPitch" not in code[lo:hi] and "hudPitchOffset" not in code[lo:hi],
              f"{tag}: no pitch in the {name} branch")
    check(call_at < force_if, f"{tag}: forceGuiToHUD write back sees the same placement that is rendered")

    paren = code.index("(", call.end() - 1)
    args = split_args(code[paren + 1:match(code, paren, "(", ")")])
    check(args == ["MC.gui.screen() != null",
                   "DH.vrSettings.seated && DH.vrSettings.seatedHudAltMode",
                   "position", "guipos", "guirot",
                   "DH.vrSettings.hudPitchOffset"], f"{tag}: exact call arguments, got {args}")

    use_if = re.compile(r"if\s*\(\s*" + re.escape(var) + r"\s*!=\s*null\s*\)\s*\{").search(code, call.end())
    if check(use_if is not None and use_if.start() < head_close, f"{tag}: null check on `{var}` in head branch"):
        use_open, use_close = block_at(code, use_if.start())
        statements = [norm(s) for s in code[use_open + 1:use_close].split(";") if s.strip()]
        check(statements == [f"guipos = {var}.position()", f"guirot = {var}.rotation()"],
              f"{tag}: pitched placement only replaces guipos and guirot, got {statements}")
        check(use_close < scale_at, f"{tag}: pitch applied before the HUD scale, scale untouched")
    check(len(re.findall(r"\b" + re.escape(var) + r"\b", code)) == 4, f"{tag}: `{var}` used only locally")


def check_helper(src):
    code = mask(src)
    tag = "HeadHudPitch"
    check(code.startswith("package org.vivecraft.client_vr.gameplay.screenhandlers;"), f"{tag}: upstream package")
    check("org.objectweb.asm" not in code, f"{tag}: no ASM")
    signature = norm("public static Placement pitchInGameHud( boolean screenOpen, boolean seatedAltAim, Vec3 head, "
                     "Vec3 position, Matrix4fc rotation, float pitchDegrees)")
    check(signature in norm(code), f"{tag}: pitchInGameHud takes a read only Matrix4fc")
    gate = code.find("pitchInGameHud(")
    if check(gate >= 0, f"{tag}: has pitchInGameHud"):
        gate_open, gate_close = block_at(code, gate)
        statements = [norm(s) for s in code[gate_open + 1:gate_close].split(";") if s.strip()]
        check(statements[:1] == ["if (screenOpen || seatedAltAim) { return null"],
              f"{tag}: screen and seated alternate aim exit first, got {statements[:1]}")
    check("public static Placement pitch(Vec3 head, Vec3 position, Matrix4fc rotation, float pitchDegrees)" in code,
          f"{tag}: pitch takes a read only Matrix4fc")
    check("public record Placement(Vec3 position, Matrix4f rotation)" in code, f"{tag}: immutable placement")
    check("new Matrix4f(rotation).rotateX(" in code, f"{tag}: rotates a copy of the input")
    check(not re.search(r"\brotation\s*\.\s*(rotate|mul|set|identity|translate|scale|zero|invert)\w*\s*\(", code),
          f"{tag}: input rotation never mutated")
    check("public static final float MAX_PITCH_DEGREES = 45.0F;" in code, f"{tag}: 45 degree clamp")
    check("Float.isFinite(pitchDegrees)" in code, f"{tag}: finite angle guard")


DEFAULTS = [
    ('@SettingField(value = VrOptions.HUD_SCALE, config = "headHudScale")',
     "public float hudScale = 1.0f;", "public float hudScale = 0.8f;"),
    ("@SettingField(VrOptions.HUD_DISTANCE)",
     "public float hudDistance = 1.25f;", "public float hudDistance = 2.0f;"),
    ("@SettingField",
     "public float hudPitchOffset = -2f;", "public float hudPitchOffset = -15.0f;"),
    ("@SettingField(VrOptions.HUD_LOCK_TO)",
     "public HUDLock vrHudLockMode = HUDLock.WRIST;", "public HUDLock vrHudLockMode = HUDLock.HEAD;"),
]

UNCHANGED_SETTINGS = [
    ("@SettingField(VrOptions.SEATED_HUD_XHAIR)", "public boolean seatedHudAltMode = true;"),
    ("@SettingField(VrOptions.FORCE_GUI_TO_HUD)", "public boolean forceGuiToHUD = false;"),
    ("@SettingField(VrOptions.MENU_ALWAYS_FOLLOW_FACE)", "public boolean menuAlwaysFollowFace;"),
    ("@SettingField(VrOptions.HUD_WRIST_OFFSET)", "public float vrHudWristOffset = 1F;"),
    ("@SettingField", "public float hudYawOffset = 0.0f;"),
]


def check_settings(src):
    tag = "VRSettings"
    lines = [line.strip() for line in src.splitlines()]
    for annotation, before, after in DEFAULTS:
        check(before not in lines, f"{tag}: old default `{before}` gone")
        check(lines.count(after) == 1, f"{tag}: new default `{after}` declared once")
        if after in lines:
            index = lines.index(after)
            check(lines[index - 1] == annotation, f"{tag}: `{after}` keeps `{annotation}`")
    for annotation, field in UNCHANGED_SETTINGS:
        if check(lines.count(field) == 1, f"{tag}: `{field}` unchanged"):
            check(lines[lines.index(field) - 1] == annotation, f"{tag}: `{field}` keeps `{annotation}`")
    check(src.startswith("/**\n * Copyright 2013 Mark Browning, StellaArtois"), f"{tag}: existing header kept")


def check_config_loading(src):
    """serialized keys and missing-key fallback, proven from the real VRSettings source (not executed)"""
    tag, code, lines = "VRSettings load", mask(src), [line.strip() for line in src.splitlines()]
    check("String config = ann.config().isEmpty() ? field.getName() : ann.config();" in code,
          f"{tag}: serialized key is the config alias, else the field name")
    for key, decl in [("vrHudLockMode", "public HUDLock vrHudLockMode = HUDLock.HEAD;"),
                      ("hudDistance", "public float hudDistance = 2.0f;"),
                      ("headHudScale", "public float hudScale = 0.8f;"),
                      ("hudPitchOffset", "public float hudPitchOffset = -15.0f;")]:
        if check(decl in lines, f"{tag}: `{decl}` declared"):
            alias = re.search(r'config = "(\w+)"', lines[lines.index(decl) - 1])
            field = re.search(r"(\w+) =", decl).group(1)
            check((alias.group(1) if alias else field) == key, f"{tag}: `{decl}` serializes as `{key}`")
    check('@SettingField(value = VrOptions.VR_ENABLED, config = "vrEnabled")' in lines, f"{tag}: vrEnabled key")
    lo, hi = block_at(code, find_once(code, "public VRSettings() {", 0, len(code), tag))
    order = [code.find(s, lo, hi) for s in ("initializeFieldInfo();", "storeDefaults();", "this.loadOptions();")]
    check(-1 not in order and order == sorted(order), f"{tag}: constructor maps keys, stores defaults, then loads")
    lo, hi = block_at(code, find_once(code, "public void loadOptions() {", 0, len(code), tag))
    loop = code.find("for (Map.Entry<String, String> entry : settings.entrySet()) {", lo, hi)
    if check(loop >= 0 and "Map<String, String> settings = loadSettingsFile();" in code[lo:loop],
             f"{tag}: loadOptions iterates only the file's entries"):
        loop_open, loop_close = block_at(code, loop)
        check("if (mapping == null) continue;" in code[loop_open:loop_close], f"{tag}: unknown keys skipped")
        sets = [lo + m.start() for m in re.finditer(r"\b(field|Array)\.set\(", code[lo:hi])]
        check(bool(sets) and all(loop_open < s < loop_close for s in sets), f"{tag}: fields set only per present key")
        check(not re.search(r"fieldConfigMap\.(entrySet|values|keySet|forEach)|defaultsMap", code[lo:hi]),
              f"{tag}: loadOptions never walks all keys or resets defaults")
    lo, hi = block_at(code, find_once(code, "private Map<String, String> loadSettingsFile() {", 0, len(code), tag))
    body = norm(code[lo:hi])
    check(body.count("data.put(") == 1 and "for (String key : currentConfig.keySet()) { data.put(key, "
          "currentConfig.get(key).getAsString());" in body, f"{tag}: settings map holds only keys from the file")


def check_diffs(candidate, original):
    gui_new = read(candidate, GUI_HANDLER).splitlines()
    gui_old = read(original, GUI_HANDLER).splitlines()
    ops = [op for op in difflib.SequenceMatcher(None, gui_old, gui_new, autojunk=False).get_opcodes()
           if op[0] != "equal"]
    check(len(ops) == 1 and ops[0][0] == "insert", f"GuiHandler diff: one inserted hunk only, got {ops}")
    if ops and ops[0][0] == "insert":
        inserted = "\n".join(gui_new[ops[0][3]:ops[0][4]])
        check("pitchInGameHud" in inserted, "GuiHandler diff: the insertion is the pitch call")
        before = [line.strip() for line in gui_old[:ops[0][1]] if line.strip()]
        check(bool(before) and before[-1].endswith("DH.vrSettings.hudDistance);"),
              "GuiHandler diff: inserted right after the guipos statement")

    set_new = read(candidate, VR_SETTINGS).splitlines()
    set_old = read(original, VR_SETTINGS).splitlines()
    changed = set()
    clean = True
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, set_old, set_new, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        if tag != "replace" or i2 - i1 != j2 - j1:
            clean = False
            continue
        for a, b in zip(set_old[i1:i2], set_new[j1:j2]):
            changed.add((a.strip(), b.strip()))
            clean &= a[:len(a) - len(a.lstrip())] == b[:len(b) - len(b.lstrip())]
    check(clean, "VRSettings diff: only 1:1 line replacements with the same indentation")
    check(changed == {(before, after) for _, before, after in DEFAULTS},
          f"VRSettings diff: exactly the four defaults changed, got {sorted(changed)}")


def main(argv):
    if len(argv) > 3:
        raise SystemExit(__doc__)
    candidate = Path(argv[1]).resolve() if len(argv) > 1 else WORKSPACE / "candidate"
    original = Path(argv[2]).resolve() if len(argv) > 2 else WORKSPACE / "original"

    check_gui_handler(read(candidate, GUI_HANDLER))
    check_helper(read(candidate, HELPER))
    check_settings(read(candidate, VR_SETTINGS))
    check_config_loading(read(candidate, VR_SETTINGS))
    if (original / GUI_HANDLER).is_file() and (original / VR_SETTINGS).is_file():
        check_diffs(candidate, original)
    else:
        print(f"NOTE original tree {original} not found, diff checks skipped")

    if failures:
        for failure in failures:
            print("FAIL " + failure)
        print(f"{len(failures)} failed, {passed} passed")
        return 1
    print(f"PASS {passed} source contract checks ({candidate})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
