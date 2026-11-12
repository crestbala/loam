#!/bin/sh
# macOS Cocoa UI for the zeus gallery.
set -e
# Ignore a headless export left over from `nob test`; we want a real window.
unset ZEUS_HEADLESS LOAM_HEADLESS MAYA_HEADLESS
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
d=$HERE
REPO=
while [ "$d" != / ]; do
  if [ -f "$d/nob.sh" ] && [ -d "$d/packages/loam/compiler" ]; then
    REPO=$d
    break
  fi
  d=$(CDPATH= cd -- "$d/.." && pwd)
done
if [ -z "$REPO" ]; then
  echo "run.sh: could not find the loam repo (nob.sh + packages/loam/compiler/)" >&2
  exit 1
fi
if [ ! -x "$REPO/bin/loamc" ]; then
  echo "run.sh: building loam"
  (cd "$REPO" && ./nob.sh)
fi
echo "macos: Cocoa desktop"
# Assets (fonts/) are referenced relative to the gallery directory.
cd "$HERE/.."
exec "$REPO/bin/loamc" --target=native --run "$HERE/app.loam"
