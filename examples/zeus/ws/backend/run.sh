#!/bin/sh
# Native WebSocket push server on 127.0.0.1:8080 (one client, ticks).
set -e
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
  echo "run.sh: could not find the yuga repo (Makefile + packages/loam/src/)" >&2
  exit 1
fi
if [ ! -x "$REPO/bin/yugac" ]; then
  echo "run.sh: building yugac"
  make -C "$REPO" -j4
fi
mkdir -p "$HERE/build"
echo "backend: compiling server.loam"
"$REPO/bin/yugac" "$HERE/server.loam" -o "$HERE/build/server"
echo "backend: ws://127.0.0.1:8080/ws"
exec "$HERE/build/server"
