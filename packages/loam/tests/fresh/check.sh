#!/bin/sh
# The `<output>.fresh` build cache, against one compiler:
#
#   sh check.sh <loamc> <work-dir> <tag>
#
# Builds a two-module app with a `#![c_source]` and checks when a rebuild
# reuses the output (`loam: fresh` under LOAM_TIME) and when it must not.
# Prints one line per failed case and nothing when every case holds.
# <tag> names the C source, so two compilers checked at once keep apart the
# objects they leave in `<runtime>/.obj/`.
#
# The cases come in independent groups, each from its own first build in its
# own directory (and its own C source name, so no two share an object), and
# the groups run at once: a dozen builds in a row became the longest job of a
# test run.
set -u
L=$1
W=$2
T=$3
RT_OBJ="${LOAM_HOME:?}/packages/loam/runtime/.obj"

# `app <dir> <tag>`: the two-module app with a C source named after <tag>.
app() {
    mkdir -p "$1/runtime"
    cat > "$1/util.loam" <<'LOAM'
fn word() -> string {
    return "one"
}
LOAM
    cat > "$1/probe_$2.c" <<C
int probe_value(void) { return 1; }
C
    cat > "$1/main.loam" <<LOAM
#![c_source("probe_$2.c")]
import "std:fmt"
import "util.loam"

#[c("probe_value")]
fn probe_value() -> i32 {}

fn main() {
    fmt.println(util.word(), probe_value())
}
LOAM
}

# `check <name> <want: fresh|build> <want stdout> [env...] -- [extra args]`,
# in the current directory.
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

# A module edit: same size, same second, so the stamp's nanoseconds and the
# byte compare catch it.
group_module() {
    check first        build "one 1"
    check same         fresh "one 1"
    sed 's/one/two/' util.loam > util.tmp && mv util.tmp util.loam
    check module-edit  build "two 1"
    check module-same  fresh "two 1"
}

# A C source edited in the second its object was built still rebuilds it.
group_c() {
    check c-first      build "one 1"
    sed 's/return 1/return 2/' "probe_$1.c" > c.tmp && mv c.tmp "probe_$1.c"
    check c-edit       build "one 2"
    check c-same       fresh "one 2"
}

# The environment and `LOAM_NO_FRESH`.
group_env() {
    check env-first    build "one 1"
    check env          build "one 1" LOAM_DEBUG=1
    check env-same     fresh "one 1" LOAM_DEBUG=1
    check env-back     build "one 1"
    check no-fresh     build "one 1" LOAM_NO_FRESH=1
    check after-off    build "one 1"
}

# Compiler flags.
group_flag() {
    check flag-first   build "one 1"
    check flag         build "one 1" -- --int64-compat
    check flag-same    fresh "one 1" -- --int64-compat
    check flag-back    build "one 1"
}

# The runtime directory, the output, and the commands that never stamp.
group_output() {
    check out-first    build "one 1"
    echo "#" > runtime/side.h
    check runtime-dir  build "one 1"
    rm -f app app.exe
    check no-output    build "one 1"
    check output-back  fresh "one 1"
    # `--emit-c` and `check` never read or write a stamp.
    "$L" --emit-c main.loam -o out.c >/dev/null 2>&1
    [ -e out.c.fresh ] && echo "fresh: emit-c wrote a stamp"
    "$L" check main.loam >/dev/null 2>&1
    [ -n "$(ls ./*.fresh 2>/dev/null | grep -v '^./app.fresh$')" ] && echo "fresh: stray stamp: $(ls ./*.fresh)"
}

groups="module c env flag output"
for g in $groups; do
    (
        tag="${T}$g"
        app "$W/$g" "$tag"
        cd "$W/$g" || exit 1
        "group_$g" "$tag" > "$W/$g.failures" 2>&1
    ) &
done
wait
for g in $groups; do
    cat "$W/$g.failures"
done
rm -f "$RT_OBJ"/c_probe_"$T"*_*.o
exit 0
