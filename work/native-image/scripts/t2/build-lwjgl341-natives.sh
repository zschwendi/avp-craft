#!/bin/bash
# build-lwjgl341-natives.sh [macosx|xrsimulator|xros ...]   (default: macosx)
#   -> work/natives/build/lwjgl-3.4.1/<platform>/{liblwjgl,liblwjgl_opengl,liblwjgl_stb[,liblwjgl_tinyfd],libopenal}.dylib
#      + BUILT-lwjgl-<platform> one level up when verified (READY-<platform> comes from build-klglfw341.sh).
# STOCK LWJGL 3.4.1 (tag 3.4.1 = b800ccff, work/natives/downloads/lwjgl3-3.4.1; fetch: work/natives/scripts/fetch-lwjgl341.sh)
# for Minecraft 26.2 ONLY. 1.21.5 / 1.21.11 keep the QuestCraft fork f1ec2b7 (build-fork-natives*.sh, unchanged).
# Mirrors LWJGL's own config/macos/build.xml (core / opengl / stb / tinyfd rules) with plain clang, same as the fork
# scripts: -O3 -fPIC -DNDEBUG -DLWJGL_MACOS -DLWJGL_arm64 -ObjC, core links static libffi 3.5.2 + CoreFoundation.
# The native libraries are the same for the `lwjgl` and `lwjgl:unsafe` core jars (the classifier only changes Java
# classes: the unsafe jar has no META-INF/versions/25 JNI/MemoryUtil/Upcalls FFM overrides), so the bake must put
# lwjgl-3.4.1-unsafe.jar on the classpath (Mojang's 26.2 json already does: org.lwjgl:lwjgl:3.4.1:unsafe).
# Not built: glfw (klglfw replaces it), openal/openxr (no LWJGL JNI lib: OpenAL Soft / the Klepton loader shim),
#   jemalloc (macosx: stock Maven dylib as a host-JVM convenience; visionOS pins org.lwjgl.system.allocator=system),
#   vulkan/shaderc/spvc/vma (26.2 Vulkan backend: forced off; 26.2 only loads shaderc/spvc/vma after VK.create()
#   succeeded). freetype: macosx = stock Maven dylib, visionOS = build-freetype.sh (FreeType 2.14.1, same commit).
set -e
# Keep native dependency builds within the requested compiler-job budget.
JOBS=${JOBS:-2}
case "$JOBS" in ''|*[!0-9]*) echo "JOBS must be a positive integer" >&2; exit 2;; esac
if ! [ "$JOBS" -ge 1 ] 2>/dev/null; then echo "JOBS must be a positive integer" >&2; exit 2; fi
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
N="$ROOT/work/natives"
W="$ROOT/work/native-image"
L=$N/downloads/lwjgl3-3.4.1/modules/lwjgl
MV=$N/downloads/lwjgl-3.4.1-maven
G=${G:-$W/graalvm/graalvm-jdk-25.0.4+7.1/Contents/Home}
FFI=$W/dl/libffi/libffi-3.5.2.tar.gz
OUT=$N/build/lwjgl-3.4.1
[ "$(git -C $N/downloads/lwjgl3-3.4.1 rev-parse HEAD)" = b800ccffab14396fc529ddb6c931b7c5c5226763 ] || { echo "run fetch-lwjgl341.sh"; exit 1; }

build_one() {
  local P=$1 TGT SDK O B CC
  case $P in
    macosx)      TGT=arm64-apple-macos11 ;;
    xros)        TGT=arm64-apple-xros26.0 ;;
    xrsimulator) TGT=arm64-apple-xros26.0-simulator ;;
    *) echo "usage: $0 [macosx|xrsimulator|xros]"; exit 2 ;;
  esac
  SDK=$(xcrun --sdk $P --show-sdk-path); CC="xcrun --sdk $P clang"
  O=$OUT/$P; B=$OUT/obj-$P; rm -rf $B $O; mkdir -p $B/core $B/opengl $B/stb $B/tinyfd $O
  local CF="-O3 -fPIC -DNDEBUG -DLWJGL_MACOS -DLWJGL_arm64 -isysroot $SDK -target $TGT -I$G/include -I$G/include/darwin -I$L/core/src/main/c -I$L/core/src/main/c/macos"
  # visionOS SDKs lack the macOS-only <objc/objc-runtime.h> (ObjCRuntime.c includes it): a 2-line shim with what
  # the macOS header itself pulls in (<objc/runtime.h> + <objc/message.h>).
  if [ $P != macosx ]; then mkdir -p $B/shim/objc; printf '#include <objc/runtime.h>\n#include <objc/message.h>\n' > $B/shim/objc/objc-runtime.h; CF="$CF -I$B/shim"; fi
  local LF="-dynamiclib -Wl,-no_compact_unwind -target $TGT -isysroot $SDK -O3"
  echo "== $P ($TGT)"
  # libffi 3.5.2 static (Darwin configure => FFI_EXEC_TRAMPOLINE_TABLE 1: no mmap/MAP_JIT, vm_remap trampolines)
  local FO=$OUT/libffi-$P
  if [ ! -f $FO/lib/libffi.a ]; then
    rm -rf $B/libffi-src; mkdir -p $B/libffi-src; tar -xzf $FFI -C $B/libffi-src
    ( cd $B/libffi-src/libffi-3.5.2 && CC="$CC -target $TGT -isysroot $SDK" CFLAGS="-O3 -fPIC" \
        ./configure --build=aarch64-apple-darwin$(uname -r | cut -d. -f1) --host=aarch64-apple-darwin \
        --prefix=$FO --disable-shared --enable-static --disable-docs --disable-multi-os-directory >$B/libffi-configure.log 2>&1 \
      && make -j"$JOBS" >$B/libffi-make.log 2>&1 && make install >>$B/libffi-make.log 2>&1 \
      && cp aarch64-apple-darwin/fficonfig.h $FO/ ) || { echo "libffi failed ($B/libffi-*.log)"; exit 1; }
  fi
  grep -q 'FFI_EXEC_TRAMPOLINE_TABLE 1' $FO/fficonfig.h || { echo "libffi: no trampoline table"; exit 1; }
  cc1() { # cc1 <objdir> <flags...> -- files
    local d=$1; shift; local fl=(); while [ "$1" != "--" ]; do fl+=("$1"); shift; done; shift
    local f pid failed=0
    local pids=()
    rm -f "$d"/*.o
    for f in "$@"; do
      $CC -c -ObjC $CF "${fl[@]}" "$f" -o "$d/$(basename "${f%.*}").o" 2>>"$B/cc.log" &
      pids+=("$!")
      if [ "${#pids[@]}" -ge "$JOBS" ]; then
        for pid in "${pids[@]}"; do if ! wait "$pid"; then failed=1; fi; done
        pids=()
        [ "$failed" -eq 0 ] || { echo "compile failed in $d (see $B/cc.log)"; return 1; }
      fi
    done
    for pid in "${pids[@]}"; do if ! wait "$pid"; then failed=1; fi; done
    [ "$failed" -eq 0 ] && [ "$(ls "$d"/*.o 2>/dev/null | wc -l)" -eq "$#" ] \
      || { echo "compile failed in $d (see $B/cc.log)"; return 1; }
  }
  # core: LWJGL's bundled ffi.h + aarch64/ffitarget.h (trampoline table on Apple), static libffi 3.5.2
  cc1 $B/core -Wall -Wno-unused-parameter -I$L/core/src/main/c/libffi -I$L/core/src/main/c/libffi/aarch64 -- \
      $L/core/src/main/c/*.c $L/core/src/generated/c/*.c $L/core/src/generated/c/macos/*.c
  $CC $LF -install_name @rpath/liblwjgl.dylib -o $O/liblwjgl.dylib $B/core/*.o $FO/lib/libffi.a -framework CoreFoundation
  cc1 $B/opengl -I$L/opengl/src/main/c -- $(ls $L/opengl/src/generated/c/*.c | grep -v org_lwjgl_opengl_WGL.c)
  $CC $LF -install_name @rpath/liblwjgl_opengl.dylib -o $O/liblwjgl_opengl.dylib $B/opengl/*.o
  cc1 $B/stb -I$L/stb/src/main/c -- $L/stb/src/generated/c/*.c
  $CC $LF -install_name @rpath/liblwjgl_stb.dylib -o $O/liblwjgl_stb.dylib $B/stb/*.o
  # tinyfd on every platform: 26.2 NativeLibrariesBootstrap loads it eagerly (fatal if missing). On visionOS its
  # popen()-based dialogs simply fail (sandbox); T3 substitutes the message box anyway.
  cc1 $B/tinyfd -I$L/tinyfd/src/main/c -- $L/tinyfd/src/main/c/tinyfiledialogs.c $L/tinyfd/src/generated/c/*.c
  $CC $LF -install_name @rpath/liblwjgl_tinyfd.dylib -o $O/liblwjgl_tinyfd.dylib $B/tinyfd/*.o
  if [ $P = macosx ]; then
    # third-party libs LWJGL itself does not build from this repo: stock 3.4.1 Maven macos-arm64 binaries
    unzip -qjo $MV/lwjgl-openal-3.4.1-natives-macos-arm64.jar 'macos/arm64/org/lwjgl/openal/libopenal.dylib' -d $O
    unzip -qjo $MV/lwjgl-jemalloc-3.4.1-natives-macos-arm64.jar 'macos/arm64/org/lwjgl/jemalloc/libjemalloc.dylib' -d $O
    unzip -qjo $MV/lwjgl-freetype-3.4.1-natives-macos-arm64.jar 'macos/arm64/org/lwjgl/freetype/libfreetype.dylib' -d $O
  else
    cp $N/openal/$P/libopenal.dylib $O/   # OpenAL Soft 1.25.2 (scripts/build-openal.sh)
    "$(dirname "$0")/build-freetype.sh" $P  # FreeType 2.14.1 (26.2 loads it eagerly; fatal if missing)
  fi
  strip -x $O/liblwjgl*.dylib
  if [ $P = xrsimulator ]; then codesign -f -s - $O/*.dylib 2>/dev/null; fi
  verify $P $O
  date '+%Y-%m-%d %H:%M:%S %Z' > $OUT/BUILT-lwjgl-$P
  echo "BUILT-lwjgl-$P (READY-$P is written by build-klglfw341.sh once klglfw is in place too)"
}

# verify: platform load command, JNI export sets equal to stock 3.4.1 macOS natives (from Maven), no JIT-ish imports
verify() {
  local P=$1 O=$2 want plat
  case $P in macosx) want=1;; xros) want=11;; xrsimulator) want=12;; esac
  local S=$OUT/obj-$P/stock; mkdir -p $S
  unzip -qjo $MV/lwjgl-3.4.1-natives-macos-arm64.jar 'macos/arm64/org/lwjgl/liblwjgl.dylib' -d $S
  unzip -qjo $MV/lwjgl-opengl-3.4.1-natives-macos-arm64.jar '*.dylib' -d $S
  unzip -qjo $MV/lwjgl-stb-3.4.1-natives-macos-arm64.jar '*.dylib' -d $S
  unzip -qjo $MV/lwjgl-tinyfd-3.4.1-natives-macos-arm64.jar '*.dylib' -d $S
  for f in $O/liblwjgl*.dylib; do
    plat=$(otool -l $f | awk '/LC_BUILD_VERSION/{b=1} b&&/platform/{print $2; exit}')
    [ "$plat" = $want ] || { echo "verify: $f platform $plat != $want"; exit 1; }
    local n; n=$(basename $f)
    jx() { dyld_info -exports "$1" | awk '$2~/^_Java_/{print $2}' | sort; }   # stock is fully stripped: nm sees nothing
    comm -3 <(jx $f) <(jx $S/$n) > $OUT/obj-$P/$n.exportdiff
    printf '  %-24s Java_* %5d (stock %5d)  only-ours %d  only-stock %d\n' $n $(jx $f | wc -l) $(jx $S/$n | wc -l) \
      $(grep -c '^_' $OUT/obj-$P/$n.exportdiff || true) $(grep -c $'^\t' $OUT/obj-$P/$n.exportdiff || true)
    if nm -u $f | grep -qE '^_(mmap|mprotect|pthread_jit_write_protect_np)$'; then echo "verify: $n imports mmap/mprotect/JIT"; exit 1; fi
  done
}

for p in "${@:-macosx}"; do build_one $p; done
