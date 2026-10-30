# Knot bake audit: Minecraft 26.2

This audit covers the fresh Fabric/Knot capture made on October 30, 2026 (UTC). It explains why **these exact 49 failed full-pass requests can proceed to native compilation with the pinned configuration**. It does not establish that the native image compiles or that the app works on a headset.

The 49 failures are expected **only under the configuration below**. Do not accept a different failure list because its count also happens to be 49. Preserve new failures and investigate them.

## Configuration covered

The audit used the 13 top-level mods and their nested dependencies from [`mods-26.2.json`](../work/native-image/scripts/m3a/mods-26.2.json), including Fabric API `0.161.0+26.2`, Sodium `0.9.2`, Lithium `0.25.3` and the patched Vivecraft `26.2-1.3.15` JAR. The [machine-readable audit](KNOT-BAKE-AUDIT.json) records the manifest hash, all 13 versions, every failed class name, its raw class hash, and the source archive hash.

Two feature choices are part of this result:

- **Sodium is the active renderer; Indigo is off.** Sodium's metadata declares `fabric-renderer-api-v1:contains_renderer`. Indigo's mixin plugin tests that key, and its client entrypoint uses the same predicate before registering its renderer and feature renderers. The captured client reported that another rendering plugin was present and Indigo was not applied.
- **Lithium's pathing optimization is off.** The pinned Lithium JAR's `assets/lithium/lithium-mixin-config-default.properties` contains `mixin.ai.pathing=false`. The fresh runtime reported 171 options and zero overrides.

The native launcher must preserve both choices. Check its deployed config directory as well as the build profile: `config/lithium.properties` must not enable `mixin.ai.pathing` or another override that changes this bake. Enabling Indigo, enabling Lithium pathing, changing the mod set or changing its configuration requires a new bake and audit. The untransformed accessor classes discussed below are not usable substitutes for enabled features.

Before applying this audit to another capture, verify the manifest, downloaded top-level JARs and raw class inputs. Run this from the repository root; it reads the fresh build output without modifying it:

```python
from pathlib import Path
from zipfile import ZipFile
import hashlib
import json

root = Path.cwd()
audit = json.loads((root / "docs/KNOT-BAKE-AUDIT.json").read_text())
manifest = root / audit["modManifest"]["path"]
assert hashlib.sha256(manifest.read_bytes()).hexdigest() == audit["modManifest"]["sha256"]
game = root / "work/native-image/inst-26.2/fgamedir"
for mod in json.loads(manifest.read_text()):
    algorithm = "sha256" if "sha256" in mod else "sha512"
    assert hashlib.new(algorithm, (game / "mods" / mod["dest"]).read_bytes()).hexdigest() == mod[algorithm]
archives = {p.name: p for folder in [game / "mods", game / ".fabric/processedMods"] for p in folder.glob("*.jar")}
for name, expected in audit["sourceArchives"].items():
    assert hashlib.sha256(archives[name].read_bytes()).hexdigest() == expected["sha256"]
for entry in audit["classes"]:
    for name in entry["sources"]:
        with ZipFile(archives[name]) as jar:
            assert hashlib.sha256(jar.read(entry["name"] + ".class")).hexdigest() == entry["rawClassSha256"]
print("Pinned manifest, JARs and all 49 raw class definitions match.")
```

Preserve the capture log before another game launch replaces it. The audited capture contained these exact messages:

```text
Loaded configuration file for Lithium: 171 options available, 0 override(s) found.
[Indigo] Different rendering plugin detected; not applying Indigo.
```

Check both in `work/native-image/inst-26.2/fgamedir/logs/latest.log` from **your bake**, and confirm no active Lithium override in the build or deployed config. A missing message, changed source hash or enabled feature needs investigation; it is not covered by matching the failure count.

## What the capture verified

The actual client created a disposable world, loaded a player, integrated server and a 3×3 area of chunks, and advanced at least 100 game ticks. During capture it reached that state in 22.44 seconds, before the full pass began at 300 seconds. It subsequently used the normal save/quit path and exited with code 0.

The bake's final summary was:

```text
[BAKE] defined-at-runtime=14878 fullpass-transformed=34513 already=14833 skipped-mixin-classes=1055 null=0 failed=49 other-loader-classes=0
```

The audit inspected 49,404 captured classes before pruning. It recovered the raw definitions of all 49 failures from the recorded Knot classpath, checked their ASM metadata, and scanned transformed class hierarchies, descriptors, bytecode instructions, bootstrap arguments, constants, frames, catch types and annotations for references. A separate constant-pool scan agreed on the three referenced failed names.

## Why the requests failed without leaving enabled dependencies missing

A bake walks more classes than normal gameplay loads. Its full pass asks Knot for original mixin templates, their local classes and helper containers as well as game classes. Original mixin names can be unnecessary after their methods and generated inner classes have been incorporated into target classes. That distinction must be checked against the captured output; a mixin-looking name alone is not grounds to ignore a failure.

| Classification | Count | Captured reference evidence |
| --- | ---: | --- |
| Interface mixin templates | 16 | No executable reference to the original name; any retained name is a Mixin provenance annotation string. |
| Original anonymous/local classes inside mixin namespaces | 25 | No reference to the original name remains; required generated references resolve. |
| Lithium mixin namespace outer/helper containers | 5 | No reference to the original name remains. |
| Disabled Indigo accessor interfaces | 2 | Calls remain inside the disabled Indigo implementation. |
| Disabled Lithium pathing accessor | 1 | A cast and call remain inside the disabled pathing implementation. |

The five Lithium containers are non-annotated classes. Four contain only constructors; `AxisCycleDirectionMixin` also contains an assertion initializer. None has a remaining transformed reference under its original name.

The 46 names in the first three categories therefore have no remaining executable/type/descriptor/frame dependency under the failed name. Across the complete set, 22 retained references are only string values in Mixin provenance annotations. They are not class literals or reflective-load instructions.

### Three affected caller classes

These are actual instructions, not unused constant-pool entries:

| Captured caller | Failed interface and remaining operation | Required condition |
| --- | --- | --- |
| `AltModelBlockRendererImpl` | `BlockModelLighterAccessor.fabric_getCACHE()` | Indigo remains unregistered and unapplied. |
| `ExtendedItemFeatureRenderer` | `ItemFeatureRendererAccessor.fabric_computeFoilDecalPose(...)` and `fabric_useTransparentGlint(...)` | Indigo remains unregistered and unapplied. |
| `PathNodeCache` | Cast to `PathfindingContextAccessor`, then `getLastNodePos()` | Lithium `mixin.ai.pathing` remains false. |

Both raw Indigo accessor interfaces contain static methods that throw `AssertionError`; using those raw methods would fail. Lithium's accessor is abstract, but the captured `PathfindingContext` does not implement it, consistent with the optimization being disabled. The evidence for proceeding is the matching disabled-feature configuration, not the presence of raw class files on the fallback classpath.

The JSON records full package names, calling methods and operations for these five instruction sites (four method calls and one cast).

## Generated-class closure

The bake recovered **13 additional generated classes**. Its failure list contained no `GEN-FAIL` or `GEN-NULL` entries. Scanning all captured references against the overlay and recorded classpath found **zero unresolved generated references**.

There were 31 stored names matching the bake's generated-name patterns, including 20 anonymous relocations. This total also includes preexisting generated helper definitions; it does not mean 31 classes were newly synthesized. Original inner-class names and relocated generated names must not be treated interchangeably.

For another capture, the following read-only check repeats generated-name closure over class constants and type descriptors in the remaining overlay. The recorded Knot classpath supplies library definitions, so it also works after the documented pruning step. Run from the repository root:

```python
from pathlib import Path
from zipfile import ZipFile
import re
import struct

instance = Path("work/native-image/inst-26.2")
overlay = instance / "bake/classes"
files = list(overlay.rglob("*.class"))
available = {p.relative_to(overlay).as_posix()[:-6] for p in files}
for entry in (instance / "bake/knot-classpath.txt").read_text().splitlines():
    path = Path(entry)
    if path.is_dir():
        available.update(p.relative_to(path).as_posix()[:-6] for p in path.rglob("*.class"))
    else:
        with ZipFile(path) as jar:
            available.update(n[:-6] for n in jar.namelist() if n.endswith(".class"))

def generated(name):
    return any(part in name for part in ("$Anonymous$", "/generated/", "synthetic/args"))

def references(data):
    assert data[:4] == b"\xca\xfe\xba\xbe"
    count = struct.unpack_from(">H", data, 8)[0]
    strings, class_indices = {}, []
    offset, index = 10, 1
    sizes = {3: 4, 4: 4, 5: 8, 6: 8, 8: 2, 9: 4, 10: 4, 11: 4,
             12: 4, 15: 3, 16: 2, 17: 4, 18: 4, 19: 2, 20: 2}
    while index < count:
        tag = data[offset]
        offset += 1
        if tag == 1:
            length = struct.unpack_from(">H", data, offset)[0]
            offset += 2
            strings[index] = data[offset:offset + length].decode("utf-8", errors="replace")
            offset += length
        elif tag == 7:
            class_indices.append(struct.unpack_from(">H", data, offset)[0])
            offset += 2
        else:
            offset += sizes[tag]
            if tag in (5, 6):
                index += 1
        index += 1
    names = {strings[index] for index in class_indices if not strings[index].startswith("[")}
    for value in strings.values():
        names.update(re.findall(r"L([A-Za-z0-9_/$]+)[;<]", value))
    return {name for name in names if generated(name)}

required = set().union(*(references(path.read_bytes()) for path in files))
missing = sorted(required - available)
assert not missing, missing
print("No unresolved generated names in overlay constants or type descriptors.")
```

Also require no `GEN-FAIL` or `GEN-NULL` lines in `fullpass-failures.txt`; the names checker below rejects any `GEN-` line. This closure check is a diagnostic, not a substitute for redoing the broader ASM/reference and feature-guard audit when inputs change.

## Exact failure list

The sorted internal names appear in the JSON's `classes` array with per-class classifications and SHA-256 hashes. Their normalized names-only SHA-256 is:

```text
0badbd5737a24f5cc3cc901332b82cd1b487bd264f140d9ff94818e68850c8d3
```

Normalization is: take the class name before ` :: ` on each failure line, require unique names, sort by Unicode code point (Python `sorted` on `str`), encode as UTF-8 with one name per line and a final LF. Exception wording and timestamps are excluded.

From the repository root, this checks a new capture's names against the audited list without changing the capture:

```python
from pathlib import Path
import hashlib
import json

expected = json.loads(Path("docs/KNOT-BAKE-AUDIT.json").read_text())
lines = Path("work/native-image/inst-26.2/bake/fullpass-failures.txt").read_text().splitlines()
assert all(" :: " in line and not line.startswith("GEN-") for line in lines)
names = [line.split(" :: ", 1)[0] for line in lines]
assert len(names) == len(set(names)) == expected["failureNames"]["count"]
names = sorted(names)
assert names == [entry["name"] for entry in expected["classes"]]
digest = hashlib.sha256(("\n".join(names) + "\n").encode("utf-8")).hexdigest()
assert digest == expected["failureNames"]["sha256"]
print("Failure names match; verify pinned inputs, feature guards and generated closure separately.")
```

A names-only match does not verify the configuration, raw bytes, generated closure or runtime behavior. New names, changed source hashes, active feature overrides, a different renderer, `GEN-FAIL`/`GEN-NULL`, or unresolved generated references require investigation before proceeding. This static audit also cannot prove arbitrary dynamically constructed reflective names.

Continue with [BUILDING.md](../BUILDING.md) only after checking those conditions. Native compilation, app installation and interactive headset validation have their own acceptance steps.
