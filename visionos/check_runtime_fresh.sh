#!/bin/sh
# Fail the app build when the C runtime it LINKS is older than the C runtime in
# the tree — because the app does not build that runtime, and nothing else was
# checking.
#
# The app target compiles visionos/Sources/*.swift and kl_app.c itself, and
# links `build/Klepton.xcframework` for everything under runtime/. Those two
# halves are produced by different tools: Xcode builds the first, `make xros`
# builds the second. So an `xcodebuild` that does not go through
# `visionos/run.sh` recompiles the app against a runtime archive of whatever age
# happens to be on disk, links cleanly, installs cleanly, and runs the OLD
# runtime — while the app-side half of the very same commit is fresh.
#
# That is not hypothetical. 2026-08-12, phase 12: five runtime fixes were
# committed between 14:10 and 14:29 and an app built at 14:31 shipped the 13:33
# archive. The log rotation and the jetsam tripwire (visionos/Sources/kl_app.c)
# were in the build; the semaphore free list, the x18 race and the layer-storm
# dedup (runtime/) were not. The device then reproduced all the old deaths, the
# logs showed pre-fix diagnostic strings, and the session was spent re-deriving
# a bug that had already been fixed. A stale link is the most expensive kind of
# silence: everything succeeds and the evidence lies.
#
# Compared against the ARCHIVE inside the xcframework, never the .xcframework
# directory itself. A directory's mtime moves when anything is written inside it
# — Finder dropping a .DS_Store is enough — so a directory timestamp can report
# "fresh" for contents that are not, which is the same failure one level up.
set -eu

ROOT=$(cd "${PROJECT_DIR:-$(dirname "$0")}/.." && pwd)
XCF="$ROOT/build/Klepton.xcframework"

fail() {
    echo "error: $1" >&2
    echo "error: the app links build/Klepton.xcframework but does not build it." >&2
    echo "error: run \`make xros\` from $ROOT, then build again." >&2
    exit 1
}

[ -d "$XCF" ] || fail "build/Klepton.xcframework is missing."

# Both slices: a device build that skipped the simulator archive still leaves the
# xcframework half stale, and the next simulator run inherits it.
ARCHIVES=$(find "$XCF" -name libklepton.a)
[ -n "$ARCHIVES" ] || fail "build/Klepton.xcframework contains no libklepton.a."

for A in $ARCHIVES; do
    # The sources AND the headers: KLX_TSD_SLOT and every other cross-file
    # constant lives in a header, so a header-only edit changes what the archive
    # should contain while changing no .c file at all. `-headers runtime` also
    # copies them into the xcframework for the app to compile against, so a stale
    # archive and a stale header copy are the same staleness with two symptoms.
    NEWER=$(find "$ROOT/runtime" \( -name '*.c' -o -name '*.h' -o -name '*.S' \) \
                 -newer "$A" -print 2>/dev/null | head -5)
    if [ -n "$NEWER" ]; then
        echo "error: $(basename "$(dirname "$A")")/libklepton.a is older than:" >&2
        echo "$NEWER" | sed 's|^|error:   |' >&2
        fail "the linked runtime archive is stale."
    fi
done

echo "  runtime archive is current with runtime/*.{c,h,S}"
