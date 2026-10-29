#!/bin/bash
# mknative.sh — the native kind's mkguest.sh: wrap a NATIVE target's Darwin
# dylibs (targets.py `natives`) into LIBRARY xcframeworks under
# Frameworks/<target>/, which gen_xcodeproj.py embeds (CodeSignOnCopy) like any
# other guest. Library, not .framework, because the guest loads them by FILE
# name: LWJGL's -Dorg.lwjgl.librarypath=<Frameworks> looks for
# Frameworks/liblwjgl.dylib, and the dylibs' install names are
# @rpath/<name>.dylib already. Embedded, they land as Frameworks/<name>.dylib.
#
# Only the slices XROS_PLATFORMS names (run.sh exports it); a missing source is
# a hard stop naming the file and what builds it.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
[ "$KLT_KIND" = native ] || { echo "!! $KLT_NAME is not a native target"; exit 1; }
OUT="Frameworks/$KLT_NAME"
PLATS=""
for P in ${XROS_PLATFORMS:-xros xrsim}; do
  case "$P" in
    xros)  PLATS="$PLATS xros" ;;
    xrsim) PLATS="$PLATS xrsimulator" ;;
    *)     echo "!! XROS_PLATFORMS: '$P' is not one of xros xrsim"; exit 1 ;;
  esac
done
rm -rf "$OUT" "build/native/$KLT_NAME"; mkdir -p "$OUT"
missing=0
for pair in $KLT_NATIVES; do
  NAME="${pair%%=*}"; TPL="${pair#*=}"
  ARGS=()
  for PLAT in $PLATS; do
    case "$PLAT" in xros) SIM=xros ;; xrsimulator) SIM=xrsim ;; esac
    SRC="$ROOT/$(printf '%s' "$TPL" | sed -e "s/{sdk}/$PLAT/g" -e "s/{sim}/$SIM/g")"
    # MCV=<minecraft version> (unset/1.21.5 = the shipped image): the image for that instance is built into
    # out/t8-$MCV/ (M3A-1.21.11-HOST-REPORT.md §7); stage_native.sh must be run with the same MCV.
    if [ "$NAME" = libminecraft ] && [ "${MCV:-26.2}" != 1.21.5 ]; then SRC="${SRC%%/out/t8/*}/out/t8-$MCV/${SRC#*/out/t8/}"; fi
    if [ ! -f "$SRC" ]; then
      echo "!! $NAME ($PLAT): $SRC missing"
      case "$NAME" in
        libminecraft) echo "   (work/native-image/scripts/t8/build-shared.sh + relink-xros.sh $PLAT${MCV:+, T8OUT=out/t8-$MCV for MCV=$MCV})";;
        libklglfw|libklegl|libltw) echo "   (work/native-image/scripts/t8b/build-natives.sh $PLAT)";;
        libvloader|libopenxr_loader) echo "   (work/natives/scripts/build-openxr-shim.sh)";;
        libopenal) echo "   (work/natives/scripts/build-openal.sh)";;
        liblwjgl*) echo "   (work/native-image/scripts/t2/build-fork-natives-xros.sh $PLAT)";;
      esac
      missing=1; continue
    fi
    # A copy per slice: the device slice is signed by the app build, the
    # simulator's ad-hoc by run.sh; neither touches the source tree.
    D="build/native/$KLT_NAME/$PLAT"; mkdir -p "$D"
    cp "$SRC" "$D/$NAME.dylib"
    chmod u+w "$D/$NAME.dylib"
    id=$(otool -D "$D/$NAME.dylib" | tail -1)
    if [ "$id" != "@rpath/$NAME.dylib" ]; then
      install_name_tool -id "@rpath/$NAME.dylib" "$D/$NAME.dylib" 2>/dev/null
      echo "  $NAME ($PLAT): install name $id -> @rpath/$NAME.dylib"
    fi
    ARGS+=(-library "$D/$NAME.dylib")
  done
  [ "${#ARGS[@]}" -gt 0 ] || continue
  xcodebuild -create-xcframework "${ARGS[@]}" -output "$OUT/$NAME.xcframework" > /dev/null
  printf '  %-26s %s bytes\n' "$NAME.xcframework" "$(stat -f%z "$D/$NAME.dylib")"
done
[ "$missing" = 0 ] || { echo "!! [mknative] missing natives above"; exit 1; }
echo "[mknative] $KLT_NAME done -> $OUT/"
