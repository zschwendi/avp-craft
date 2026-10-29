# filter-config.py <traced-dir> <out-dir> <extra-in> <extra-out>: T3 trim of native-image-agent output.
# Drops JNA / OSHI / java-objc-bridge / NarratorMac reflection, JNI and proxy entries, and every embedded native
# library resource (LWJGL natives now come from -Dorg.lwjgl.librarypath; JNA, jcocoa, velocity and Simple Voice
# Chat natives are not shipped: velocity and SVC fall back to Java).
import json, os, re, shutil, sys
src, dst, extra_in, extra_out = sys.argv[1:5]
DROP = re.compile(r'com\.sun\.jna|(^|[\'" ])oshi\.|ca\.weblite|text2speech\.NarratorMac|voicechat\.macos')
RES_DROP = re.compile(r'\.dylib|\.jnilib|\.so\b|com/sun/jna|darwin-aarch64|macos/arm64|natives/|velocity|oshi\.')
FFM = re.compile(r'java\.lang\.foreign\.')
os.makedirs(dst, exist_ok=True)
for f in os.listdir(src):
    p = os.path.join(src, f)
    if os.path.isdir(p):
        shutil.copytree(p, os.path.join(dst, f), dirs_exist_ok=True); continue
    d = json.load(open(p)); n0 = None
    if f == 'reachability-metadata.json':
        # GraalVM 25 agent (26.2): one file; JNI = reflection entries with "jniAccessible", proxies = {"type":{"proxy":..}}.
        # Also drops java.lang.foreign: netty 4.2.15's CleanerJava24Linker would otherwise get Linker reflection + two
        # malloc/free downcall stubs; without them it falls back to its Unsafe cleaner. LWJGL is the :unsafe JNI core.
        r = d.get('reflection', []); n0 = len(r)
        d['reflection'] = [e for e in r if not DROP.search(json.dumps(e.get('type'))) and not FFM.search(json.dumps(e.get('type')))]
        res = d.get('resources', [])
        d['resources'] = [i for i in res if not RES_DROP.search(json.dumps(i))]
        print(' dropped resources:', [json.dumps(i) for i in res if RES_DROP.search(json.dumps(i))])
        print(' dropped foreign:', json.dumps(d.pop('foreign', None)))
        # Target_MethodHandleProxies (subst-26.2): Fabric's "Class::method" entrypoints become reflect Proxies, whose
        # classes must exist at build time. JDK 25's agent sees hidden classes there, not proxies, so add them here.
        for i in ('net.fabricmc.api.ModInitializer', 'net.fabricmc.api.ClientModInitializer'):
            if {'type': {'proxy': [i]}} not in d['reflection']: d['reflection'].append({'type': {'proxy': [i]}})
        json.dump(d, open(os.path.join(dst, f), 'w'), indent=1)
        print(f'{f}: reflection {n0} -> {len(d["reflection"])}, resources {len(res)} -> {len(d["resources"])}')
        continue
    if f in ('reflect-config.json', 'jni-config.json'):
        n0 = len(d); d = [e for e in d if not DROP.search(e['name'])]
    elif f == 'proxy-config.json':
        n0 = len(d); d = [e for e in d if not DROP.search(json.dumps(e))]
    elif f == 'resource-config.json':
        inc = d['resources']['includes']; n0 = len(inc)
        d['resources']['includes'] = [i for i in inc if not RES_DROP.search(i['pattern'])]
        print(' dropped resources:', [i['pattern'] for i in inc if RES_DROP.search(i['pattern'])])
    json.dump(d, open(os.path.join(dst, f), 'w'), indent=1)
    if n0 is not None:
        n1 = len(d['resources']['includes']) if f == 'resource-config.json' else len(d)
        print(f'{f}: {n0} -> {n1}')
e = json.load(open(extra_in)); inc = e['resources']['includes']
e['resources']['includes'] = [i for i in inc if not RES_DROP.search(i['pattern'])]
# Fabric's FormattedException needs its message bundle, or every launch failure is masked by MissingResourceException
e['bundles'] = [{'name': 'net.fabricmc.loader.Messages'}]
os.makedirs(os.path.dirname(extra_out), exist_ok=True); json.dump(e, open(extra_out, 'w'), indent=1)
print('extra:', len(inc), '->', len(e['resources']['includes']))
