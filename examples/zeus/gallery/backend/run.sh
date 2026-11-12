#!/bin/sh
# Native HTTP listener on 127.0.0.1:8081 (gallery UI does not call it yet).
set -e
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
mkdir -p "$HERE/build"
echo "backend: compiling server.loam"
"$REPO/bin/loamc" "$HERE/server.loam" -o "$HERE/build/server"
echo "backend: http://127.0.0.1:8081"
exec "$HERE/build/server"
