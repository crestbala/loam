#!/bin/sh
# One entry file of nob's `selfhost_passes` stanza: every checker dump of the
# Loam compiler (`tcdump --all`, fed the module list C's loader resolved)
# against the seed's (`loamc --dump-all`), stage by stage:
#   typecheck  --dump-typed     check  --dump-checked   ir  --dump-ir
#   dce        --dump-dce       codegen  --dump-c
# Each side writes, per stage, what that single dump mode prints (stdout and
# stderr together) and its exit status ("exit N", appended here), so every
# stage is compared byte for byte as when each mode ran on its own. Prints one
# verdict line per stage, `<verdict> <stage> <path>`:
#   match  the stage's dumps are byte-identical
#   load   C cannot load the file (parse errors: selfhost_parse covers those)
#   hang   tcdump ran past 60 s (a watchdog subshell kills it; plain sh, so no
#          `timeout` or perl is needed)
#   diff   anything else, a stage file missing on either side included
#
# Both sides are deterministic: C's dumps depend only on the loamc binary, the
# module list it resolved, those modules' sources and the runtime header, and
# the Loam ones on the same plus the tcdump binary. So $REF caches, under a key
# over all of those, C's five stage checksums (`<key>`) and a past match of all
# five (`<key>.<tcdump sum>.ok`). A file whose inputs and both binaries are
# unchanged is not checked again; one with cached C checksums skips the C run
# when every Loam stage has its checksum. Anything else reruns C before it
# calls a stage a diff, so a stale entry cannot fail a run, and only a real
# match of all five is recorded.
#
# Run from the repo root. Env: LOAMC, EXE (tcdump), SCRATCH (where the dumps go:
# /dev/shm on Linux), REF, LOAMC_SUM,
# EXE_SUM (`cksum` of each binary).
entry_file=$1
stages="typecheck check ir dce codegen"
work_dir=$(mktemp -d "$SCRATCH/w.XXXXXX") || exit 1
trap 'rm -rf "$work_dir"' EXIT
module_list="$work_dir/modules"
loam_dir="$work_dir/loam"
c_dir="$work_dir/c"
hung_marker="$work_dir/hung"

every_stage() {
    for stage in $stages; do
        echo "$1 $stage $entry_file"
    done
}

# One `<stage> <cksum>` line per stage of `$1`, with each stage's exit line
# appended to its dump first. A stage with no exit file did not finish.
stage_sums() {
    for stage in $stages; do
        if [ -f "$1/$stage.exit" ] && [ -f "$1/$stage" ]; then
            cat "$1/$stage.exit" >> "$1/$stage"
            echo "$stage $(cksum < "$1/$stage")"
        else
            echo "$stage unfinished-$2"
        fi
    done
}

"$LOAMC" --dump-modules "$entry_file" > "$module_list" 2>/dev/null || {
    every_stage load
    exit 0
}

input_key=$({
    echo "$LOAMC_SUM --dump-all"
    cat "$module_list"
    sed -n -e 's/^rt "\(.*\)"$/\1/p' -e 's/^module .* path="\(.*\)"$/\1/p' "$module_list" |
        while IFS= read -r source_path; do printf '%s\0' "$source_path"; done |
        xargs -0 cksum
} | cksum | tr ' ' _)
c_sums_cache="$REF/$input_key"
match_cache="$c_sums_cache.$(echo "$EXE_SUM" | tr ' ' _).ok"
if [ -f "$match_cache" ]; then
    every_stage match
    exit 0
fi

mkdir "$loam_dir" "$c_dir" || exit 1
"$EXE" --all "$loam_dir" "$module_list" > /dev/null 2>&1 &
checker_pid=$!
# The watchdog kills its own sleep when it is stopped, so none outlive the entry.
(
    trap 'kill $sleep_pid 2>/dev/null; exit 0' TERM
    sleep 60 &
    sleep_pid=$!
    wait $sleep_pid
    kill $checker_pid 2>/dev/null && : > "$hung_marker"
) > /dev/null 2>&1 &
watchdog_pid=$!
wait $checker_pid
kill $watchdog_pid 2>/dev/null
wait $watchdog_pid 2>/dev/null
if [ -e "$hung_marker" ]; then
    every_stage hang
    exit 0
fi
loam_sums=$(stage_sums "$loam_dir" loam)

if [ -f "$c_sums_cache" ] && [ "$(cat "$c_sums_cache")" = "$loam_sums" ]; then
    : > "$match_cache"
    every_stage match
    exit 0
fi
"$LOAMC" --dump-all "$c_dir" "$entry_file" > /dev/null 2>&1
c_sums=$(stage_sums "$c_dir" c)
case $c_sums in
*unfinished-c*) ;;
*) echo "$c_sums" > "$work_dir/c_sums" && mv "$work_dir/c_sums" "$c_sums_cache" ;;
esac
all_match=1
for stage in $stages; do
    if [ -f "$c_dir/$stage.exit" ] && [ -f "$loam_dir/$stage.exit" ] && cmp -s "$c_dir/$stage" "$loam_dir/$stage"; then
        echo "match $stage $entry_file"
    else
        echo "diff $stage $entry_file"
        all_match=0
    fi
done
[ $all_match = 1 ] && : > "$match_cache"
exit 0
