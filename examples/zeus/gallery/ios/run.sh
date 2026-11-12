#!/bin/sh
# iOS Simulator UI for the zeus gallery (pure UI: no backend needed).
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
echo "ios: Simulator (pure UI, no backend)"
exec "$REPO/bin/loamc" --target=ios --run "$HERE/app.loam"
