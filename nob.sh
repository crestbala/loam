#!/bin/sh
# Bootstrap for `nob.loam`, which builds and tests Loam. The compiler and `nob`
# are Loam programs, so something that is not Loam has to build the first
# compiler; this does that, then forwards every argument.
#
#   ./nob.sh                  build bin/loamc, bin/loam-lsp, bin/loam-fmt, bin/zeli
#   ./nob.sh test             the suite, batched
#   ./nob.sh test --isolated  the suite, every fixture as its own binary
#   ./nob.sh test <filter>    only fixtures whose path contains <filter>
#   ./nob.sh help             everything else
#
# The same three steps on every OS, needing only a C compiler:
#
#   1. cc builds bootstrap/loamc.c into bin/loamc0. That file is the stage-0
#      seed: the Loam compiler's own generated C, committed (`nob seed`
#      regenerates it when it can no longer build the current sources).
#   2. bin/loamc0 builds bin/loamc from packages/loam/compiler/loamc.loam, the
#      current compiler. Its `.fresh` cache makes this a no-op until a source,
#      std, the runtime or the seed changes.
#   3. bin/loamc builds bin/nob from nob.loam.
#
# The compiler finds std and the runtime from its own path (<root>/bin/loamc),
# so both binaries must sit in bin/ and the C holds no absolute paths.
#
# On Windows (MSYS2 / MinGW) `nob` itself does not run (`sys.exec` goes through
# cmd.exe, and nob's commands are POSIX sh), so this stops after bin/loamc.
set -e

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cd "$ROOT"

exe_suffix=""
on_windows=0
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
  exe_suffix=".exe"
  on_windows=1
  ;;
esac

c_compiler=${CC:-cc}
command -v "${c_compiler%% *}" >/dev/null 2>&1 || c_compiler=gcc

seed_binary="bin/loamc0$exe_suffix"
if [ ! -x "$seed_binary" ] || [ bootstrap/loamc.c -nt "$seed_binary" ]; then
  mkdir -p bin
  echo "  cc  bin/loamc0 (from bootstrap/loamc.c)"
  # The flags the Loam driver builds a program with (copt_flags in
  # packages/loam/compiler/loamc.loam); -w because the C is generated.
  # shellcheck disable=SC2086
  $c_compiler -std=gnu99 -O1 -fno-asynchronous-unwind-tables -fomit-frame-pointer -ffp-contract=off \
    -w -DLOAM_STRING_OWNS bootstrap/loamc.c -o "$seed_binary" -lm
fi

# The build modes of the environment (a headless or debug test run, a batch
# or shard build) are for the programs under test, not for the compiler: with
# them it would be rebuilt, unoptimized, whenever one is set or cleared.
(
  unset ZEUS_HEADLESS LOAM_HEADLESS MAYA_HEADLESS LOAM_DEV LOAM_DEBUG ZEUS_MEM_STATS \
    LOAM_LINK_EXTRA LOAM_BATCH LOAM_SHARD LOAM_HOST LOAM_SERVER_SPLIT LOAM_NO_DCE LOAM_HOME
  [ -n "${CC:-}" ] || [ "$c_compiler" = cc ] || export CC="$c_compiler"
  ./bin/loamc0 packages/loam/compiler/loamc.loam -o bin/loamc >/dev/null
)

if [ "$on_windows" = 1 ]; then
  if [ $# -gt 0 ]; then
    echo "nob.sh: only the build runs on Windows (nob needs a POSIX shell for sys.exec)" >&2
    exit 2
  fi
  exit 0
fi

if [ ! -x bin/nob ] || [ nob.loam -nt bin/nob ] || [ bin/loamc -nt bin/nob ]; then
  ./bin/loamc nob.loam -o bin/nob
fi

exec ./bin/nob "$@"
