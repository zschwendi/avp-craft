# Source integration and release

The public repository stores clean text patches against a fixed upstream renderer commit plus the original terrain/arm source modules. Working folders, game installations, local tools and private resource packs are not input trees.

Import the final reviewed source once, using paths chosen on your machine:

```sh
python3 tools/import_source.py --renderer PATH_TO_RENDERER --terrain PATH_TO_TERRAIN --hands PATH_TO_HANDS
python3 tools/audit.py
```

The importer includes both tracked changes and new untracked approved Java, Metal, Objective-C/C and header files under `metal/src/main`. It therefore includes new material-loader mixins and native headers. It excludes local benchmark tools, executables, recordings, account files, worlds and private packs. Module imports include source/resources and their original LICENSE files. The generated source manifest contains relative paths and hashes only; the build checks that every imported file still matches it. Review the patch and added-source snapshot before committing.

For a later import, review and remove only the existing generated snapshot deliberately before rerunning; the tool refuses to silently overwrite a prior module/addition snapshot. Never point it at launcher account/config directories.

The public Balanced properties are curated separately from personal instance configurations. Material-independent analytic normals and neutral normal/AO/height fallbacks must be tested using vanilla textures before calling a release usable. Recorder/debug settings stay outside the public preset. Update the README evidence statement only after controlled runtime measurements.

Run the build on macOS ARM64 with Java25. The native library gets an ad-hoc signature for load integrity; this is not Developer ID notarization. Keep upstream LICENSE, NOTICE and LICENSES in every renderer jar. A release contains exactly three owned mod jars, the dependency-download `.mrpack` and SHA256SUMS. The `.mrpack` links those jars to the configured GitHub repository/tag; release URLs do not resolve until the maintainer uploads the jars to that exact release.

The `.mrpack` is a ZIP archive in the standard Modrinth format; Prism can import it directly. Its overrides belong in the Minecraft game folder. Launcher memory/Java options are documented installation settings; placing `instance.cfg` inside those overrides would not configure Prism. GitHub-hosted mod downloads go through Prism's normal source-review dialog. Do not disable that review to reduce install steps.

Publication is a separate maintainer action. Do not commit `.cache`, build output, game dependencies, accounts, worlds, private pack assets or full review conversations. Do not describe shader-compilation/CPU checks as measured display FPS. Do not alter authentication or game ownership checks.
