#!/bin/bash
# Bootstrap the Loam compiler natively on Windows from the C the Linux job
# emitted for it (docs/self-hosting-loam.md §3), then check the fixed point
# there. Runs under MSYS2 bash with a MinGW-w64 gcc on PATH, from the repo root.
#
#   tools/win-selfhost.sh BUNDLEDIR     (made by tools/win-bundle.sh)
#
# 1. gcc builds stage 1 from `loamc.c.txt`.
# 2. Stage 1 emits its own C: the same bytes as Linux emitted.
# 3. Stage 1 builds itself through its driver (gcc via sys.spawn, no shell);
#    stage 2 emits the same C again.
# 4. For every bundled fixture, stage 1 emits the same C as Linux did, then
#    builds the fixture itself and runs it against its `.expected`.
set -u
ROOT=$(cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
IN=${1:?usage: win-selfhost.sh BUNDLEDIR}
CC=${CC:-gcc}
export CC
# A native program reads LOAM_HOME as a Windows path (C:/...), not MSYS's /c/...
export LOAM_HOME=$(cygpath -m "$ROOT")
export ZEUS_HEADLESS=1
work=$(mktemp -d)
src=packages/loam/compiler/loamc.loam
fail=0
die() { echo "FAIL $*"; exit 1; }

cp "$IN/loamc.c.txt" "$work/loamc.c"
$CC -std=gnu99 -O1 -w -DLOAM_STRING_OWNS -I packages/loam/runtime "$work/loamc.c" -o "$work/loamc1.exe" ||
    die "gcc could not build stage 1"
"$work/loamc1.exe" --emit-c "$src" -o "$work/s1.c" >/dev/null || die "stage 1 --emit-c of itself"
cmp -s "$work/loamc.c" "$work/s1.c" || { diff "$work/loamc.c" "$work/s1.c" | head -20; die "stage 1 C differs from Linux"; }
"$work/loamc1.exe" "$src" -o "$work/loamc2" || die "stage 1 could not build itself"
"$work/loamc2.exe" --emit-c "$src" -o "$work/s2.c" >/dev/null || die "stage 2 --emit-c of itself"
cmp -s "$work/s1.c" "$work/s2.c" || die "stage 2 C differs from stage 1"
echo "ok   stage 1 = stage 2 = Linux ($(wc -c < "$work/s1.c") bytes of C)"

pass=0; failed=""
for c in "$IN"/*.c; do
    b=$(basename "$c" .c)
    f=packages/loam/tests/compile_pass/$b.loam
    log="$work/$b.log"
    ok=1
    "$work/loamc1.exe" --emit-c "$f" -o "$work/$b.c" >"$log" 2>&1 || ok=0
    if [ $ok = 1 ] && ! cmp -s "$c" "$work/$b.c"; then echo "C differs from Linux" >>"$log"; ok=0; fi
    [ $ok = 1 ] && { "$work/loamc1.exe" "$f" -o "$work/$b" >>"$log" 2>&1 || ok=0; }
    [ $ok = 1 ] && { "$work/$b.exe" </dev/null >"$work/$b.out" 2>>"$log" || { echo "exit status $?" >>"$log"; ok=0; }; }
    if [ $ok = 1 ] && [ -f "$IN/$b.expected" ] && ! cmp -s "$work/$b.out" "$IN/$b.expected"; then
        echo "stdout differs from $b.expected" >>"$log"; ok=0
    fi
    if [ $ok = 1 ]; then pass=$((pass+1)); else
        fail=$((fail+1)); failed="$failed $b"; echo "FAIL $b"; sed 's/^/    /' "$log" | head -20
    fi
done
echo "win-selfhost: $pass fixtures emitted, built and run by the Loam compiler, $fail failed"
[ $fail -eq 0 ] || { echo "failed:$failed"; exit 1; }
