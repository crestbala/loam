#!/bin/sh
# One-time setup for building Loam and running its examples.
#
#   ./install.sh            core: cc, make, LLVM (wasm32 clang), Node
#   ./install.sh android    core + Android SDK/NDK/Gradle + emulator AVD (several GB)
#
# Idempotent — safe to re-run. Works on macOS (Homebrew) and Linux
# (apt / dnf / pacman). The android stack downloads several GB and asks you to
# accept Google's SDK licenses; on macOS the iOS Simulator target additionally
# needs full Xcode (App Store), not just the Command Line Tools.
set -e
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
MODE=${1:-core}
OS=$(uname -s)

say() { printf 'install: %s\n' "$*"; }
die() { echo "install: $*" >&2; exit 1; }

case "$OS" in
  Darwin|Linux) ;;
  *) die "$OS is not a supported host. Install a C11 cc + make, a wasm32 clang (LLVM), Node, and (for the GUI) libX11 yourself; 'make' then works the same." ;;
esac

if [ "$MODE" != core ] && [ "$MODE" != android ]; then
  die "unknown mode '$MODE' (core | android)"
fi

if [ "$MODE" = android ] && [ "$OS" = Linux ]; then
  die "the android installer targets macOS. On Linux install the Android SDK/NDK, Gradle and a JDK with your package manager, then set ANDROID_HOME/ANDROID_NDK_HOME and run 'make' normally."
fi

# --- Linux: core toolchain ----------------------------------------------------
# The GUI host is X11 (packages/zeus/hosts/desktop/linux.c): no GTK, just
# libX11 plus a C11 cc. Desktop and CLI programs need nothing else. iOS/Android
# hosts still come from their own SDKs; a Linux box builds the native + wasm
# + CLI targets.
if [ "$OS" = Linux ]; then
  command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 \
    || die "no C compiler (cc / gcc / clang). Install build-essential (Debian/Ubuntu), 'Development Tools' (Fedora), or 'base-devel' (Arch), then re-run."
  command -v make >/dev/null 2>&1 || die "make is missing. Install your distro's build-essential / base-devel group, then re-run."
  say "C11 toolchain ok ($(command -v cc >/dev/null 2>&1 && echo cc || command -v gcc 2>/dev/null || command -v clang))"

  # LLVM ships a clang with the wasm32 target that a distro gcc lacks.
  if ! command -v clang >/dev/null 2>&1; then
    say "clang/LLVM is missing (needed for --target=wasm32)."
    say "  Debian/Ubuntu:  sudo apt-get install -y clang lld"
    say "  Fedora:         sudo dnf install -y clang lld"
    say "  Arch:           sudo pacman -S --needed clang lld"
  else
    WASMCC=$(command -v clang)
    if "$WASMCC" --target=wasm32 -fsyntax-only -x c /dev/null >/dev/null 2>&1; then
      say "wasm32 clang ok ($WASMCC)"
    else
      say "clang found but has no wasm32 target; a full LLVM clang is required for --target=wasm32"
    fi
    say "different LLVM? export LOAM_WASM_CC=/path/to/clang"
  fi

  # libX11 + headers for the desktop GUI host.
  if ! { command -v pkg-config >/dev/null 2>&1 && pkg-config --exists x11; }; then
    say "libX11 headers not found (needed for the native GUI window)."
    say "  Debian/Ubuntu:  sudo apt-get install -y libx11-dev"
    say "  Fedora:         sudo dnf install -y libX11-devel"
    say "  Arch:           sudo pacman -S --needed libx11"
  else
    say "libX11 ok (native GUI host)"
  fi

  if ! command -v node >/dev/null 2>&1; then
    say "Node.js not found (needed only for the Vite wasm dev servers)."
    say "  Debian/Ubuntu:  sudo apt-get install -y nodejs npm"
    say "  Fedora:         sudo dnf install -y nodejs npm"
    say "  Arch:           sudo pacman -S --needed nodejs npm"
  else
    say "node ok ($(node --version 2>/dev/null || echo present))"
  fi

  say "done. Next steps:"
  say "  make && make test            # build loam, run the full gate"
  say "  ./bin/loamc app.loam --run   # a CLI program"
  say "  ./bin/loamc --cost app.loam  # where the compile time goes"
  say "  ./run.sh gallery web         # wasm UI (Vite) at http://127.0.0.1:5174"
  say "  ./run.sh gallery             # native X11 window (needs a DISPLAY)"
  say "Everything else: ./run.sh (no arguments) lists every example."
  exit 0
fi

# --- macOS --------------------------------------------------------------------

# 1. Apple Command Line Tools — cc, make, and the native-target clang.
if ! xcode-select -p >/dev/null 2>&1; then
  die "Xcode Command Line Tools are missing. Run 'xcode-select --install' (GUI), wait for it to finish, then re-run this script."
fi
command -v cc >/dev/null 2>&1 || die "no cc after Command Line Tools — re-run 'xcode-select --install'"
command -v make >/dev/null 2>&1 || die "no make after Command Line Tools — re-run 'xcode-select --install'"
say "Apple Command Line Tools ok"

# 2. Homebrew.
if ! command -v brew >/dev/null 2>&1; then
  echo "install: Homebrew is missing (https://brew.sh)."
  printf "install: run the Homebrew installer now? [y/N] "
  read -r ans
  case "$ans" in
    y|Y|yes|YES)
      /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
      ;;
    *) die "Homebrew is required. Re-run after installing it." ;;
  esac
fi
command -v brew >/dev/null 2>&1 || die "Homebrew did not finish installing — re-run this script"

# 3. Core brew packages: LLVM (its clang has the wasm32 target that Apple's
#    /usr/bin/clang lacks), Node (Vite dev servers for the wasm examples), and
#    raylib (the raygui example).
for p in llvm node raylib; do
  if brew list --formula "$p" >/dev/null 2>&1; then
    say "$p already installed"
  else
    say "brew install $p (this can take a while)"
    brew install "$p"
  fi
done

if [ "$(uname -m)" = arm64 ]; then
  WASMCC=/opt/homebrew/opt/llvm/bin/clang
else
  WASMCC=/usr/local/opt/llvm/bin/clang
fi
[ -x "$WASMCC" ] || die "llvm installed but $WASMCC is missing"
if ! "$WASMCC" --target=wasm32 -fsyntax-only -x c /dev/null >/dev/null 2>&1; then
  die "wasm32 probe failed on $WASMCC — is this LLVM built with the wasm target?"
fi
say "wasm32 clang ok ($WASMCC) — loam finds this path automatically"
say "different LLVM? export LOAM_WASM_CC=/path/to/clang instead"

# 4. Android stack (optional): JDK, SDK, NDK, Gradle, emulator image + AVD.
#    The counter example's installer is the canonical one; every zeus
#    android/run.sh sources the .sdk-env next to it, so link it into the
#    gallery example too.
if [ "$MODE" = android ]; then
  say "android stack: JDK / SDK / NDK / Gradle / emulator AVD (several GB)"
  sh "$HERE/examples/zeus/counter/android/install.sh"
  ln -sfn ../../counter/android/.sdk-env "$HERE/examples/zeus/gallery/android/.sdk-env"
  say "android ok — boot the shared 'loam' AVD with examples/zeus/<app>/android/emu.sh"
fi

say "done. Next steps:"
say "  make && make test            # build loam, run the full gate"
say "  ./bin/loamc --cost app.loam  # where the compile time goes"
say "  ./run.sh gallery web         # wasm UI (Vite) at http://127.0.0.1:5174"
say "  ./run.sh gallery macos       # Cocoa window"
say "  ./run.sh gallery ios         # iOS Simulator (needs full Xcode)"
if [ "$MODE" = android ]; then
  say "  examples/zeus/gallery/android/emu.sh   # terminal 1"
  say "  ./run.sh gallery android               # terminal 2"
fi
say "Everything else: ./run.sh (no arguments) lists every example."
