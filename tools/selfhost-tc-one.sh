#!/bin/sh
# One entry file of nob's `selfhost_typecheck` gate: the Loam checker (tcdump)
# against `loamc --dump-typed`. Prints one verdict line, then the path:
#   match  the dumps are byte-identical
#   load   C cannot load the file (parse errors: selfhost_parse covers those)
#   hang   tcdump ran past 60 s (a watchdog subshell kills it; plain sh, so no
#          `timeout` or perl is needed)
#   diff   anything else
#
# Both dumps are deterministic: C's depends only on the loamc binary, the
# module list it resolved, those modules' sources and the runtime header, and
# the Loam one on the same plus the tcdump binary. So $REF caches, under a key
# over all of those, C's dump checksum (`<key>`) and a past match
# (`<key>.<tcdump sum>.ok`). A file whose inputs and both binaries are
# unchanged is not checked again; one with a cached C checksum skips only the
# C dump. A mismatch always redoes the C dump before it calls the file a diff,
# so a stale entry cannot fail a run, and only a real match is recorded.
#
# Run from the repo root. Env: LOAMC, EXE (tcdump), SCRATCH, REF, LOAMC_SUM,
# EXE_SUM (`cksum` of each binary).
entry_file=$1
work_dir=$(mktemp -d "$SCRATCH/w.XXXXXX") || exit 1
trap 'rm -rf "$work_dir"' EXIT
module_list="$work_dir/modules"
loam_dump="$work_dir/loam_dump"
c_dump="$work_dir/c_dump"
hung_marker="$work_dir/hung"

"$LOAMC" --dump-modules "$entry_file" > "$module_list" 2>/dev/null || {
    echo "load $entry_file"
    exit 0
}

input_key=$({
    echo "$LOAMC_SUM"
    cat "$module_list"
    sed -n -e 's/^rt "\(.*\)"$/\1/p' -e 's/^module .* path="\(.*\)"$/\1/p' "$module_list" |
        while IFS= read -r source_path; do cksum < "$source_path"; done
} | cksum | tr ' ' _)
c_sum_cache="$REF/$input_key"
match_cache="$c_sum_cache.$(echo "$EXE_SUM" | tr ' ' _).ok"
if [ -f "$match_cache" ]; then
    echo "match $entry_file"
    exit 0
fi

"$EXE" "$module_list" > "$loam_dump" 2>&1 &
checker_pid=$!
( sleep 60; kill $checker_pid 2>/dev/null && : > "$hung_marker" ) > /dev/null 2>&1 &
watchdog_pid=$!
wait $checker_pid
checker_status=$?
kill $watchdog_pid 2>/dev/null
wait $watchdog_pid 2>/dev/null
if [ -e "$hung_marker" ]; then
    echo "hang $entry_file"
    exit 0
fi
echo "exit $checker_status" >> "$loam_dump"
loam_dump_sum=$(cksum < "$loam_dump")

if [ -f "$c_sum_cache" ] && [ "$(cat "$c_sum_cache")" = "$loam_dump_sum" ]; then
    : > "$match_cache"
    echo "match $entry_file"
    exit 0
fi
"$LOAMC" --dump-typed "$entry_file" > "$c_dump" 2>&1
echo "exit $?" >> "$c_dump"
c_dump_sum=$(cksum < "$c_dump")
echo "$c_dump_sum" > "$work_dir/c_sum" && mv "$work_dir/c_sum" "$c_sum_cache"
if [ "$c_dump_sum" = "$loam_dump_sum" ] && cmp -s "$c_dump" "$loam_dump"; then
    : > "$match_cache"
    echo "match $entry_file"
else
    echo "diff $entry_file"
fi
