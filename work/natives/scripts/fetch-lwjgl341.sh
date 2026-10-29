#!/bin/bash
# fetch-lwjgl341.sh: LWJGL 3.4.1 source tag + Maven Central jars into work/natives/downloads (see PROVENANCE.md).
set -e
N="$(cd "$(dirname "$0")/.." && pwd)"
D=$N/downloads
[ -d $D/lwjgl3-3.4.1/.git ] || git clone -q --depth 1 -b 3.4.1 https://github.com/LWJGL/lwjgl3 $D/lwjgl3-3.4.1
test "$(git -C $D/lwjgl3-3.4.1 rev-parse HEAD)" = b800ccffab14396fc529ddb6c931b7c5c5226763
M=$D/lwjgl-3.4.1-maven; mkdir -p $M
R=https://repo1.maven.org/maven2/org/lwjgl
get() { [ -f $M/$2 ] || curl -sfL -o $M/$2 $R/$1/3.4.1/$2 || { rm -f $M/$2; echo "  (absent: $2)"; }; }
get lwjgl lwjgl-3.4.1.jar; get lwjgl lwjgl-3.4.1-unsafe.jar; get lwjgl lwjgl-3.4.1-natives-macos-arm64.jar
for m in opengl stb openal openxr glfw jemalloc tinyfd freetype vulkan; do
  get lwjgl-$m lwjgl-$m-3.4.1.jar; get lwjgl-$m lwjgl-$m-3.4.1-natives-macos-arm64.jar
done
( cd $M && shasum -a 256 *.jar > SHA256SUMS )
ls -la $M
