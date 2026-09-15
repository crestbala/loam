#!/bin/sh
# Native HTTP API on 127.0.0.1:8080
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
  echo "run.sh: could not find the loam repo (Makefile + packages/loam/src/)" >&2
  exit 1
fi
if [ ! -x "$REPO/bin/loam" ]; then
  echo "run.sh: building loam"
  make -C "$REPO" -j4
fi
mkdir -p "$HERE/build"
echo "backend: compiling server.loam"
"$REPO/bin/loam" "$HERE/server.loam" -o "$HERE/build/server"
echo "backend: http://127.0.0.1:8080  (Counter.Hello  Counter.Stats)"
exec "$HERE/build/server"
