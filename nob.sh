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
set -e

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cd "$ROOT"

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
