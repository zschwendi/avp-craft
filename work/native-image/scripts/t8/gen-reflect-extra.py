#!/usr/bin/env python3
# gen-reflect-extra.py -> config/t23-extra/reflect-config.json (vivecraft round "qc 04", 2026-10-12)
# Static reflection metadata for paths the JVM tracing runs never reached (title -> world only). Reads the native-image
# classpath (t4-ni.cp) through cpscan.py, so it always matches the bytes the image is built from. Hand-kept entries
# (MANUAL below) are merged in. Re-run after any re-bake:  python3 -I scripts/t8/gen-reflect-extra.py
#  1. Vivecraft screens: GuiVROptionsBase opens every sub-screen with layout.getScreen().getConstructor(Screen.class)
#     .newInstance(this) -> every org.vivecraft Screen subclass gets all its constructors (launch 3: Standing /
#     Roomscale / HUD settings "error setting screen: NoSuchMethodException ...<init>(net.minecraft.class_437)").
#  2. Vivecraft enums: OptionEnum.getValues() = getClass().getMethod("values"); VRSettings load/save =
#     type.getMethod("valueOf", String.class) -> values() + valueOf(String) on every org.vivecraft enum (the trimmed
#     trace had valueOf on 20 of them and values() on none: cycling an enum VR option threw).
#  3. Vivecraft roomscale interact: InteractTracker/BlockInteractionModule ask every registered Block class (and its
#     superclass) for getDeclaredMethod(<useWithoutItem = intermediary method_55766>, BlockState, Level, BlockPos,
#     Player, BlockHitResult); unregistered = "not right-clickable" -> doors/levers/buttons ignored the hand.
#     Every class that DECLARES that method gets it as a queried method.
#  4. Every enum on the classpath with a public static valueOf/values queried by name elsewhere is NOT added (too
#     broad); only the mod-config enums (Cloth/AutoConfig, voicechat, Sodium options) used by config screens are.
import json, os, subprocess, sys
W = str(__import__('pathlib').Path(__file__).resolve().parents[2])
S = os.path.dirname(os.path.abspath(__file__))
# MCV=<version> picks the instance (as scripts/inst.sh). Unset means 26.2.
# 1.21.5 = $W (writes config/t23-extra). Anything else = $W/inst-<v>
# (reads inst-<v>/t4-ni.cp, writes inst-<v>/config/t23-extra/reflect-config.json).
MCV = os.environ.get('MCV', '26.2')
IR = W if MCV == '1.21.5' else W + '/inst-' + MCV
IDX = os.environ.get('IDX', W + '/out/t8/cpindex.json' if MCV == '1.21.5' else IR + '/out/cpindex.json')
if not os.path.exists(IDX) or os.environ.get('RESCAN'):
    os.makedirs(os.path.dirname(IDX), exist_ok=True)
    # CP=<cpfile> overrides the classpath scanned (M3C: 26.2's baked.cp before the natives agent's t4-ni.cp exists)
    subprocess.check_call([sys.executable, '-I', S + '/cpscan.py', os.environ.get('CP', IR + '/t4-ni.cp'), IDX])
I = json.load(open(IDX))
# Names by version (M3 stage C): 1.21.x = intermediary; 26.x is unobfuscated (Mojang names everywhere, Vivecraft 26.2
# itself asks for getDeclaredMethod("useWithoutItem", BlockState, Level, BlockPos, Player, BlockHitResult)).
MOJ = not MCV.startswith('1.')
SCREEN = 'net/minecraft/client/gui/screens/Screen' if MOJ else 'net/minecraft/class_437'
BLOCKBEH = 'net/minecraft/world/level/block/state/BlockBehaviour' if MOJ else 'net/minecraft/class_4970'
def supers(n):
    out = []
    while n in I:
        n = I[n]['sup']; out.append(n)
    return out
dot = lambda n: n.replace('/', '.')
def ptypes(desc):
    ps, i, a = [], 1, ''
    while desc[i] != ')':
        c = desc[i]
        if c == '[': a += '[]'; i += 1; continue
        if c == 'L':
            j = desc.index(';', i); t = dot(desc[i + 1:j]); i = j + 1
        else:
            t = {'Z': 'boolean', 'B': 'byte', 'C': 'char', 'S': 'short', 'I': 'int', 'J': 'long', 'F': 'float', 'D': 'double'}[c]; i += 1
        ps.append(t + a); a = ''
    return ps
E = {}
def entry(n):
    return E.setdefault(dot(n), {'name': dot(n)})
def add_method(n, key, name, params):
    e = entry(n); l = e.setdefault(key, [])
    m = {'name': name, 'parameterTypes': params}
    if m not in l: l.append(m)
counts = {}
# 1. screens
for n, d in I.items():
    if n.startswith('org/vivecraft/') and SCREEN in supers(n):
        e = entry(n); e['allDeclaredConstructors'] = True; e['allPublicConstructors'] = True
        counts['screens'] = counts.get('screens', 0) + 1
# 2. enums
MOD_ENUM_PREFIXES = ('org/vivecraft/', 'de/maxhenkel/voicechat/', 'me/shedaniel/', 'net/caffeinemc/mods/sodium/client/gui/',
                     'ca/fxco/moreculling/config/', 'dev/tr7zw/entityculling/', 'fabric/me/thosea/badoptimizations/config/')
for n, d in I.items():
    if d['sup'] == 'java/lang/Enum' and n.startswith(MOD_ENUM_PREFIXES):
        meths = {(m[0], m[1]) for m in d['methods']}
        for name, desc in (('values', None), ('valueOf', '(Ljava/lang/String;)L' + n + ';')):
            if desc is None:
                desc = '()[L' + n + ';'
            if (name, desc) in meths:
                add_method(n, 'methods', name, ptypes(desc))
        # Gson reads enums through getDeclaredFields() (EnumTypeAdapter: isEnumConstant + @SerializedName); with no
        # field metadata every constant deserialises to null. QC 1.3.7 (1.21.11) ControllerMapping.getMapping Gson-reads
        # input/mappings/*.json into Map<String, ActionType> -> null ActionType -> NPE in MCOpenXR.createAction ->
        # "OpenXR init failed" (sim round, launch 8). Every org.vivecraft enum gets its fields.
        if n.startswith('org/vivecraft/'):
            entry(n)['allDeclaredFields'] = True
            counts['enum-fields'] = counts.get('enum-fields', 0) + 1
        counts['enums'] = counts.get('enums', 0) + 1
# 3. useWithoutItem declarers
USE = ('method_55766', '(Lnet/minecraft/class_2680;Lnet/minecraft/class_1937;Lnet/minecraft/class_2338;'
       'Lnet/minecraft/class_1657;Lnet/minecraft/class_3965;)Lnet/minecraft/class_1269;')
if MOJ:
    USE = ('useWithoutItem', '(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/Level;'
           'Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/entity/player/Player;Lnet/minecraft/world/phys/BlockHitResult;)'
           'Lnet/minecraft/world/InteractionResult;')
for n, d in I.items():
    if any((m[0], m[1]) == USE for m in d['methods']) and (BLOCKBEH in supers(n) or n == BLOCKBEH):
        add_method(n, 'queriedMethods', USE[0], ptypes(USE[1]))
        counts['useWithoutItem'] = counts.get('useWithoutItem', 0) + 1
# 5. Gson-read types in authlib / Minecraft (qc 04b): an MSA session runs authlib's fetchProperties
#    (UserAttributesResponse, a record) -> "UnsupportedFeatureError: Record components not available" made
#    class_310.method_55607's join() throw on every world join / Multiplayer / chat screen. Every record and every class
#    in authlib's request/response/minecraft.client/minecraft.report/properties packages, plus every class anywhere under
#    com/mojang/ or net/minecraft/ that carries a Gson annotation (@SerializedName/@Expose/...), gets all declared
#    constructors/fields/methods + record components.
GSON_PKGS = ('com/mojang/authlib/yggdrasil/request/', 'com/mojang/authlib/yggdrasil/response/',
             'com/mojang/authlib/minecraft/client/', 'com/mojang/authlib/minecraft/report/', 'com/mojang/authlib/properties/')
for n, d in I.items():
    rec = d['sup'] == 'java/lang/Record'
    if (n.startswith('com/mojang/authlib/') and rec) or n.startswith(GSON_PKGS) or \
       (d.get('gson') and n.startswith(('com/mojang/', 'net/minecraft/'))):
        e = entry(n)
        for k in ('allDeclaredConstructors', 'allDeclaredFields', 'allDeclaredMethods'): e[k] = True
        if rec: e['allRecordComponents'] = True
        counts['gson'] = counts.get('gson', 0) + 1
# 6. (M3 stage A, QC Vivecraft 1.3.7 / 1.21.11) Vivecraft's JSON binding profiles: BindingProfile.getDefaultBinding /
#    getAllProfiles Gson-read input/profiles/*.json into records (BindingProfile, ActionSet, ControllerPaths, ...) ->
#    same record treatment as rule 5. No such records exist on QC 1.2.5 (1.21.5), so this adds nothing there.
for n, d in I.items():
    if n.startswith('org/vivecraft/client_vr/provider/control/') and (d['sup'] == 'java/lang/Record' or d.get('gson')):
        e = entry(n)
        for k in ('allDeclaredConstructors', 'allDeclaredFields', 'allDeclaredMethods'): e[k] = True
        if d['sup'] == 'java/lang/Record': e['allRecordComponents'] = True
        counts['vivecraft-records'] = counts.get('vivecraft-records', 0) + 1
# Rule 7 (scan every jar's fabric.mod.json for untraced entrypoints) was removed with the public bake.
# The default image does not add entrypoints beyond the trace and the manual list.
# manual entries (kept by hand; see the reports that added them)
MANUAL = json.load(open(S + '/reflect-extra-manual.json'))
for m in MANUAL:
    e = entry(m['name'].replace('.', '/'))
    for k, v in m.items():
        if k == 'name': continue
        if isinstance(v, list):
            l = e.setdefault(k, [])
            for x in v:
                if x not in l: l.append(x)
        else:
            e[k] = v
out = sorted(E.values(), key=lambda e: e['name'])
os.makedirs(IR + '/config/t23-extra', exist_ok=True)
json.dump(out, open(IR + '/config/t23-extra/reflect-config.json', 'w'), indent=1)
print('reflect-extra:', len(out), 'entries', counts, 'manual', len(MANUAL))
