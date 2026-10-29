#!/bin/bash
# build-t23.sh: compile the T3 substitutions + SpikeT23 harness into baked/t23, the signature-only stubs into
# baked/t23-stubs (native-image classpath only), and the two macOS stand-in dylibs
# (libvloader stub, libopenxr_loader probe) into out/t23/natives.
# MCV=<version> (scripts/inst.sh): baked/t23 is per instance ($I/baked/t23, compiled against $I/t23.cp); the stubs and
# the two stand-in dylibs are SHARED and only rebuilt for 1.21.5 (or when missing).
set -e
. "$(dirname "$0")/../inst.sh"
X=$W/out/openxr-sdk/OpenXR-SDK-release-1.1.49
rm -rf $I/baked/t23; mkdir -p $I/baked/t23 $W/out/t23/natives
$G/bin/javac -nowarn -d $I/baked/t23 -cp "$(cat $I/t23.cp)" $W/scripts/t3-src/*.java
if [ "$MCV" = 1.21.5 ] || [ ! -d $W/baked/t23-stubs ]; then
rm -rf $W/baked/t23-stubs; mkdir -p $W/baked/t23-stubs
$G/bin/javac -nowarn -d $W/baked/t23-stubs $(find $W/scripts/t3-src/stubs -name '*.java')
SDK=$(xcrun --sdk macosx --show-sdk-path)
clang -dynamiclib -O2 -target arm64-apple-macos11 -isysroot $SDK -I$G/include -I$G/include/darwin \
  -o $W/out/t23/natives/libvloader.dylib $W/scripts/t3-src/native/vloader_stub.c
clang -dynamiclib -O2 -target arm64-apple-macos11 -isysroot $SDK -I$X/include \
  -o $W/out/t23/natives/libopenxr_loader.dylib $W/scripts/t3-src/native/openxr_probe.c
fi
ls -la $I/baked/t23 $W/out/t23/natives
