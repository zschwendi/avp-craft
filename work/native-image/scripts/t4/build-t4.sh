#!/bin/bash
# build-t4.sh: the T4 GLFW stand-in for the macOS run.
#   lwjgl-fork-t4/natives/libklglfw.dylib  = scripts/t4-src/native/klglfw.c (libc/pthread/dl/mach only; EGL via libltw)
#   lwjgl-fork-t4/lwjgl-glfw-t4.jar        = stock lwjgl-glfw-3.3.3 struct + callback classes (no GLFW, GLFWNative*,
#                                            Callbacks, EventLoop, GLFWVulkan, no native sha1s)
#                                            + the stand-in (scripts/t4-src/java/org/lwjgl/glfw: GLFW, KlGlfwNative,
#                                            Callbacks, GLFWNative{EGL,Cocoa,NSGL,GLX,WGL,Win32,X11})
#   lwjgl-fork-t4/natives/                 = fork liblwjgl/_opengl/_stb + stock libopenal from lwjgl-fork/natives,
#                                            NO libglfw
#   baked/t4                               = T3 substitutions + SpikeT23 + Target_T4 (MCOpenXR.destroy null guard)
#   t4.cp / t4-ni.cp                       = t23.cp / t23-ni.cp with lwjgl-glfw-3.3.3.jar -> lwjgl-glfw-t4.jar and
#                                            baked/t23 -> baked/t4
# MCV=<version> (scripts/inst.sh): libklglfw, lwjgl-glfw-t4.jar and lwjgl-fork-t4/natives are SHARED and only rebuilt
# for 1.21.5 (or KLGLFW_REBUILD=1); other instances only get $I/t4.cp, $I/t4-ni.cp and $I/baked/t4.
set -e
. "$(dirname "$0")/../inst.sh"
S=$W/scripts/t4-src
D=$W/lwjgl-fork-t4
O=$W/out/t4
ANGLE_INC=${ANGLE_INC:-$ROOT/vendor/include}
mkdir -p $D/natives $O
if [ "$MCV" = 1.21.5 ] || [ "$KLGLFW_REBUILD" = 1 ]; then
SDK=$(xcrun --sdk macosx --show-sdk-path)
clang -dynamiclib -O2 -g -Wall -Wextra -Wno-unused-parameter -target arm64-apple-macos11 -isysroot $SDK \
  -I$G/include -I$G/include/darwin -I$ANGLE_INC -fvisibility=hidden \
  -install_name @rpath/libklglfw.dylib -o $D/natives/libklglfw.dylib $S/native/klglfw.c
# xros / xrsimulator compile check of the same source (T8 links these; not run here)
mkdir -p $O/xros
for sdk in xros xrsimulator; do
  t=arm64-apple-xros26.0; [ $sdk = xrsimulator ] && t=arm64-apple-xros26.0-simulator
  xcrun --sdk $sdk clang -dynamiclib -O2 -Wall -Wextra -Wno-unused-parameter -target $t -I$G/include -I$G/include/darwin \
    -I$ANGLE_INC -fvisibility=hidden -install_name @rpath/libklglfw.dylib -o $O/xros/libklglfw-$sdk.dylib $S/native/klglfw.c
done
for f in liblwjgl.dylib liblwjgl_opengl.dylib liblwjgl_stb.dylib libopenal.dylib; do cp $W/lwjgl-fork/natives/$f $D/natives/; done
rm -f $D/natives/libglfw.dylib
# Java stand-in
rm -rf $O/glfw-classes; mkdir -p $O/glfw-classes
unzip -qo $W/dl/lwjgl/lwjgl-glfw-3.3.3.jar 'org/lwjgl/glfw/*' -d $O/glfw-classes
( cd $O/glfw-classes/org/lwjgl/glfw && rm -f GLFW.class 'GLFW$Functions.class' Callbacks.class EventLoop*.class GLFWNative*.class GLFWVulkan*.class )
$G/bin/javac -nowarn --release 21 -d $O/glfw-classes -cp "$W/lwjgl-fork/lwjgl-fork-mac.jar:$O/glfw-classes:$(tr ':' '\n' < $W/t23.cp | grep -m1 jsr305 || true)" \
  $S/java/org/lwjgl/glfw/*.java
rm -f $D/lwjgl-glfw-t4.jar
( cd $O/glfw-classes && zip -qr $D/lwjgl-glfw-t4.jar org )
fi
# classpaths
case $MCV in
1.*)
sed -e "s#$W/dl/lwjgl/lwjgl-glfw-3.3.3.jar#$D/lwjgl-glfw-t4.jar#" -e "s#$I/baked/t23:#$I/baked/t4:#" $I/t23.cp > $I/t4.cp
sed -e "s#$W/dl/lwjgl/lwjgl-glfw-3.3.3.jar#$D/lwjgl-glfw-t4.jar#" -e "s#$I/baked/t23:#$I/baked/t4:#" $I/t23-ni.cp > $I/t4-ni.cp
grep -q lwjgl-glfw-t4.jar $I/t4.cp && grep -q lwjgl-glfw-t4.jar $I/t4-ni.cp && ! grep -q lwjgl-glfw-3.3.3 $I/t4-ni.cp
;;
*)
# 26.x (M3 stage C): no QC fork, no t23 step. Straight from $I/baked.cp per the natives agent's contract
# (work/natives/build/lwjgl-3.4.1/USAGE.md): stock LWJGL 3.4.1 Java jars with the :unsafe core (JNI, not FFM),
# lwjgl-glfw-3.4.1.jar -> klglfw/lwjgl-glfw-klglfw-3.4.1.jar, every *-natives-*.jar dropped (no stock dylib, no
# MoltenVK), jtracy natives dropped. Natives come from $LWJGL341/<platform>/ via -Dorg.lwjgl.librarypath (run.sh).
#   t4.cp    = baked/t4 : baked.cp(filtered)
#   t4-ni.cp = t4.cp minus jna, jna-platform, oshi-core, java-objc-bridge, plus the shared baked/t23-stubs
LWJGL341=${LWJGL341:-$W/../natives/build/lwjgl-3.4.1}
[ -f $LWJGL341/READY-macosx ] && [ -f $LWJGL341/klglfw/lwjgl-glfw-klglfw-3.4.1.jar ] || { echo "!! $LWJGL341 not READY (natives agent)"; exit 1; }
# The klglfw jar is APFS-cloned (cp -c: no extra disk) into $I/klglfw/ so every classpath entry lives under work/native-image
# (stage_native.sh only stages entries under it). Vivecraft's nested `org_lwjgl_*_natives-*` jars go too (Linux/Windows/
# Android openxr + openvr natives: never loaded here, the OpenXR loader is named explicitly).
mkdir -p $I/klglfw && cp -c $LWJGL341/klglfw/lwjgl-glfw-klglfw-3.4.1.jar $I/klglfw/
tr ':' '\n' < $I/baked.cp | grep -v -E -- '-natives-|_natives-|/jtracy-.*natives' \
  | sed "s#^$I/vanilla/lib/lwjgl-glfw-3\.4\.1\.jar\$#$I/klglfw/lwjgl-glfw-klglfw-3.4.1.jar#" > $I/cp.lines
grep -q -- '/lwjgl-3.4.1-unsafe.jar$' $I/cp.lines && ! grep -q -- '/lwjgl-3.4.1.jar$' $I/cp.lines && grep -q lwjgl-glfw-klglfw $I/cp.lines \
  && ! grep -q '/lwjgl-glfw-3.4.1.jar$' $I/cp.lines
{ echo -n "$I/baked/t4:"; paste -sd: $I/cp.lines; } > $I/t4.cp
{ echo "$I/baked/t4"; grep -v -E '/(jna|jna-platform|oshi-core|java-objc-bridge)-[0-9.]+\.jar$' $I/cp.lines; echo "$W/baked/t23-stubs"; } | paste -sd: - > $I/t4-ni.cp
rm $I/cp.lines
;;
esac
grep -q "$I/baked/t4:" $I/t4.cp && grep -q "$I/baked/t4:" $I/t4-ni.cp
# substitutions + harness (T3 + T4)
mkdir -p "$W/baked/t23-stubs"
STUB_SOURCES=()
while IFS= read -r -d '' source; do STUB_SOURCES+=("$source"); done < <(find "$W/scripts/t3-src/stubs" -name '*.java' -print0)
[ "${#STUB_SOURCES[@]}" -gt 0 ] || { echo "Missing T23 stub sources"; exit 1; }
"$G/bin/javac" -nowarn -d "$W/baked/t23-stubs" "${STUB_SOURCES[@]}"
rm -rf $I/baked/t4; mkdir -p $I/baked/t4
SUBST="$(ls $W/scripts/t3-src/*.java $S/subst/*.java | grep -v -F "${SUBST_SKIP:-/nothing/}") $(ls $S/subst-$MCV/*.java 2>/dev/null)"
$G/bin/javac -nowarn -d $I/baked/t4 -cp "$(cat $I/t4-ni.cp)" $SUBST
ls -la $D $D/natives $I/baked/t4
case $MCV in
  1.*) unzip -l "$D/lwjgl-glfw-t4.jar" | tail -1 ;;
  *) unzip -l "$I/klglfw/lwjgl-glfw-klglfw-3.4.1.jar" | tail -1 ;;
esac
