#!/bin/bash
# Build pinned OpenAL Soft for visionOS; outputs consumed by build-lwjgl341-natives.sh.
# Source/syntax reviewed for the public source release; full clean build not rerun.
set -euo pipefail
N="$(cd "$(dirname "$0")/.." && pwd)"
DOWNLOAD="$N/downloads/openal-soft-1.25.2.tar.gz"
EXPECTED=fb27e5839aa11f0e5b9d33756965291fad5d6909ab928ea1f796f4a1a6877894
for dep in cmake curl xcrun shasum; do command -v "$dep" >/dev/null || { echo "missing $dep" >&2; exit 1; }; done
mkdir -p "$N/downloads"
if [ ! -f "$DOWNLOAD" ]; then
  curl -fL --retry 3 https://codeload.github.com/kcat/openal-soft/tar.gz/refs/tags/1.25.2 -o "$DOWNLOAD.tmp"
  printf '%s  %s\n' "$EXPECTED" "$DOWNLOAD.tmp" | shasum -a 256 -c -
  mv "$DOWNLOAD.tmp" "$DOWNLOAD"
fi
printf '%s  %s\n' "$EXPECTED" "$DOWNLOAD" | shasum -a 256 -c -
# Re-extract the verified archive into this helper's generated tree; never patch upstream source.
WRAPPER="$N/build/openal-1.25.2-source"
mkdir -p "$WRAPPER"
tar -xzf "$DOWNLOAD" -C "$WRAPPER"
cat > "$WRAPPER/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
project(AVPcraftOpenAL LANGUAGES C CXX)
foreach(option UTILS EXAMPLES TESTS INSTALL INSTALL_CONFIG INSTALL_HRTF_DATA INSTALL_AMBDEC_PRESETS INSTALL_EXAMPLES INSTALL_UTILS UPDATE_BUILD_VERSION ENABLE_MODULES)
  set(ALSOFT_${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(ALSOFT_REQUIRE_COREAUDIO ON CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_COREAUDIO ON CACHE BOOL "" FORCE)
set(ALSOFT_OSX_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(ALSOFT_WERROR ON CACHE BOOL "" FORCE)
# Static try-compiles can otherwise falsely report that libatomic exists.
set(HAVE_LIBATOMIC OFF CACHE BOOL "" FORCE)
foreach(backend PIPEWIRE PULSEAUDIO ALSA OSS SOLARIS SNDIO WASAPI DSOUND WINMM JACK OBOE OPENSL PORTAUDIO SDL3 SDL2 WAVE)
  set(ALSOFT_BACKEND_${backend} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(openal-soft-1.25.2)
# SDK27 enum-group deprecation and new real-time function-effect annotations.
# Preserve upstream source; only relax these diagnostics in CoreAudio's translation unit.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/openal-soft-1.25.2/alc/backends/coreaudio.cpp"
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/openal-soft-1.25.2"
  PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-anon-enum-enum-conversion;-Wno-error=function-effects")
set_property(TARGET OpenAL PROPERTY VERSION)
set_property(TARGET OpenAL PROPERTY SOVERSION)
set_target_properties(OpenAL PROPERTIES OUTPUT_NAME openal MACOSX_RPATH TRUE
  INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
CMAKE
for sdk in ${*:-xros xrsimulator}; do
  case "$sdk" in
    xros) target=arm64-apple-xros26.0; expected_platform=11 ;;
    xrsimulator) target=arm64-apple-xros26.0-simulator; expected_platform=12 ;;
    *) echo "usage: $0 [xros|xrsimulator ...]" >&2; exit 2 ;;
  esac
  BUILD="$N/build/openal-1.25.2-$sdk"
  OUT="$N/openal/$sdk"
  cmake -S "$WRAPPER" -B "$BUILD" -G 'Unix Makefiles' \
    -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT="$(xcrun --sdk "$sdk" --show-sdk-path)" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_C_COMPILER_TARGET="$target" -DCMAKE_CXX_COMPILER_TARGET="$target" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_SHARED_LINKER_FLAGS='-Wl,-no_adhoc_codesign' -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" --target OpenAL --parallel "${JOBS:-2}"
  mkdir -p "$OUT"
  cp -L "$BUILD/openal-soft-1.25.2/libopenal.dylib" "$OUT/libopenal.dylib"
  install_name_tool -id @rpath/libopenal.dylib "$OUT/libopenal.dylib"
  platform=$(otool -l "$OUT/libopenal.dylib" | awk '/LC_BUILD_VERSION/{b=1} b&&/platform/{print $2;exit}')
  [ "$platform" = "$expected_platform" ] || { echo "wrong platform: $platform" >&2; exit 1; }
  xcrun vtool -show-build "$OUT/libopenal.dylib"
  shasum -a 256 "$OUT/libopenal.dylib"
done
