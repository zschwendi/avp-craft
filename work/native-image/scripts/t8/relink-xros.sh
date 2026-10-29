#!/bin/bash
# relink-xros.sh xros|xrsimulator|macosx  -> out/t8/<sdk>/libminecraft.dylib  (route B, work/xros-probe/scripts)
# Links native-image's libminecraft.o (out/t8/tmp/SVM-*, from build-shared.sh) + scripts/t8/klmc_entry.c + GraalVM's
# static JDK/SVM archives against the chosen SDK. For xros/xrsimulator the archives are first copied with every
# member's LC_BUILD_VERSION rewritten (work/xros-probe/scripts/patch_buildver.py; 11 = visionOS, 12 = visionOS sim,
# minos 26.0) into out/t8/<sdk>/archives. The archive and library lists are read from the image's own
# Target.StaticLibraries / Target.Libraries metadata (out/t8/ni/libminecraft.dylib). Verifies build version,
# imports (system only), exports, size -> out/t8/<sdk>/verify.txt.
set -euo pipefail
W="$(cd "$(dirname "$0")/../.." && pwd)"
. $W/scripts/jdk.sh   # G by MCV: 21.0.12 for 1.21.x, 25.0.4 for 26.x
P="$(cd "$(dirname "$0")/../../../xros-probe/scripts" && pwd)"
S=$(cd "$(dirname "$0")" && pwd)
SDKNAME=${1:?usage: $0 xros|xrsimulator|macosx}
case $SDKNAME in
  xros)        TRIPLE=arm64-apple-xros26.0;           PNUM=11 ;;
  xrsimulator) TRIPLE=arm64-apple-xros26.0-simulator; PNUM=12 ;;
  macosx)      TRIPLE=arm64-apple-macos11.0;          PNUM= ;;
  *) echo "usage: $0 xros|xrsimulator|macosx"; exit 1 ;;
esac
SDK=$(xcrun --sdk $SDKNAME --show-sdk-path)
SDKVER=$(xcrun --sdk $SDKNAME --show-sdk-version)
T8OUT=${T8OUT:-$W/out/t8}
TMP=$(ls -d $T8OUT/tmp/SVM-* | tail -1)
OBJ=$TMP/libminecraft.o; EXP=$TMP/exported_symbols.list
[ -f $OBJ ] && [ -f $EXP ] || { echo "missing $OBJ / $EXP: run build-shared.sh"; exit 1; }
NI=$T8OUT/ni/libminecraft.dylib
DST=$T8OUT/$SDKNAME; mkdir -p $DST
# --- archive + library lists from the image's metadata
STATICS=$(strings -a $NI | sed -n 's/^.*Target\.StaticLibraries=//p' | head -1 | tr '|' ' ')
LIBS=$(strings -a $NI | sed -n 's/^.*VM\.Target\.Libraries=//p' | head -1 | tr '|' '\n')
echo "[relink] $SDKNAME static: $STATICS"
LIBFLAGS=()
while IFS= read -r l; do [ -z "$l" ] && continue
  case "$l" in -framework\ *) LIBFLAGS+=(-framework "${l#-framework }") ;; -*) LIBFLAGS+=("$l") ;; *) LIBFLAGS+=("-l$l") ;; esac
done <<< "$LIBS"
# visionOS: CFNetwork is not re-exported through CoreServices (xros-probe REPORT, route B step 3)
LIBFLAGS+=(-framework CFNetwork)
echo "[relink] libs: ${LIBFLAGS[*]}"
ARCH=$DST/archives; rm -rf $ARCH; mkdir -p $ARCH
ARGS=()
for n in $STATICS; do
  a=""; for d in $G/lib/svm/clibraries/darwin-aarch64 $G/lib/static/darwin-aarch64; do [ -f $d/$n ] && a=$d/$n && break; done
  [ -n "$a" ] || { echo "archive $n not found"; exit 1; }
  if [ -z "$PNUM" ]; then ARGS+=("$a"); continue; fi
  X=$ARCH/.x-$n; rm -rf $X; mkdir -p $X
  (cd $X && ar -x "$a")
  python3 -I $P/patch_buildver.py $PNUM 26.0 $SDKVER $X/*.o | sed "s/^/  $n: /"
  ar -rcs $ARCH/$n $X/*.o 2>/dev/null; rm -rf $X
  ARGS+=("$ARCH/$n")
done
# --- exports: native-image's list + the two klmc_* symbols
cp $EXP $DST/exported_symbols.list; printf '_klmc_main\n_klmc_version\n' >> $DST/exported_symbols.list
VER="klmc t8a $(date -u +%Y-%m-%dT%H:%MZ) $SDKNAME ni=$(shasum -a 256 $OBJ | cut -c1-12)"
xcrun --sdk $SDKNAME clang -target $TRIPLE -isysroot $SDK -O2 -c -o $DST/klmc_entry.o -DKLMC_VERSION="\"$VER\"" $S/klmc_entry.c
OUTF=$DST/libminecraft.dylib
T0=$(date +%s)
xcrun --sdk $SDKNAME clang -target $TRIPLE -isysroot $SDK -dynamiclib -o $OUTF $OBJ $DST/klmc_entry.o \
  -Wl,-exported_symbols_list,$DST/exported_symbols.list -install_name @rpath/libminecraft.dylib \
  "${ARGS[@]}" "${LIBFLAGS[@]}" -Wl,-x ${EXTRA_LDFLAGS:-} 2>&1 | grep -v 'reducing alignment of section' || true
[ -f $OUTF ] || { echo "LINK FAILED"; exit 1; }
echo "[relink] link $(( $(date +%s) - T0 )) s"
rm -rf $ARCH
# --- verify
{
  echo "== $OUTF"; ls -l $OUTF | awk '{print "size", $5}'
  echo "== build version"; xcrun vtool -show-build $OUTF | grep -E 'platform|minos|sdk'
  echo "== imports"; otool -L $OUTF | tail -n +2
  NONSYS=$(otool -L $OUTF | tail -n +3 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/)' || true)
  echo "non-system imports: ${NONSYS:-none}"
  echo "== exports"; nm -gU $OUTF | awk '{print $3}' | sort | tr '\n' ' '; echo
  for s in _klmc_main _klmc_version _run_main _graal_create_isolate _graal_attach_thread _graal_tear_down_isolate; do
    nm -gU $OUTF | awk '{print $3}' | grep -qx $s && echo "export $s OK" || echo "export $s MISSING"; done
  echo "== undefined (non-system check: all must resolve from the imports above)"; nm -u $OUTF | wc -l
  echo "== klmc_version: $VER"
  echo "== segments"; size -m $OUTF | grep -E '^Segment|total' | head -12
} | tee $DST/verify.txt
