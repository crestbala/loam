#!/bin/sh
# The `<output>.fresh` build cache, against one compiler:
#
#   sh check.sh <loamc> <work-dir> <tag>
#
# Builds a two-module app with a `#![c_source]` in <work-dir> and checks when a
# rebuild reuses the output (`loam: fresh` under LOAM_TIME) and when it must
# not. Prints one line per failed case and nothing when every case holds.
# <tag> names the C source, so two compilers checked at once keep apart the
# objects they leave in `<runtime>/.obj/`.
set -u
L=$1
W=$2
T=$3
mkdir -p "$W/runtime"
cd "$W" || exit 1
RT_OBJ="${LOAM_HOME:?}/packages/loam/runtime/.obj"

cat > util.loam <<'LOAM'
fn word() -> string {
    return "one"
}
LOAM
cat > "probe_$T.c" <<C
int probe_value(void) { return 1; }
C
cat > main.loam <<LOAM
#![c_source("probe_$T.c")]
import "std:fmt"
import "util.loam"

#[c("probe_value")]
fn probe_value() -> i32 {}

fn main() {
    fmt.println(util.word(), probe_value())
}
LOAM

# `case <name> <want: fresh|build> <want stdout> [env...] -- [extra args]`
case_n=0
check() {
    name=$1 want=$2 out=$3
    shift 3
    envs=""
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do envs="$envs $1"; shift; done
    [ "$#" -gt 0 ] && shift
    # shellcheck disable=SC2086
    log=$(env LOAM_TIME=1 $envs "$L" main.loam -o app --run "$@" 2>&1)
    rc=$?
    got=build
    case "$log" in *"loam: fresh"*) got=fresh ;; esac
    printed=$(printf '%s\n' "$log" | grep -v '^loam:' | tail -n 1)
    if [ "$rc" -ne 0 ]; then
        echo "fresh: $name: exit $rc"
        printf '%s\n' "$log" | sed 's/^/  /'
    elif [ "$got" != "$want" ] || [ "$printed" != "$out" ]; then
        echo "fresh: $name: want $want '$out', got $got '$printed'"
    fi
}

check first        build "one 1"
check same         fresh "one 1"
# Same size, same second: the stamp's nanoseconds and the byte compare catch it.
sed 's/one/two/' util.loam > util.tmp && mv util.tmp util.loam
check module-edit  build "two 1"
check module-same  fresh "two 1"
# A C source edited in the second its object was built still rebuilds it.
sed 's/return 1/return 2/' "probe_$T.c" > c.tmp && mv c.tmp "probe_$T.c"
check c-edit       build "two 2"
check c-same       fresh "two 2"
check env          build "two 2" LOAM_DEBUG=1
check env-same     fresh "two 2" LOAM_DEBUG=1
check env-back     build "two 2"
check no-fresh     build "two 2" LOAM_NO_FRESH=1
check after-off    build "two 2"
check flag         build "two 2" -- --int64-compat
check flag-same    fresh "two 2" -- --int64-compat
check flag-back    build "two 2"
echo "#" > runtime/side.h
check runtime-dir  build "two 2"
rm -f app app.exe
check no-output    build "two 2"
check output-back  fresh "two 2"

# `--emit-c` and `check` never read or write a stamp.
"$L" --emit-c main.loam -o out.c >/dev/null 2>&1
[ -e out.c.fresh ] && echo "fresh: emit-c wrote a stamp"
"$L" check main.loam >/dev/null 2>&1
[ -n "$(ls ./*.fresh 2>/dev/null | grep -v '^./app.fresh$')" ] && echo "fresh: stray stamp: $(ls ./*.fresh)"
rm -f "$RT_OBJ"/c_probe_"$T"_*.o
exit 0
