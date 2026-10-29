#!/bin/bash
# Build flat vanilla classpath for macOS arm64 -> $I/vanilla.cp   (MCV=<version>: instance, see scripts/inst.sh)
#   1.21.5: copied from the Klepton userdata instance (original behaviour). Other versions: from $I/files
#   (scripts/m3a/fetch-inst.py). Stock LWJGL 3.3.3 from dl/lwjgl for 1.21.x; 26.x: LWJGL 3.4.1 from the version json.
set -e
. "$(dirname "$0")/inst.sh"
mkdir -p $I/vanilla/lib $I/gamedir
if [ "$MCV" = 1.21.5 ]; then
  V="$HOME/Library/Application Support/Klepton/userdata/vivecraft/files"
  cp -n "$V/versions/1.21.5/client.jar" $I/vanilla/client-1.21.5.jar || true
  grep '^libraries/' "$V/versions/1.21.5/classpath.txt" | grep -v -e fabricmc -e 'natives-linux' -e 'natives-windows' -e 'native-epoll' | sort -u | while read l; do cp -n "$V/$l" $I/vanilla/lib/ || true; done
  ln -sfn "$V/assets" $W/assets
else
  V=$I/files
  cp -n $V/versions/$MCV/client.jar $I/vanilla/client-$MCV.jar || true
  grep -v -e fabricmc -e 'natives-linux' -e 'natives-windows' -e 'native-epoll' $V/versions/$MCV/libs-vanilla.txt | sort -u | while read l; do cp -n "$V/$l" $I/vanilla/lib/ || true; done
fi
case $MCV in
  1.*) cp -n $W/dl/lwjgl/*.jar $I/vanilla/lib/ || true;;
  *)   # 26.x: LWJGL 3.4.1 from the version json (fetch-inst.py libs-lwjgl.txt: java jars, :unsafe core,
       # natives-macos-arm64) for the Knot/bake run on stock GLFW + Apple GL. The client jar is SIGNED (MOJANGCS):
       # strip the signature so the bake overlay's classes can share its packages on the baked classpath.
       while read l; do cp -n "$V/$l" $I/vanilla/lib/ || true; done < $V/versions/$MCV/libs-lwjgl.txt
       zip -q -d $I/vanilla/client-$MCV.jar 'META-INF/MOJANGCS.SF' 'META-INF/MOJANGCS.RSA' >/dev/null 2>&1 || true;;
esac
{ echo -n $I/vanilla/client-$MCV.jar; for j in $I/vanilla/lib/*.jar; do echo -n ":$j"; done; } > $I/vanilla.cp
