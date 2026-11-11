#!/bin/sh
# Bootstrap for `nob.loam`, which builds and tests Loam. `nob` is a Loam program,
# so something that is not Loam has to build the first compiler; this does that
# once, then forwards every argument.
#
#   ./nob.sh                  build bin/loamc, bin/loam-lsp, bin/loam-fmt, bin/zeli
#   ./nob.sh test             the suite, batched
#   ./nob.sh test --isolated  the suite, every fixture as its own binary
#   ./nob.sh test <filter>    only fixtures whose path contains <filter>
#   ./nob.sh help             everything else
#
# The flags build the *seed* compiler in one shot. Keep them in sync with
# `cflags()` in nob.loam, which owns the incremental object build from then on.
#
# On Windows (MSYS2 / MinGW) the C compiler's driver.c is POSIX and does not
# build, so bin/loamc is built from bootstrap/loamc.c instead: the Loam
# compiler's own generated C, committed (`nob seed` regenerates it). It finds
# std and the runtime from its own path (<root>/bin/loamc), so the C holds no
# absolute paths. `nob` itself does not run there (`sys.exec` goes through
# cmd.exe, and nob's commands are POSIX sh), so this stops after bin/loamc.
set -e

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cd "$ROOT"

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
  if [ ! -x bin/loamc.exe ] || [ bootstrap/loamc.c -nt bin/loamc.exe ]; then
    mkdir -p bin
    cc=${CC:-cc}
    command -v "${cc%% *}" >/dev/null 2>&1 || cc=gcc
    echo "  cc  bin/loamc (from bootstrap/loamc.c)"
    # The flags the Loam driver builds a program with (copt_flags in
    # packages/loam/compiler/loamc.loam); -w because the C is generated.
    # shellcheck disable=SC2086
    $cc -std=gnu99 -O1 -fno-asynchronous-unwind-tables -fomit-frame-pointer -ffp-contract=off \
      -w -DLOAM_STRING_OWNS bootstrap/loamc.c -o bin/loamc.exe -lm
  fi
  if [ $# -gt 0 ]; then
    echo "nob.sh: only the build runs on Windows (nob needs a POSIX shell for sys.exec)" >&2
    exit 2
  fi
  exit 0
  ;;
esac

# Rebuild when missing *or* stale: `nob.loam` imports std, which may use syntax
# only the current sources understand, so an old binary cannot build `nob`.
if [ ! -x bin/loamc ] || [ -n "$(find packages/loam/src -name '*.[ch]' -newer bin/loamc -print | head -n 1)" ]; then
  mkdir -p bin
  # driver.c is loamc's main; lsp.c and fmt.c carry their own and are built apart.
  sources=$(ls packages/loam/src/*.c packages/loam/src/sema/*.c | grep -v -e '/lsp\.c$' -e '/fmt\.c$')
  echo "  cc  bin/loamc (seed)"
  # Strict -std=c11 hides strdup and friends from glibc's headers.
  # shellcheck disable=SC2086
  cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -O2 -g \
    -DLOAM_RT_PATH="\"$ROOT/packages/loam/runtime/loam_rt.h\"" \
    -DLOAM_RUNTIME_DIR="\"$ROOT/packages/loam/runtime\"" \
    -DLOAM_STD_DIR="\"$ROOT/packages/loam/std\"" \
    -DLOAM_ZEUS_DIR="\"$ROOT/packages/zeus\"" \
    -DLOAM_RAYGUI_DIR="\"$ROOT/packages/raygui\"" \
    -DLOAM_PATH="\"$ROOT/packages/zeus:$ROOT/packages/zeus-components:$ROOT/packages/http:$ROOT/packages/maya:$ROOT/packages/raygui:$ROOT/packages/sqlite:$ROOT/packages/crypto\"" \
    $sources -o bin/loamc
fi

if [ ! -x bin/nob ] || [ nob.loam -nt bin/nob ] || [ bin/loamc -nt bin/nob ]; then
  ./bin/loamc nob.loam -o bin/nob
fi

exec ./bin/nob "$@"
