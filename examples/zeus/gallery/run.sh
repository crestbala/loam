#!/bin/sh
# Start the wasm UI (Vite :5174). Ctrl-C stops it.
#   ./run.sh            frontend
#   ./run.sh frontend
#   ./run.sh ios
#   ./run.sh android
#   ./run.sh macos
#   ./run.sh linux      (on Linux; elsewhere: linux/kit.sh)
#   ./run.sh backend
set -e
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
variant=${1:-frontend}
case $variant in
  web|frontend|"") exec "$HERE/frontend/run.sh" ;;
  ios)              exec "$HERE/ios/run.sh" ;;
  android)          exec "$HERE/android/run.sh" ;;
  macos|native)     exec "$HERE/macos/run.sh" ;;
  linux)            exec "$HERE/linux/run.sh" ;;
  backend)          exec "$HERE/backend/run.sh" ;;
  *)
    echo "run.sh: unknown variant '$variant' (frontend ios android macos linux backend)" >&2
    exit 1
    ;;
esac
