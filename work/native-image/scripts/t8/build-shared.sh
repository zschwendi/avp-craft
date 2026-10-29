#!/bin/bash
# build-shared.sh [extra native-image args] -> out/t8/ni/libminecraft.dylib (+ the kept object file in out/t8/tmp/SVM-*)
# T8a: the T4 image (t4-ni.cp, config/t23-trim + t23-extra, -O1, main SpikeT23) as a native-image --shared library.
# With a main class, --shared exports `run_main(int argc, char **argv)` (argv[0] = program name; -D/-X/-XX/-R: parsed
# exactly like the T4 executable's command line). klmc_main/klmc_version are a C shim added at relink time
# (scripts/t8/klmc_entry.c via relink-xros.sh), not Java entry points.
# Build time + max RSS -> logs/t8-build-shared.txt (/usr/bin/time -l).
# qc 04 defaults: -O2 (OPT=-O1 for the old image; __TEXT 146 MB links fine with Xcode 27 ld, B5 no longer bites) and
#   -H:InitialCollectionPolicy=BySpaceAndTime (GCPOLICY=Adaptive for the old one): with launch.txt's -Xmn1g it cut
#   host GC pause time 8.5 -> 1.6 s per minute of new-terrain flight and full GCs 18 -> 2 (the Adaptive policy ignores
#   -Xmn and keeps the young gen at ~200 MB). Env: T8OUT=<dir> (default out/t8; relink-xros.sh / host-run.sh take the same T8OUT),
#   LOGTAG=<name> (log logs/t8-build-<name>.txt), CONFIG_DIRS, CPFILE. Extra configs that are single files rather than
#   config dirs (the SVC round's config/extra/svc-jni.json + svc-resources.json) are added when present.
# M3 1.21.11 (2026-10-12): the default OPT is -O1 for any instance classpath (CPFILE under inst-*): at -O2 the 1.21.11
#   code section is 156 MB (one atom; 149 MB without ML profiles, 148 MB with inlining/unrolling cut), ld64 cannot reach
#   relocated call targets across it ("B/BL out of range ... max is +/-128MB" on throw_internal_error's island), and
#   GraalVM's fallback ld64.lld 16 cannot read the macOS 27 SDK .tbd files (arm64e.x1). 1.21.5 at -O2 (145 MB) links.
#   Netty 4.2's netty-codec-http ships `--initialize-at-build-time=io.netty`, which drags log4j into the build-time
#   heap (AdaptivePoolingAllocator$Chunk -> PlatformDependent -> slf4j); its native-image.properties is excluded below
#   (the regex only matches 4.2 jars, so the 1.21.5 build is unchanged).
set -euo pipefail
W="$(cd "$(dirname "$0")/../.." && pwd)"
. $W/scripts/jdk.sh   # G by MCV: 21.0.12 for 1.21.x, 25.0.4 for 26.x
O=${T8OUT:-$W/out/t8}
case ${CPFILE:-} in */inst-*) DEFOPT=-O1;; *) DEFOPT=-O2;; esac
LOGF=$W/logs/t8-build-${LOGTAG:-shared}.txt
EXTRA=()
[ -f $W/config/extra/svc-jni.json ] && EXTRA+=(-H:JNIConfigurationFiles=$W/config/extra/svc-jni.json)
[ -f $W/config/extra/svc-resources.json ] && EXTRA+=(-H:ResourceConfigurationFiles=$W/config/extra/svc-resources.json)
CONFIG_DIRS=${CONFIG_DIRS:-$W/config/t23-trim,$W/config/t23-extra}
IFS=',' read -r -a CONFIG_PATHS <<< "$CONFIG_DIRS"
for dir in "${CONFIG_PATHS[@]}"; do
  [ -d "$dir" ] || { echo "Missing native-image configuration directory: $dir"; exit 1; }
  for f in "$dir"/*.json; do
    [ -f "$f" ] || continue
    python3 -I -m json.tool "$f" >/dev/null || { echo "bad json: $f"; exit 1; }
  done
done
for arg in ${EXTRA[@]+"${EXTRA[@]}"}; do
  f=${arg#*=}
  python3 -I -m json.tool "$f" >/dev/null || { echo "bad json: $f"; exit 1; }
done
rm -rf $O/ni $O/tmp; mkdir -p $O/ni $O/tmp $W/logs
cd $O/ni
/usr/bin/time -l $G/bin/native-image -J-Xmx28g \
  -cp "$(cat ${CPFILE:-$W/t4-ni.cp})" \
  --no-fallback -H:+ReportExceptionStackTraces --enable-url-protocols=http,https,jar \
  -H:+UnlockExperimentalVMOptions --enable-monitoring=jfr \
  "-H:ConfigurationFileDirectories=$CONFIG_DIRS" \
  --exclude-config '.*/netty-codec-http-4\.2\.[0-9.]*Final\.jar' '.*native-image\.properties' \
  ${OPT:-$DEFOPT} -H:InitialCollectionPolicy=${GCPOLICY:-BySpaceAndTime} --shared -H:TempDirectory=$O/tmp ${EXTRA[@]+"${EXTRA[@]}"} "$@" \
  -o libminecraft -H:Class=SpikeT23 2>&1 | tee $LOGF
ls -la $O/ni; ls -la $O/tmp/SVM-*/
