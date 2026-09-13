#!/bin/sh
# macOS Cocoa UI for the zeus gallery.
set -e
# Ignore a headless export left over from `make test`; we want a real window.
unset ZEUS_HEADLESS YUGA_HEADLESS MAYA_HEADLESS
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
d=$HERE
REPO=
while [ "$d" != / ]; do
  if [ -f "$d/Makefile" ] && [ -d "$d/packages/compiler/src" ]; then
    REPO=$d
    break
  fi
  d=$(CDPATH= cd -- "$d/.." && pwd)
done
if [ -z "$REPO" ]; then
  echo "run.sh: could not find the yuga repo (Makefile + packages/compiler/src/)" >&2
  exit 1
fi
if [ ! -x "$REPO/bin/yugac" ]; then
  echo "run.sh: building yugac"
  make -C "$REPO" -j4
fi
echo "macos: Cocoa desktop"
exec "$REPO/bin/yugac" --target=native --run "$HERE/app.yuga"
