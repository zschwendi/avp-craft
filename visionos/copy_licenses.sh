#!/bin/sh
# Copy visionos/Licenses into the built .app.
#
# visionOS has no Contents/ folder. UNLOCALIZED_RESOURCES_FOLDER_PATH is the
# .app itself (relative to TARGET_BUILD_DIR), so this lands at
# KleptonVivecraft.app/Licenses and is inside the IPA.
#
# The folder is authored in the tree (NOTICES.txt plus the license texts).
# This script only copies. A missing index fails the app build.
set -eu

SRC="${PROJECT_DIR}/Licenses"
DEST="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/Licenses"

if [ ! -f "$SRC/NOTICES.txt" ]; then
  echo "error: $SRC/NOTICES.txt is missing. The app bundle ships license texts for the baked mods." >&2
  exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST"
cp -R "$SRC/." "$DEST/"
# Drop anything the copy picked up from a local editor.
find "$DEST" -name '.DS_Store' -delete
n=$(find "$DEST" -type f | wc -l | tr -d ' ')
echo "copy_licenses: $n files -> $DEST"
