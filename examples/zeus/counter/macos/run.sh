#!/bin/sh
# macOS Cocoa UI. Starts the gRPC-Web backend on :8080 if it is not up.
set -e
# Ignore a headless export left over from `make test`; we want a real window.
unset ZEUS_HEADLESS YUGA_HEADLESS MAYA_HEADLESS
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
d=$HERE
REPO=
BACK_PID=
STARTED_BACK=0

cleanup() {
  if [ "$STARTED_BACK" = 1 ] && [ -n "$BACK_PID" ]; then
    kill "$BACK_PID" 2>/dev/null || true
    wait "$BACK_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

while [ "$d" != / ]; do
  if [ -f "$d/Makefile" ] && [ -d "$d/packages/yuga/src" ]; then
    REPO=$d
    break
  fi
  d=$(CDPATH= cd -- "$d/.." && pwd)
done
if [ -z "$REPO" ]; then
  echo "run.sh: could not find the yuga repo (Makefile + packages/yuga/src/)" >&2
  exit 1
fi
if [ ! -x "$REPO/bin/yugac" ]; then
  echo "run.sh: building yugac"
  make -C "$REPO" -j4
fi

if ! nc -z 127.0.0.1 8080 >/dev/null 2>&1; then
  echo "macos: starting backend on :8080"
  "$HERE/../backend/run.sh" &
  BACK_PID=$!
  STARTED_BACK=1
  n=0
  while [ "$n" -lt 80 ]; do
    if nc -z 127.0.0.1 8080 >/dev/null 2>&1; then
      break
    fi
    if ! kill -0 "$BACK_PID" 2>/dev/null; then
      echo "run.sh: backend exited before it listened on :8080" >&2
      exit 1
    fi
    n=$((n + 1))
    sleep 0.1
  done
  if [ "$n" -ge 80 ]; then
    echo "run.sh: timed out waiting for backend on :8080" >&2
    exit 1
  fi
fi

echo "macos: Cocoa desktop  (RPC http://127.0.0.1:8080)"
exec "$REPO/bin/yugac" --target=native --run "$HERE/app.yuga"
