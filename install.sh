#!/bin/sh
# One-time macOS setup for building Loam and running its examples.
#
#   ./install.sh            core: Apple CLT, Homebrew, LLVM (wasm32 clang), Node
#   ./install.sh android    core + Android SDK/NDK/Gradle + emulator AVD (several GB)
#
# Idempotent — safe to re-run. Requires macOS and an internet connection.
# The android stack downloads several GB and asks you to accept Google's SDK
# licenses; the iOS Simulator target additionally needs full Xcode (App
# Store), not just the Command Line Tools.
set -e
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
MODE=${1:-core}

say() { printf 'install: %s\n' "$*"; }
die() { echo "install: $*" >&2; exit 1; }

case "$(uname -s)" in
  Darwin) ;;
  *) die "this script sets up macOS only. On other systems install a C11 cc + make, a wasm32 clang (LLVM), and Node yourself; 'make' then works the same." ;;
esac

if [ "$MODE" != core ] && [ "$MODE" != android ]; then
  die "unknown mode '$MODE' (core | android)"
fi

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
say "  ./run.sh gallery web         # wasm UI (Vite) at http://127.0.0.1:5174"
say "  ./run.sh gallery macos       # Cocoa window"
say "  ./run.sh gallery ios         # iOS Simulator (needs full Xcode)"
if [ "$MODE" = android ]; then
  say "  examples/zeus/gallery/android/emu.sh   # terminal 1"
  say "  ./run.sh gallery android               # terminal 2"
fi
say "Everything else: ./run.sh (no arguments) lists every example."
