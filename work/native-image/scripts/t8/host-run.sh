#!/bin/bash
# host-run.sh <seconds> [extra runtime args] -- T8a host sanity: out/t8/macosx/klmc-host dlopens out/t8/macosx/
# libminecraft.dylib and calls klmc_main on an 8 MB pthread with EXACTLY the T4 native command line, by running
# scripts/t4/run.sh with BIN=klmc-host (same env: LTW_EGL_LIBRARY, KLGLFW_SHOT_*; same -D props: libname=ltw,
# librarypath=lwjgl-fork-t4/natives, baked.classpath, quick-play world). All run.sh env knobs work (NOWORLD, SHOT_AT, ...).
# Log: logs/t8-<TAG>.txt (default TAG=host-world), shots: out/t4/shots-t8-<TAG>.
set -euo pipefail
W="$(cd "$(dirname "$0")/../.." && pwd)"
S=$(cd "$(dirname "$0")" && pwd)
O=${T8OUT:-$W/out/t8}/macosx
[ -f $O/libminecraft.dylib ] || { echo "run relink-xros.sh macosx first"; exit 1; }
clang -O2 -Wall -target arm64-apple-macos11 -isysroot $(xcrun --sdk macosx --show-sdk-path) -o $O/klmc-host $S/klmc-host.c
T=${1:-75}; shift || true
TAG=t8-${TAG:-host-world}
export KLMC_DYLIB=$O/libminecraft.dylib
TAG=$TAG LOG=$W/logs/$TAG.txt BIN=$O/klmc-host exec $W/scripts/t4/run.sh native $T "$@"
