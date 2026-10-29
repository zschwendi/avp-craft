#!/bin/bash
# run.sh jvm|native <seconds> [extra runtime args]  -- T4: the baked Fabric world on the klglfw GLFW stand-in, headless.
#   GL path (default): -Dorg.lwjgl.opengl.libname=$LTW (libltw.dylib -> ANGLE Metal ES 3.0); klglfw creates the
#                      ES 3.0 pbuffer context through ltw's eglCreateContext/eglMakeCurrent.
#   env: LTW=<path>        libltw.dylib (default out/t4/ltw/libltw.dylib = snapshot of the ltw owner's build)
#        ANGLE=<dir>       ANGLE libEGL/libGLESv2 dir (default $ROOT/vendor/out/Debug) -> LTW_EGL_LIBRARY
#        TAG=<name>        output tag: logs/t4-<TAG>.txt, out/t4/shots-<TAG>/ (default <mode>)
#        SHOT_EVERY / SHOT_AT / SCRIPT / EVENTS / SIZE  -> KLGLFW_SHOT_EVERY / _SHOT_AT / _SCRIPT / _EVENTS / _SIZE
#        NOWORLD=1         no --quickPlaySingleplayer world (title screen); WORLD=<save dir> picks another save (default world)
#        MCAUTH_JSON=<f>   signed-in account (see below)
#        VR=1              Android/OpenXR path as in scripts/t23/run.sh (probe loader, vrEnabled for this run)
#        BIN=...           native binary (default out/t4/mc-fabric-t4)
#   At T-8 s: mapped files (lsof) -> <log>.dylibs, every __TEXT image incl. the shared cache (vmmap) -> <log>.images;
#   CGWindows owned by the pid -> <log>.windows (out/t4/windows, scripts/t4/windows.swift);
#   SAMPLE=1 also writes a 3 s sample(1) profile -> <log>.sample. Prints EXIT.
#        KL_INSERT=<dylib> DYLD_INSERT_LIBRARIES for the native binary only (e.g. scripts/perf/mtlcount.m)
#        MCV=<version>     instance (scripts/inst.sh; default 26.2): fgamedir, t4.cp, assets, index
. "$(dirname "$0")/../inst.sh"
MODE=$1; T=$2; shift 2
TAG=${TAG:-$MODE}
LOG=${LOG:-$W/logs/t4-$TAG.txt}
LTW=${LTW:-$W/out/t4/ltw/libltw.dylib}   # snapshot of work/natives/ltw/build/macosx/libltw.dylib, see out/t4/ltw/SNAPSHOT.txt
# 1.21.6+ instances need the M3 stage A ltw fixes (sync flush, cube faces, UBO list reset, texture buffers): live build
[ "$MCV" != 1.21.5 ] && [ "$LTW" = $W/out/t4/ltw/libltw.dylib ] && LTW=$W/../natives/ltw/build/macosx/libltw.dylib
ANGLE=${ANGLE:-$ROOT/vendor/out/Debug}
SHOTS=$W/out/t4/shots-$TAG
rm -rf $SHOTS; mkdir -p $SHOTS $W/logs
export LTW_EGL_LIBRARY=$ANGLE/libEGL.dylib
export KLGLFW_SHOT_DIR=$SHOTS KLGLFW_FPS=1
[ -n "$SHOT_EVERY" ] && export KLGLFW_SHOT_EVERY=$SHOT_EVERY
export KLGLFW_SHOT_AT=${SHOT_AT:-8,20,35,50}
[ -n "$SCRIPT" ] && export KLGLFW_SCRIPT=$SCRIPT
[ -n "$EVENTS" ] && export KLGLFW_EVENTS="$EVENTS"
[ -n "$SIZE" ] && export KLGLFW_SIZE=$SIZE
CFG=$I/fgamedir/config/vivecraft-client-config.json
GLARGS="-Dorg.lwjgl.opengl.libname=$LTW"
[ "$NOLTW" = 1 ] && GLARGS=""
LWJGL="-Dorg.lwjgl.librarypath=$W/lwjgl-fork-t4/natives -Dorg.lwjgl.system.allocator=system -Dorg.lwjgl.util.Debug=true -Dorg.lwjgl.util.DebugLoader=true $GLARGS"
# 26.x: stock LWJGL 3.4.1 natives + libklglfw from the natives agent (work/natives/build/lwjgl-3.4.1/USAGE.md); our
# Vivecraft 26.2 OpenXR jar takes its mobile GLES path with -Dvivecraft.openxr.mobile=true (as launch.txt for 26.x).
case $MCV in 1.*) ;; *)
  LWJGL="-Dorg.lwjgl.librarypath=${LWJGL341:-$W/../natives/build/lwjgl-3.4.1}/macosx -Dorg.lwjgl.system.allocator=system -Dorg.lwjgl.util.Debug=true -Dorg.lwjgl.util.DebugLoader=true -Dorg.lwjgl.util.NoHashChecks=true -Dvivecraft.openxr.mobile=true $GLARGS";;
esac
VRARGS=""
if [ "$VR" = 1 ]; then
  cp $CFG $CFG.t4bak
  python3 -I -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d['vrEnabled']='true'; json.dump(d,open(p,'w'),indent=2)" $CFG
  VRARGS="-Dklepton.vr.android=true -Dorg.lwjgl.openxr.libname=$W/out/t23/natives/libopenxr_loader.dylib -Djava.library.path=$W/out/t23/natives"
fi
GA="--quickPlaySingleplayer ${WORLD:-world}"; [ "$NOWORLD" = 1 ] && GA=""
COMMON="-Dfabric.gameMappingNamespace=$GAMENS -Dfabric.gameJarPath=$REMAPPED"
AUTH="--accessToken 0 --username spike --uuid 00000000-0000-0000-0000-000000000000"
# MCAUTH_JSON=<userdata accounts/<uuid>.json> (tools/vivecraft_msa_login.py --refresh-only): play as the signed-in
# account. The token goes only into the game's argv; it is never echoed or logged here.
# 26.x dropped --userType from the launch args (authlib 9; Main still ignores unknown options): pass it on 1.x only.
UT=" --userType msa"; case $MCV in 1.*) ;; *) UT="";; esac
[ -n "$MCAUTH_JSON" ] && AUTH=$(python3 -I -c "import json,sys; d=json.load(open(sys.argv[1])); print('--accessToken', d['accessToken'], '--username', d['username'], '--uuid', d['uuid'] + sys.argv[2])" "$MCAUTH_JSON" "$UT")
GAME="$AUTH --version $MCV --gameDir $I/fgamedir --assetsDir $ASSETS --assetIndex $AIDX $GA"
cd $I/fgamedir
{
echo "[t4-run] mcv=$MCV mode=$MODE T=$T tag=$TAG ltw=$( [ "$NOLTW" = 1 ] && echo none || echo $LTW ) angle=$ANGLE shots=$SHOTS"
if [ "$MODE" = jvm ]; then
  exec $G/bin/java -Xmx3G -Djava.awt.headless=true $COMMON $LWJGL $VRARGS "$@" -cp "$(cat $I/t4.cp)" SpikeT23 $GAME
else
  [ -n "$KL_INSERT" ] && export DYLD_INSERT_LIBRARIES=$KL_INSERT   # set here: SIP bash drops DYLD_* from its env
  exec ${BIN:-$W/out/t4/mc-fabric-t4} -Xmx3G $COMMON -Dbaked.classpath="$(cat $I/t4.cp)" -Djava.home=$I/fgamedir \
    -Djava.awt.headless=true $LWJGL $VRARGS "$@" $GAME
fi
} > $LOG 2>&1 &
P=$!
( sleep $((T-8)); [ -n "$SAMPLE" ] && sample $P 3 -file $LOG.sample >/dev/null 2>&1; $W/out/t4/windows $P > $LOG.windows 2>&1; vmmap $P 2>/dev/null | grep -E "^__TEXT" | awk "{print \$NF}" | sort -u > $LOG.images; lsof -p $P 2>/dev/null | awk '{print $NF}' | grep -E '\.(dylib|jnilib|so)$|framework' | sort -u > $LOG.dylibs; sleep 8; kill $P 2>/dev/null; sleep 5; kill -9 $P 2>/dev/null ) &
wait $P; echo "EXIT $?"
[ "$VR" = 1 ] && mv $CFG.t4bak $CFG
echo "log: $LOG  shots: $SHOTS"; ls $SHOTS | head -20
exit 0
