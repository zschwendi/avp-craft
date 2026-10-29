#!/bin/bash
# runfabric.sh <seconds> [jvm args]; GAMEARGS env for extra game args. Runs Fabric (Knot) on GraalVM JVM.
# MCV=<version> picks the instance (scripts/inst.sh). The bake = this run with -javaagent:$W/bakeagent/bakeagent.jar=$I/bake
. "$(dirname "$0")/inst.sh"
T=$1; shift
cd $I/fgamedir
$G/bin/java -XstartOnFirstThread -Xmx3G "$@" -cp "$(cat $I/fabric.cp)" ${MAIN:-net.fabricmc.loader.impl.launch.knot.KnotClient} \
  --accessToken 0 --username spike --uuid 00000000-0000-0000-0000-000000000000 --version $MCV \
  --gameDir $I/fgamedir --assetsDir $ASSETS --assetIndex $AIDX $GAMEARGS &
P=$!
( sleep $T; kill $P 2>/dev/null; sleep 5; kill -9 $P 2>/dev/null ) &
wait $P; echo "EXIT $?"
