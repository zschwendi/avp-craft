#!/bin/bash
# build-klglfw341.sh [macosx|xrsimulator|xros ...]  (default: all three)
# klglfw (the GLFW stand-in) for Minecraft 26.x / stock LWJGL 3.4.1. Outputs (work/natives/build/lwjgl-3.4.1/):
#   klglfw/lwjgl-glfw-klglfw-3.4.1.jar  stock lwjgl-glfw-3.4.1 struct + callback classes (no GLFW, GLFW$Functions,
#                                       Callbacks, EventLoop, GLFWNative*, GLFWVulkan, native sha1s)
#                                       + the stand-in: scripts/t4-src/java/org/lwjgl/glfw/*.java with the
#                                       /*#LWJGL341 ... #LWJGL341*/ block of GLFW.java switched ON
#                                       + scripts/t4-src/java-341/org/lwjgl/glfw/*.java (GLFWVulkan: Vulkan unsupported)
#   <platform>/libklglfw.dylib          scripts/t4-src/native/klglfw.c (LWJGL-version independent; GraalVM 25 jni.h)
#   READY-<platform>                    once BUILT-lwjgl-<platform> (build-lwjgl341-natives.sh) + jar + libklglfw exist
# 1.21.x is untouched: scripts/t4/build-t4.sh compiles the same GLFW.java against the QC 3.3.3 fork with the block
# still a comment, and t8b/build-natives.sh keeps building the same klglfw.c into out/t8b.
# Coverage gate: glfwcover.py checks every org/lwjgl/glfw member the 26.2 classpath references (glfwrefs-26.2.txt,
# regenerated from work/native-image/inst-26.2 when present) resolves in the jar.
set -e
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
W="$ROOT/work/native-image"
N="$ROOT/work/natives"
G=${G:-$W/graalvm/graalvm-jdk-25.0.4+7.1/Contents/Home}
MV=$N/downloads/lwjgl-3.4.1-maven
S=$W/scripts/t4-src
T2=$W/scripts/t2
OUT=$N/build/lwjgl-3.4.1
B=$OUT/obj-klglfw; K=$OUT/klglfw
ANGLE_INC=${ANGLE_INC:-$ROOT/vendor/include}
rm -rf $B; mkdir -p $B/src/org/lwjgl/glfw $B/anno/javax/annotation $B/classes $B/anno-classes $K

# --- Java stand-in -------------------------------------------------------------------------------------------------
unzip -qo $MV/lwjgl-glfw-3.4.1.jar 'org/lwjgl/glfw/*' -d $B/classes
( cd $B/classes/org/lwjgl/glfw && rm -f GLFW.class 'GLFW$Functions.class' Callbacks.class EventLoop*.class GLFWNative*.class GLFWVulkan*.class )
cp $S/java/org/lwjgl/glfw/*.java $S/java-341/org/lwjgl/glfw/*.java $B/src/org/lwjgl/glfw/
grep -q '^    /\*#LWJGL341$' $B/src/org/lwjgl/glfw/GLFW.java && grep -q '^    #LWJGL341\*/$' $B/src/org/lwjgl/glfw/GLFW.java
sed -i '' -e '/^    \/\*#LWJGL341$/d' -e '/^    #LWJGL341\*\/$/d' $B/src/org/lwjgl/glfw/GLFW.java
# compile-only javax.annotation stubs (the 3.3.3 fork jar shades jsr305; stock 3.4.1 does not). CLASS retention:
# never needed at run time, not put in the jar.
for a in Nullable Nonnull; do printf 'package javax.annotation;\npublic @interface %s { }\n' $a > $B/anno/javax/annotation/$a.java; done
$G/bin/javac -nowarn -d $B/anno-classes $B/anno/javax/annotation/*.java
$G/bin/javac -nowarn --release 21 -d $B/classes -cp "$MV/lwjgl-3.4.1-unsafe.jar:$MV/lwjgl-vulkan-3.4.1.jar:$B/classes:$B/anno-classes" \
  $B/src/org/lwjgl/glfw/*.java
rm -f $K/lwjgl-glfw-klglfw-3.4.1.jar
( cd $B/classes && zip -qr -X $K/lwjgl-glfw-klglfw-3.4.1.jar org )
echo "jar: $K/lwjgl-glfw-klglfw-3.4.1.jar ($(unzip -l $K/lwjgl-glfw-klglfw-3.4.1.jar | tail -1 | awk '{print $2}') entries)"

# --- coverage against the 26.2 classpath -------------------------------------------------------------------------
I=$W/inst-26.2
if [ -f $I/vanilla/client-26.2.jar ] && [ -d $I/fgamedir/mods ]; then
  python3 -I - "$I" "$B/refs" <<'PY'
import zipfile, os, sys, io
I, S = sys.argv[1], sys.argv[2]; os.makedirs(S + '/nested', exist_ok=True)
cp = [I + '/vanilla/client-26.2.jar']
def walk(path, data=None):
    z = zipfile.ZipFile(io.BytesIO(data) if data else path)
    for n in z.namelist():
        if n.startswith('META-INF/jars/') and n.endswith('.jar'):
            b = z.read(n); out = os.path.join(S, 'nested', os.path.basename(n)); open(out, 'wb').write(b); cp.append(out); walk(None, b)
for m in sorted(os.listdir(I + '/fgamedir/mods')):
    if m.endswith('.jar'): p = I + '/fgamedir/mods/' + m; cp.append(p); walk(p)
for l in open(I + '/vanilla.cp').read().strip().split(':'):
    if 'lwjgl' not in os.path.basename(l) and l not in cp: cp.append(l)
open(S + '/refs.cp', 'w').write(':'.join(cp))
PY
  python3 -I $W/scripts/t4/glfwrefs.py $B/refs/refs.cp | sed "s#$B/refs/nested/##; s#$I/##" > $T2/glfwrefs-26.2.txt
  echo "glfwrefs-26.2.txt regenerated from $I ($(wc -l < $T2/glfwrefs-26.2.txt) refs)"
fi
python3 -I $T2/glfwcover.py $T2/glfwrefs-26.2.txt $K/lwjgl-glfw-klglfw-3.4.1.jar $MV/lwjgl-3.4.1-unsafe.jar $MV/lwjgl-vulkan-3.4.1.jar

# --- libklglfw per platform -----------------------------------------------------------------------------------------
CF="-O2 -g -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -I$G/include -I$G/include/darwin -I$ANGLE_INC"
for P in ${*:-macosx xrsimulator xros}; do
  case $P in macosx) t=arm64-apple-macos11;; xros) t=arm64-apple-xros26.0;; xrsimulator) t=arm64-apple-xros26.0-simulator;;
    *) echo "bad platform $P"; exit 2;; esac
  mkdir -p $OUT/$P
  xcrun --sdk $P clang -dynamiclib $CF -target $t -install_name @rpath/libklglfw.dylib -o $OUT/$P/libklglfw.dylib $S/native/klglfw.c
  rm -rf $OUT/$P/libklglfw.dylib.dSYM
  if [ $P = xrsimulator ]; then codesign -f -s - $OUT/$P/libklglfw.dylib 2>/dev/null; fi
  echo "libklglfw $P: $(xcrun vtool -show-build $OUT/$P/libklglfw.dylib | awk '/platform/{print $2}') $(stat -f%z $OUT/$P/libklglfw.dylib) B"
  if [ -f $OUT/BUILT-lwjgl-$P ] && [ -f $OUT/$P/liblwjgl.dylib ] && [ -f $K/lwjgl-glfw-klglfw-3.4.1.jar ]; then
    date '+%Y-%m-%d %H:%M:%S %Z' > $OUT/READY-$P; echo "READY-$P"
  else
    echo "NOT READY-$P (need BUILT-lwjgl-$P, liblwjgl.dylib and the klglfw jar)"
  fi
done
