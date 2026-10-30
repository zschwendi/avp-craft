#!/bin/bash
# Simple Voice Chat natives (henkelmax opus4j / rnnoise4j / speex4j / lame4j) as visionOS dylibs.
# Each repo's own native/CMakeLists.txt (plain C JNI glue + FetchContent of libopus / rnnoise / speexdsp / lame)
# cross-compiled with CMAKE_SYSTEM_NAME=visionOS. Sources: downloads/svc/<repo> (git clones, commits pinned in
# svc/PROVENANCE.md); FetchContent fetches the codec at configure time (pinned tags/commits in the upstream CMake).
# Output: svc/<sdk>/lib<name>.dylib, install name @rpath/lib<name>.dylib, unsigned (codesign happens in the app
# build). The Java side is a GraalVM substitution (work/native-image/scripts/t4-src/subst/Target_SvcNatives.java)
# that System.load()s these from the app's Frameworks dir instead of extracting the jar's mac dylibs to tmp.
# Usage: build-svc.sh [xros|xrsimulator|macosx ...]   (default: xros xrsimulator macosx)
#        LIBS="opus4j rnnoise4j" build-svc.sh xros    (default LIBS: all four)
set -euo pipefail
# Keep native dependency builds within the requested compiler-job budget.
JOBS=${JOBS:-2}
case "$JOBS" in ''|*[!0-9]*) echo "JOBS must be a positive integer" >&2; exit 2;; esac
if ! [ "$JOBS" -ge 1 ] 2>/dev/null; then echo "JOBS must be a positive integer" >&2; exit 2; fi
N="$(cd "$(dirname "$0")/.." && pwd)"
D="$N/downloads/svc"
MIN=26.0
LIBS="${LIBS:-opus4j rnnoise4j speex4j lame4j}"
commit() { case $1 in opus4j) echo 362db14;; rnnoise4j) echo 5f2097d;; speex4j) echo 9428cb1;; lame4j) echo b095999;; esac; }
for r in $LIBS; do
  [ -d "$D/$r" ] || git clone -q "https://github.com/henkelmax/$r.git" "$D/$r"
  git -C "$D/$r" -c advice.detachedHead=false checkout -q "$(commit $r)"
done
for S in ${@:-xros xrsimulator macosx}; do
  case $S in
    xros|xrsimulator) SYS=(-DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=$S -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN) ;;
    macosx)           SYS=(-DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0) ;;
    *) echo "unknown sdk $S"; exit 1 ;;
  esac
  OUT="$N/svc/$S"; mkdir -p "$OUT"
  for r in $LIBS; do
    B="$N/build/svc-$r-$S"; rm -rf "$B"
    cmake -S "$D/$r/native" -B "$B" -G "Unix Makefiles" "${SYS[@]}" -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_TESTING=OFF \
      -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF \
      -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-install_name,@rpath/lib$r.dylib -Wl,-dead_strip" \
      > "$N/build/svc-$r-$S-cmake.log" 2>&1 || { tail -30 "$N/build/svc-$r-$S-cmake.log"; exit 1; }
    cmake --build "$B" --target "$r" --parallel "$JOBS" > "$N/build/svc-$r-$S-build.log" 2>&1 \
      || { tail -30 "$N/build/svc-$r-$S-build.log"; exit 1; }
    cp -L "$B/lib$r.dylib" "$OUT/lib$r.dylib"
    install_name_tool -id "@rpath/lib$r.dylib" "$OUT/lib$r.dylib" 2>/dev/null || true
    plat=$(xcrun vtool -show-build "$OUT/lib$r.dylib" | awk '/platform/{print $2}')
    echo "OK $S $r -> $OUT/lib$r.dylib ($(stat -f%z "$OUT/lib$r.dylib") B, $plat)"
  done
done
