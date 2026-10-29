# work/natives — download provenance

Downloads live in `downloads/` (never edited); build trees in `build/` (disposable, 55 MB); outputs in
`libffi/`, `openal/`, `openxr-shim/`. Scripts in `scripts/`. Everything regenerates from the scripts.

| What | URL | sha256 | Fetched | Used by |
|---|---|---|---|---|
| libffi 3.4.8 release tarball | https://github.com/libffi/libffi/releases/download/v3.4.8/libffi-3.4.8.tar.gz | `bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b` | 2026-10-12 | `scripts/build-libffi.sh` -> `libffi/{xros,xrsimulator}/lib/libffi.a` |
| OpenAL Soft 1.25.2 (git tag archive) | https://github.com/kcat/openal-soft/archive/refs/tags/1.25.2.tar.gz | `fb27e5839aa11f0e5b9d33756965291fad5d6909ab928ea1f796f4a1a6877894` | 2026-10-12 | `scripts/build-openal.sh` -> `openal/{xros,xrsimulator}/libopenal.dylib` |
| Simple Voice Chat natives (opus4j/rnnoise4j/speex4j/lame4j + codecs via FetchContent) | see `svc/PROVENANCE.md` (git clones at pinned commits) | n/a (git) | 2026-10-12 | `scripts/build-svc.sh` -> `svc/{xros,xrsimulator,macosx}/lib*.dylib` |

No other downloads besides the SVC clones (`downloads/svc`). `openxr-shim/` is original source; its jni.h comes from the local GraalVM JDK
(`work/native-image/graalvm/graalvm-jdk-21.0.12+7.1`).

Safe to delete: `build/` always; `downloads/` if the URLs above still resolve (re-fetch with curl, check sha256).

## LWJGL 3.4.1 for Minecraft 26.2 (M3 stage C natives, 2026-10-12; recorded BEFORE fetching)
Script: `scripts/build-lwjgl341-natives.sh` (macosx) / `scripts/build-lwjgl341-natives-xros.sh` (xros, xrsimulator) ->
`build/lwjgl-3.4.1/<platform>/`. Replaces the QuestCraftPlusPlus fork (f1ec2b7) **for 26.2 only**; 1.21.5/1.21.11 keep f1ec2b7.

| What | URL | sha / sha256 | Fetched | Used by |
|---|---|---|---|---|
| LWJGL 3.4.1 source, git tag `3.4.1` (annotated tag object c46b8379…, commit **b800ccffab14396fc529ddb6c931b7c5c5226763**) shallow clone to `downloads/lwjgl3-3.4.1/` | `git clone --depth 1 -b 3.4.1 https://github.com/LWJGL/lwjgl3 downloads/lwjgl3-3.4.1` | commit b800ccff | 2026-10-12 | native C sources (core/opengl/stb/jemalloc/tinyfd) |
| LWJGL 3.4.1 Maven Central jars (`downloads/lwjgl-3.4.1-maven/`): `lwjgl`, `lwjgl:unsafe`, `lwjgl-opengl`, `lwjgl-stb`, `lwjgl-openal`, `lwjgl-openxr`, `lwjgl-glfw`, `lwjgl-jemalloc`, `lwjgl-tinyfd`, and `natives-macos-arm64` of each for export comparison | `https://repo1.maven.org/maven2/org/lwjgl/<m>/3.4.1/<m>-3.4.1[-unsafe\|-natives-macos-arm64].jar` | see `downloads/lwjgl-3.4.1-maven/SHA256SUMS` | 2026-10-12 | Java API for javac/javap, export-set diff, the `:unsafe` core jar the bake puts on the classpath |
| libffi 3.5.2 (reused, already fetched: `work/native-image/dl/libffi/libffi-3.5.2.tar.gz`) | https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz | f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc | (2026-10-12, T2) | static libffi linked into liblwjgl |
| FreeType 2.14.1 source (git tag `VER-2-14-1` = commit **526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b**, the exact commit LWJGL 3.4.1's own `libfreetype.dylib.git` names), tag archive to `downloads/freetype-VER-2-14-1.tar.gz`. Needed because Minecraft 26.2 `NativeLibrariesBootstrap.loadLibraries` loads freetype eagerly and a failure is a fatal crash report | https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-1.tar.gz | see `downloads/SHA256SUMS-freetype` | 2026-10-12 | `work/native-image/scripts/t2/build-freetype.sh` -> `build/lwjgl-3.4.1/{xros,xrsimulator}/libfreetype.dylib` |
| Mojang 26.2 version json (`downloads/mojang-26.2/26.2.json`, reading only: library list) | https://piston-meta.mojang.com/v1/packages/d367f3dfbc0b3e14688df2311359deb609b234e3/26.2.json | (json) | 2026-10-12 | which LWJGL modules 26.2 loads |
