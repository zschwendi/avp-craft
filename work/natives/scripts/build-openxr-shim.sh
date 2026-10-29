#!/bin/bash
# T6: build libopenxr_loader.dylib + libvloader.dylib for xros and xrsimulator. kl_openxr_sym is left
# undefined (-Wl,-U: flat lookup at load time, resolved from the Klepton app image), weak so a missing
# runtime gives XR_ERROR_RUNTIME_UNAVAILABLE instead of a dyld abort. jni.h from the GraalVM JDK.
set -euo pipefail
N="$(cd "$(dirname "$0")/.." && pwd)"; D="$N/openxr-shim"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
G="${G:-$ROOT/work/native-image/graalvm/graalvm-jdk-25.0.4+7.1/Contents/Home}"
for S in xros xrsimulator; do
  case $S in xros) T=arm64-apple-xros26.0;; xrsimulator) T=arm64-apple-xros26.0-simulator;; esac
  O="$D/$S"; mkdir -p "$O"
  CC="xcrun -sdk $S clang -target $T -O2 -Wall -fvisibility=hidden -dynamiclib"
  $CC "$D/openxr_loader_shim.c" -install_name @rpath/libopenxr_loader.dylib -Wl,-U,_kl_openxr_sym -Wl,-U,_kl_openxr_set_spec_input -o "$O/libopenxr_loader.dylib"
  $CC "$D/vloader_shim.c" -I"$G/include" -I"$G/include/darwin" -install_name @rpath/libvloader.dylib -o "$O/libvloader.dylib"
  # link check against a stub of the runtime symbol (stands in for the Klepton app image)
  printf 'void *kl_openxr_sym(const char *n){(void)n;return 0;}\nvoid kl_openxr_set_spec_input(int on){(void)on;}\n' > "$O/kl_openxr_stub.c"
  xcrun -sdk $S clang -target $T -dynamiclib "$O/kl_openxr_stub.c" -install_name @rpath/libkl_openxr_stub.dylib -o "$O/libkl_openxr_stub.dylib"
  xcrun -sdk $S clang -target $T -O2 -fvisibility=hidden -dynamiclib "$D/openxr_loader_shim.c" "$O/libkl_openxr_stub.dylib" \
     -install_name @rpath/libopenxr_loader.dylib -o "$O/libopenxr_loader.stublinked.dylib"
  echo "OK $S"
done
