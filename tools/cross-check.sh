#!/bin/sh
# Cross-compile the CLI surface for Linux and Windows with `zig cc`, so a
# runtime change that breaks another platform shows up on a macOS machine.
#
#   tools/cross-check.sh [target ...]
#
# Default targets: x86_64/aarch64 Linux (glibc) and x86_64/aarch64 Windows
# (MinGW-w64 headers). For each one this builds nob.loam's generated C, then
# compiles and links the generated C of every non-GUI compile_pass fixture,
# and for Linux also the seed compiler (packages/loam/src). It checks that the
# code builds and links; it does not run it (that needs the platform itself).
# Fixtures that link extra objects the driver adds (net.c, crypto, sqlite, a C sidecar)
# are skipped. Needs zig on PATH (`brew install zig`).
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
command -v zig >/dev/null || { echo "cross-check: zig not found (brew install zig)" >&2; exit 2; }
[ -x bin/loamc ] || ./nob.sh >/dev/null
OUT="$ROOT/out/cross"
mkdir -p "$OUT/cache"
export ZIG_GLOBAL_CACHE_DIR="$OUT/cache" ZIG_LOCAL_CACHE_DIR="$OUT/cache"
targets=${*:-"x86_64-linux-gnu aarch64-linux-gnu x86_64-windows-gnu aarch64-windows-gnu"}
skip='^(c_ffi|net_listen|sqlite|crypto|zeus_|maya_|raygui_|zui_|counter_panel)'
fail=0

cc_one() { # target c-file out
    zig cc -target "$1" -std=gnu99 -O1 -w -I packages/loam/runtime "$2" -o "$3" 2>&1 | grep -E "error" | head -3
}

for t in $targets; do
    n=0; bad=0
    bin/loamc nob.loam --emit-c -o "$OUT/nob.c" >/dev/null
    e=$(cc_one "$t" "$OUT/nob.c" "$OUT/nob-$t")
    [ -n "$e" ] && { echo "$t nob: $e"; bad=$((bad+1)); }
    for f in packages/loam/tests/compile_pass/*.loam; do
        b=$(basename "$f" .loam)
        echo "$b" | grep -qE "$skip" && continue
        grep -qE 'import "(zeus|maya|raygui|zui|http|crypto|sqlite)' "$f" && continue
        bin/loamc "$f" --emit-c -o "$OUT/$b.c" >/dev/null 2>&1 || continue
        n=$((n+1))
        e=$(cc_one "$t" "$OUT/$b.c" "$OUT/$b-$t")
        [ -n "$e" ] && { echo "$t $b: $e"; bad=$((bad+1)); }
    done
    case $t in *linux*)
        for s in $(ls packages/loam/src/*.c packages/loam/src/sema/*.c | grep -v -e '/lsp\.c$' -e '/fmt\.c$'); do
            e=$(zig cc -target "$t" -std=c11 -D_GNU_SOURCE -O1 -w -c "$s" -o "$OUT/seed.o" \
                -DLOAM_RT_PATH='"x"' -DLOAM_RUNTIME_DIR='"x"' -DLOAM_STD_DIR='"x"' -DLOAM_ZEUS_DIR='"x"' \
                -DLOAM_RAYGUI_DIR='"x"' -DLOAM_PATH='"x"' 2>&1 | grep -E "error" | head -2)
            [ -n "$e" ] && { echo "$t seed $s: $e"; bad=$((bad+1)); }
        done ;;
    esac
    echo "cross-check $t: nob + $n fixtures, $bad failed"
    [ "$bad" -eq 0 ] || fail=1
done
exit $fail
