# work/natives/svc — Simple Voice Chat natives (2026-10-12)

Built by `work/natives/scripts/build-svc.sh [xros|xrsimulator|macosx]` (about 70 s for all three).
Outputs `svc/<sdk>/lib{opus4j,rnnoise4j,speex4j,lame4j}.dylib`: arm64, minos 26.0 (macOS 13.0), SDK 27.0.
They are unsigned, have install name `@rpath/lib<name>.dylib`, and import only libSystem. The outputs and
`downloads/` are not committed (work/ is gitignored). Everything regenerates from the script.

## Sources
The JNI glue is plain C, with no Rust. Each of these is the repo's own `native/CMakeLists.txt`, cross-compiled
with `CMAKE_SYSTEM_NAME=visionOS`.

| What | Where | Pinned |
|---|---|---|
| opus4j | git clone https://github.com/henkelmax/opus4j -> `downloads/svc/opus4j` | `362db14553d39c4dc080e4edd50630b17dbf9682` (2026-03-23) |
| rnnoise4j | https://github.com/henkelmax/rnnoise4j -> `downloads/svc/rnnoise4j` | `5f2097d504fc67f31d0d8900e9df0a5bdf4ec9ae` |
| speex4j | https://github.com/henkelmax/speex4j -> `downloads/svc/speex4j` | `9428cb14b8ec373fcd36f5b5792899611042ece9` |
| lame4j | https://github.com/henkelmax/lame4j -> `downloads/svc/lame4j` | `b09599989878cbc7349075eca0b3aa4f105e5957` |

At configure time, FetchContent fetches the following into `build/svc-<lib>-<sdk>/_deps`. The pins come from the
upstream CMakeLists, and the resolved commits are recorded here:
- libopus: xiph/opus `v1.5.2`, `ddbe48383984d56acd9e1ab6a090c54ca6b735a6`.
- rnnoise: xiph/rnnoise `372f7b4b76cde4ca1ec4605353dd17898a99de38`.
  - Model archive: `media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz`, sha256 `4ac81c5c…4b43`. Its
    weights are blanked; the real weights are loaded at run time from the jar resource `rnnoise/weights_blob.bin`.
- speexdsp: xiph/speexdsp `SpeexDSP-1.2.1`, `1b28a0f61bc31162979e1f26f3981fc3637095c8`.
- lame: lameproject/lame `1f5cc9487284d5950343aa5d4f70de433468070a`.
- minimp3: lieff/minimp3 `master`. This is unpinned upstream; the build used `ea99364f61c14656440e8d77e9c233ccf3124633`.
- jni.h / jni_md.h: openjdk/jdk8u `jdk8u462-ga` (raw.githubusercontent.com).

The JNI exports match the shipped SVC 1.21.5-2.6.11 jar's `natives/mac-aarch64` dylibs symbol for symbol:
opus 17, rnnoise 6, speex 15, lame 11.

## Built (sha256 prefix)
| sdk | opus4j | rnnoise4j | speex4j | lame4j |
|---|---|---|---|---|
| xros | 438,248 `fa76fb0298e9` | 103,152 `faee357795a1` | 103,272 `c5ba19104708` | 306,544 `00ede4ae6b4b` |
| xrsimulator | 458,224 `286cadc22d91` | 104,016 `abf6a3c2a912` | 104,192 `405b496d6275` | 308,976 `b420ef6e1860` |
| macosx | 458,288 `72ea2df610a1` | 104,080 `2a68889ca419` | 104,192 `2f9df1b65857` | 309,040 `ff0900333fd0` |

## How they are loaded
Three pieces work together:
1. `work/native-image/scripts/t4-src/subst/Target_SvcNatives.java` replaces each lib's `LibraryLoader.load`. The
   stock loader extracts the jar's mac-aarch64 dylib to tmp and `System.load`s it; the substitution instead
   `System.load`s `lib<name>.dylib` from `java.library.path`, which is the app's Frameworks dir.
2. The vivecraft row's `libs`/`natives` embed the dylibs. `mknative.sh` builds the xcframeworks, and the app build
   signs them.
3. The image needs two config files:
   - `config/extra/svc-jni.json`: the exception classes' `<init>(String)`, `[S`, and `OpusEncoder$Application.value`.
   - `config/extra/svc-resources.json`: `rnnoise/weights_blob.bin`.

## Host proof (macOS native image)
A standalone native-image test was built from the SVC jar, the substitution and the two config files. It ran
against `svc/macosx` with these results:
- opus 1.5.2: encode, decode, `decode(pkt, 2)` and the JNI `IllegalArgumentException` path all work.
- rnnoise: denoise works, with speech probability 0.998.
- speex: AGC works.
- lame: mp3 encoding works.

The prints were `[kl-svc] lib<name> loaded from …` for each library, then `SVCTEST OK`.

Safe to delete: `build/svc-*` always, and `downloads/svc` (re-clone it at the commits above).
