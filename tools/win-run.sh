#!/bin/bash
# Build and run, natively on Windows, the fixtures tools/win-bundle.sh emitted.
# Runs under MSYS2 bash with a MinGW-w64 gcc on PATH, from the repo root.
#
#   tools/win-run.sh BUNDLEDIR
#
# A fixture passes when it builds, exits 0, and — if it has a `.expected` —
# prints exactly that; one with a `.trap` must instead panic with that message (byte for byte: the runtime puts stdout in binary mode).
set -u
ROOT=$(cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
IN=${1:?usage: win-run.sh BUNDLEDIR}
CC=${CC:-gcc}
work=$(mktemp -d)
pass=0; fail=0; failed=""
for c in "$IN"/*.c; do
    b=$(basename "$c" .c)
    objs=""; links=""; i=0; ok=1; log="$work/$b.log"
    : > "$log"
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        dir=${dep%%|*}; rest=${dep#*|}; path=${rest%%|*}; flags=${rest#*|}
        if [ "$dir" = link ]; then links="$links $flags"; continue; fi
        i=$((i+1))
        # shellcheck disable=SC2086
        $CC -O1 -w -c "$dir/$path" $flags -o "$work/$b.dep$i.o" >>"$log" 2>&1 || ok=0
        objs="$objs $work/$b.dep$i.o"
    done < "$IN/$b.deps"
    # shellcheck disable=SC2086
    [ $ok = 1 ] && { $CC -std=gnu99 -O1 -w -I packages/loam/runtime "$c" $objs -o "$work/$b.exe" $links >>"$log" 2>&1 || ok=0; }
    if [ $ok = 1 ] && [ -f "$IN/$b.trap" ]; then
        # Must panic, with the `.trap` message on stderr.
        if "$work/$b.exe" < /dev/null > "$work/$b.out" 2>"$work/$b.err"; then
            echo "exited 0; should have panicked" >>"$log"; ok=0
        elif ! grep -qF -f "$IN/$b.trap" "$work/$b.err"; then
            { echo "--- panic message differs from $b.trap:"; cat "$work/$b.err"; } >>"$log"; ok=0
        fi
    elif [ $ok = 1 ]; then
        "$work/$b.exe" < /dev/null > "$work/$b.out" 2>>"$log" || { echo "exit status $?" >>"$log"; ok=0; }
    fi
    if [ $ok = 1 ] && [ -f "$IN/$b.expected" ] && ! cmp -s "$work/$b.out" "$IN/$b.expected"; then
        { echo "--- stdout differs from $b.expected:"; diff "$IN/$b.expected" "$work/$b.out" | head -20; } >>"$log"
        ok=0
    fi
    if [ $ok = 1 ]; then pass=$((pass+1)); else
        fail=$((fail+1)); failed="$failed $b"
        echo "FAIL $b"; sed 's/^/    /' "$log" | head -30
    fi
done
echo "win-run: $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "failed:$failed"; exit 1; }
