#!/bin/bash
# build-natives.sh [xros|xrsimulator ...] -- T8b: the app-side natives the vivecraft target embeds that are built from
# THIS port's sources (everything else comes prebuilt from T2/T5/T6/T7/T8a; visionos/mknative.sh lists them all):
#   out/t8b/<sdk>/libklglfw.dylib   scripts/t4-src/native/klglfw.c       (GLFW stand-in, app mode = KLGLFW_HOST=klepton)
#   out/t8b/<sdk>/libklegl.dylib    scripts/t4-src/native/klegl_bridge.c (ltw's libEGL -> the app's kl_egl gateway)
#   work/natives/ltw/build/<sdk>/libltw.dylib  (make -C work/natives/ltw SDK=<sdk>, only if missing or KL_LTW_REBUILD=1)
# Also rebuilds the macOS libklglfw (lwjgl-fork-t4/natives) so the T4/T8a host runs keep using the same source.
set -euo pipefail
_HERE="$(cd "$(dirname "$0")" && pwd)"
W="$(cd "$_HERE/../.." && pwd)"
ROOT="$(cd "$_HERE/../../../.." && pwd)"
. $W/scripts/jdk.sh   # G by MCV: 21.0.12 for 1.21.x, 25.0.4 for 26.x
S=$W/scripts/t4-src/native
ANGLE_INC=${ANGLE_INC:-$ROOT/vendor/include}
LTWDIR=$ROOT/work/natives/ltw
SDKS=${*:-xros xrsimulator}
CF="-O2 -g -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -I$G/include -I$G/include/darwin -I$ANGLE_INC"
clang -dynamiclib $CF -target arm64-apple-macos11 -isysroot "$(xcrun --sdk macosx --show-sdk-path)" \
  -install_name @rpath/libklglfw.dylib -o $W/lwjgl-fork-t4/natives/libklglfw.dylib $S/klglfw.c
echo "OK macosx libklglfw"
for sdk in $SDKS; do
  case $sdk in xros) t=arm64-apple-xros26.0 ;; xrsimulator) t=arm64-apple-xros26.0-simulator ;; *) echo "bad sdk $sdk"; exit 1 ;; esac
  O=$W/out/t8b/$sdk; mkdir -p $O
  xcrun --sdk $sdk clang -dynamiclib $CF -target $t -install_name @rpath/libklglfw.dylib -o $O/libklglfw.dylib $S/klglfw.c
  xcrun --sdk $sdk clang -dynamiclib $CF -target $t -install_name @rpath/libklegl.dylib -o $O/libklegl.dylib $S/klegl_bridge.c
  if [ ! -f $LTWDIR/build/$sdk/libltw.dylib ] || [ "${KL_LTW_REBUILD:-0}" = 1 ]; then
    make -C $LTWDIR SDK=$sdk -j12 > $O/ltw-make.log 2>&1 || { tail -20 $O/ltw-make.log; exit 1; }
  fi
  for f in $O/libklglfw.dylib $O/libklegl.dylib $LTWDIR/build/$sdk/libltw.dylib; do
    printf '  %-60s %s %s\n' "${f#"$ROOT/"}" "$(stat -f%z $f)" \
      "$(xcrun vtool -show-build $f | awk '/platform/{print $2}')"
  done
  echo "OK $sdk"
done
