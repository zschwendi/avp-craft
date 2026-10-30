#!/bin/bash
# build-freetype.sh [xrsimulator|xros ...]  (default both) -> work/natives/build/lwjgl-3.4.1/<sdk>/libfreetype.dylib
# FreeType 2.14.1 (tag VER-2-14-1 = 526ec5c4, the commit LWJGL 3.4.1's libfreetype.dylib.git names) for visionOS.
# Minecraft 26.2 NativeLibrariesBootstrap loads freetype EAGERLY (FreeType.getLibrary() != null) and a failure is a
# fatal crash report, so the app must ship it. LWJGL's lwjgl-freetype has no JNI lib (JNI.call* through core).
# Differences from LWJGL's own build: no bundled HarfBuzz (26.2 has no lwjgl-harfbuzz), no brotli/png/bzip2;
# zlib = the system libz. macosx uses LWJGL's stock Maven libfreetype (build-lwjgl341-natives.sh).
set -e
# Keep native dependency builds within the requested compiler-job budget.
JOBS=${JOBS:-2}
case "$JOBS" in ''|*[!0-9]*) echo "JOBS must be a positive integer" >&2; exit 2;; esac
if ! [ "$JOBS" -ge 1 ] 2>/dev/null; then echo "JOBS must be a positive integer" >&2; exit 2; fi
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
N="$ROOT/work/natives"
TB=$N/downloads/freetype-VER-2-14-1.tar.gz
echo "44bd69d1f0750603410cfd4f26f1c5523c5a3a087a2dfa8639b757dc7c6677be  $TB" | shasum -a 256 -c - >/dev/null
for P in ${*:-xrsimulator xros}; do
  case $P in xros|xrsimulator) ;; *) echo "usage: $0 [xrsimulator|xros]"; exit 2;; esac
  B=$N/build/lwjgl-3.4.1/obj-$P/freetype; O=$N/build/lwjgl-3.4.1/$P; rm -rf $B; mkdir -p $B $O
  tar -xzf $TB -C $B
  SDK=$(xcrun --sdk $P --show-sdk-path)
  cmake -S $B/freetype-VER-2-14-1 -B $B/build -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=visionOS \
    -DCMAKE_OSX_SYSROOT=$SDK -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_BZIP2=ON -DFT_REQUIRE_ZLIB=ON \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-install_name,@rpath/libfreetype.dylib" > $B/cmake.log 2>&1 || { tail -30 $B/cmake.log; exit 1; }
  make -C $B/build -j"$JOBS" freetype > $B/make.log 2>&1 || { tail -30 $B/make.log; exit 1; }
  cp -L $(ls $B/build/libfreetype*.dylib | head -1) $O/libfreetype.dylib
  install_name_tool -id @rpath/libfreetype.dylib $O/libfreetype.dylib
  strip -x $O/libfreetype.dylib
  [ $P = xrsimulator ] && codesign -f -s - $O/libfreetype.dylib 2>/dev/null
  # every FT_* function LWJGL 3.4.1's FreeType.Functions resolves must be exported (missing => that call throws)
  L=$N/downloads/lwjgl3-3.4.1/modules/lwjgl/freetype/src/generated/java/org/lwjgl/util/freetype/FreeType.java
  miss=$(comm -23 <(grep -o 'apiGetFunctionAddress(FREETYPE, "[A-Za-z_0-9]*")' $L | sed 's/.*"\(.*\)")/_\1/' | sort -u) \
                  <(dyld_info -exports $O/libfreetype.dylib | awk '{print $2}' | sort -u))
  echo "== $P libfreetype: $(xcrun vtool -show-build $O/libfreetype.dylib | awk '/platform/{print $2}') $(stat -f%z $O/libfreetype.dylib) B; LWJGL-required FT_* missing: ${miss:-none}"
done
