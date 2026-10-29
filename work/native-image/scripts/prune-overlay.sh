#!/bin/bash
# Remove library classes (unchanged copies Knot passed through) from the bake overlay; keep game/mod classes.
# (superseded by prune_overlay.py, which keeps mixin-modified library classes). MCV: see scripts/inst.sh
. "$(dirname "$0")/inst.sh"
for j in $I/vanilla/lib/*.jar $I/fabric/lib/*.jar; do unzip -Z1 $j 2>/dev/null | grep '\.class$'; done | sort -u > $I/bake/lib-classes.txt
cd $I/bake/classes && n=0; while read c; do [ -f "$c" ] && rm "$c" && n=$((n+1)); done < $I/bake/lib-classes.txt; echo "pruned $n"
find $I/bake/classes -type d -empty -delete
find $I/bake/classes -name '*.class' | wc -l
