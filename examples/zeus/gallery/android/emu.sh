#!/bin/sh
# Boot the `loam` AVD. SwiftShader avoids host-GPU bind failures on macOS
# (ERROR 12299 / 12301, blank emulator window).
set -e
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
AVD_NAME=loam

if [ -f "$HERE/.sdk-env" ]; then
  # shellcheck disable=SC1091
  . "$HERE/.sdk-env"
fi
if [ -z "$ANDROID_HOME" ] || [ ! -d "$ANDROID_HOME" ]; then
  if [ -d "$HOME/Library/Android/sdk" ]; then
    ANDROID_HOME=$HOME/Library/Android/sdk
  elif command -v brew >/dev/null 2>&1 && [ -d "$(brew --prefix)/share/android-commandlinetools" ]; then
    ANDROID_HOME=$(brew --prefix)/share/android-commandlinetools
  fi
  export ANDROID_HOME
fi
if [ -z "$ANDROID_HOME" ] || [ ! -d "$ANDROID_HOME" ]; then
  echo "emu.sh: no Android SDK. Run $HERE/install.sh first." >&2
  exit 1
fi
export ANDROID_SDK_ROOT=$ANDROID_HOME
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"

EMU=$ANDROID_HOME/emulator/emulator
if [ ! -x "$EMU" ]; then
  echo "emu.sh: emulator binary missing. Re-run $HERE/install.sh." >&2
  exit 1
fi

echo "android: emulator -avd $AVD_NAME (SwiftShader GPU)"
exec "$EMU" -avd "$AVD_NAME" \
  -gpu swiftshader_indirect \
  -feature -Vulkan \
  -no-snapshot-load \
  "$@"
