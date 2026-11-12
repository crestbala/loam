#!/bin/sh
# Cross-compile the CLI surface for Linux and Windows with `zig cc`, so a
# runtime change that breaks another platform shows up on a macOS machine.
#
#   tools/cross-check.sh [target ...]
#
# Default targets: x86_64/aarch64 Linux (glibc) and x86_64/aarch64 Windows
# (MinGW-w64 headers). For each one this builds nob.loam's generated C, then
# compiles and links the generated C of every non-GUI compile_pass fixture.
# (The seed, bootstrap/loamc.c, is built natively by CI on Linux and Windows.)
# It checks that the code builds and links; it does not run it (that needs the platform itself).
# A fixture links what the driver would add: `runtime/net.c` when its C calls
# into std:net, and every `#![c_source(path, flags...)]` / `#![c_link(...)]`
# of the fixture and of the packages it imports. Fixtures that import a GUI
# package (zeus, zui, maya, raygui) are skipped. Needs zig on PATH
# (`brew install zig`).
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
command -v zig >/dev/null || { echo "cross-check: zig not found (brew install zig)" >&2; exit 2; }
[ -x bin/loamc ] || ./nob.sh >/dev/null
OUT="$ROOT/out/cross"
mkdir -p "$OUT/cache"
export ZIG_GLOBAL_CACHE_DIR="$OUT/cache" ZIG_LOCAL_CACHE_DIR="$OUT/cache"
targets=${*:-"x86_64-linux-gnu aarch64-linux-gnu x86_64-windows-gnu aarch64-windows-gnu"}
. "$ROOT/tools/fixture-deps.sh"

cc_one() { # target c-file out fixture
    # net.c is linked into every fixture: the drivers add it for any program
    # that loads std:net or the http package, whether or not it calls it.
    objs="$OUT/net-$1.o"; links=""; i=0
    deps=$(deps_of "$4" | sort -u)
    IFS='
'
    for dep in $deps; do
        IFS=' '
        dir=${dep%%|*}; rest=${dep#*|}; path=${rest%%|*}; flags=${rest#*|}
        if [ "$dir" = link ]; then links="$links $flags"; continue; fi
        i=$((i+1)); o="$OUT/dep$i-$1.o"
        e=$(zig cc -target "$1" -O1 -fno-sanitize=undefined -w -c "$dir/$path" $flags -o "$o" 2>&1 | grep -E "error" | head -3)
        [ -n "$e" ] && { echo "$path: $e"; return; }
        objs="$objs $o"
    done
    IFS=' '
    zig cc -target "$1" -std=gnu99 -O1 -w -I packages/loam/runtime "$2" $objs -o "$3" $links 2>&1 | grep -E "error" | head -3
}

fail=0
for t in $targets; do
    n=0; bad=0; nogap=""
    e=$(zig cc -target "$t" -O1 -fno-sanitize=undefined -w -I packages/loam/runtime -c packages/loam/runtime/net.c -o "$OUT/net-$t.o" 2>&1 | grep -E "error" | head -3)
    [ -n "$e" ] && { echo "cross-check $t: runtime/net.c: $e"; fail=1; continue; }
    bin/loamc nob.loam --emit-c -o "$OUT/nob.c" >/dev/null
    e=$(cc_one "$t" "$OUT/nob.c" "$OUT/nob-$t" nob.loam)
    [ -n "$e" ] && { echo "$t nob: $e"; bad=$((bad+1)); }
    for f in packages/loam/tests/compile_pass/*.loam; do
        b=$(basename "$f" .loam)
        echo "$b" | grep -qE "$skip" && continue
        grep -qE "$gui" "$f" && continue
        bin/loamc "$f" --emit-c -o "$OUT/$b.c" >/dev/null 2>&1 || continue
        # Known gap: runtime/net.c is BSD sockets + platform TLS with no Windows
        # port yet. Not needed to self-host; listed so it stays visible.
        case $t in *windows*)
            if grep -q 'loam_net_' "$OUT/$b.c"; then nogap="$nogap $b"; continue; fi ;;
        esac
        n=$((n+1))
        e=$(cc_one "$t" "$OUT/$b.c" "$OUT/$b-$t" "$f")
        [ -n "$e" ] && { echo "$t $b: $e"; bad=$((bad+1)); }
    done
    echo "cross-check $t: nob + $n fixtures, $bad failed"
    [ -n "$nogap" ] && echo "  known gap, skipped (std:net has no Windows port):$nogap"
    [ "$bad" -eq 0 ] || fail=1
done
exit $fail
