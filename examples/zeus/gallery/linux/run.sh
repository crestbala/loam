#!/bin/sh
# Linux X11 UI for the zeus gallery. Run on Linux (needs cc and libx11-dev).
set -e
# Ignore a headless export left over from `make test`; we want a real window.
unset ZEUS_HEADLESS LOAM_HEADLESS MAYA_HEADLESS
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
d=$HERE
REPO=
while [ "$d" != / ]; do
  if [ -f "$d/Makefile" ] && [ -d "$d/packages/loam/src" ]; then
    REPO=$d
    break
  fi
  d=$(CDPATH= cd -- "$d/.." && pwd)
done
if [ -z "$REPO" ]; then
  echo "run.sh: could not find the loam repo (Makefile + packages/loam/src/)" >&2
  exit 1
fi
if [ "$(uname -s)" != Linux ]; then
  echo "run.sh: the X11 host builds on Linux; on another OS use ./kit.sh and build the kit there" >&2
  exit 1
fi
if [ ! -x "$REPO/bin/loamc" ]; then
  echo "run.sh: building loam"
  make -C "$REPO" -j4
fi
echo "linux: X11 desktop"
cd "$HERE/.."
exec "$REPO/bin/loamc" --target=native --run "$HERE/app.loam"
