#!/bin/sh
# own_buffer.sh — the macOS desktop display paths, measured side by side.
#
# The macOS desktop display paths, measured side by side. The DEFAULT is the
# owned bitmap (see mac.m: AppKit's store is three full-window buffers the
# compositor owns, this is one buffer we own, for identical pixels), so the
# first run sets no variable and the AppKit store is the one that has to ask:
#
#   default (owned bitmap) | `ZEUS_OWN_SURFACE=1` (owned IOSurface, the lean one)
#   | `APP_KIT=1` (AppKit's store) | `ZEUS_WIDE_GAMUT=1` (that store, display
#   profile, which doubles its depth — see the colour-space note in
#   hosts/desktop/mac.m)
#
# For each run this reports the physical footprint (now and peak) and the
# IOSurface / IOAccelerator regions, so the numbers in
# examples/zeus/myapp/readme.md can be reproduced instead of hand-copied. It also
# byte-compares the window crops, which is how "the paths draw the same pixels"
# is checked rather than asserted.
#
#   sh packages/loam/tests/bench/own_buffer.sh [app-dir] [seconds]
#
# Defaults: examples/zeus/myapp, 8s per path.
#
# `vmmap` is the source of truth: it gives the physical footprint Activity
# Monitor shows plus the per-region-type breakdown, and it needs no cooperation
# from the app. Read **peak**, not `now` — the instantaneous number moves by tens
# of MB between runs as stores are created and torn down (see the readme).
#
# Frame rate is measured with `ZEUS_FRAME_BENCH=1`, which keeps the frame clock
# running. An idle Zeus window has nothing pending and stops its own clock, so
# without it both paths would be sampled at different workloads — or not at all.
# Both runs get the same hook, so the comparison is like-for-like.
#
# The modes run *sequentially*, each taking the front, because an occluded window
# stops drawing and its store can be torn down — which would make the paths
# incomparable.
cd "$(dirname "$0")/../../../.." || exit 1
set -u

APP_DIR="${1:-examples/zeus/myapp}"
SECS="${2:-8}"
OUT=packages/loam/tests/tmp/own_buffer
LOAM=./bin/loamc
BENCH=packages/loam/tests/bench
mkdir -p "$OUT"

# Window-id lookup, so each crop is of the app's own window. See bench/winid.m.
WINID="$OUT/winid"
if [ ! -x "$WINID" ] || [ "$BENCH/winid.m" -nt "$WINID" ]; then
    cc -framework Cocoa -o "$WINID" "$BENCH/winid.m" || WINID=""
fi

# Entry resolution, same order as run.sh's zeus_entry().
entry=""
for f in app.loam "$(basename "$APP_DIR").loam" main.loam; do
    [ -f "$APP_DIR/$f" ] && { entry="$APP_DIR/$f"; break; }
done
if [ -z "$entry" ]; then
    echo "own_buffer: no entry .loam in $APP_DIR" >&2
    exit 1
fi

bin="$APP_DIR/build/$(basename "$entry" .loam)"
if [ ! -x "$bin" ]; then
    "$LOAM" "$entry" -o "$bin" || exit 1
fi

# vmmap prints human units; normalise to MB for the comparison.
to_mb() {
    awk -v s="$1" 'BEGIN {
        if (s == "") { print "0"; exit }
        u = substr(s, length(s), 1)
        v = substr(s, 1, length(s) - 1) + 0
        if (u == "K") v /= 1024
        else if (u == "G") v *= 1024
        printf "%.1f", v
    }'
}

# Median of a stream of numbers, one per line (BSD awk has no asort).
median() {
    sort -n | awk '{ v[NR] = $1 }
        END {
            if (NR == 0) { printf "n/a"; exit }
            if (NR % 2) printf "%.1f", v[(NR + 1) / 2]
            else printf "%.1f", (v[NR / 2] + v[NR / 2 + 1]) / 2
        }'
}

# field_median <log> <field> — median of `field=` over every frame but the first.
#
# The first frame is always dropped: it carries `interval=0.0` and a negative
# `other`, since its timestamps bracket nothing. Note the logged `more=` is what
# the *engine* reported, not what the scheduler did, so it cannot be used to pick
# out real frames — under ZEUS_FRAME_BENCH the hook redraws while `more` stays 0.
field_median() {
    grep '\[frame\]' "$1" 2>/dev/null | tail -n +2 \
        | sed -n "s/.*$2=\(-\{0,1\}[0-9.]*\).*/\1/p" | median
}

# run_one <label> [EXTRA=1]  →  $OUT/<label>.{log,vmmap,foot}
run_one() {
    label="$1"
    extra="${2:-}"
    log="$OUT/$label.log"
    vm="$OUT/$label.vmmap"
    : > "$log"

    if [ -n "$extra" ]; then
        env "$extra" ZEUS_FRAME_BENCH=1 ZEUS_MEM_DEBUG=1 ZEUS_FRAME_DEBUG=1 "$bin" >>"$log" 2>&1 &
    else
        env ZEUS_FRAME_BENCH=1 ZEUS_MEM_DEBUG=1 ZEUS_FRAME_DEBUG=1 "$bin" >>"$log" 2>&1 &
    fi
    pid=$!

    sleep "$SECS"

    # Capture the app's *own* window while it is up: the only way to tell a working
    # hand-off from a blank or glitching one when nobody is watching. `-l <id>`
    # rather than a screen rectangle, because a rectangle shows whatever is
    # frontmost — including some other app entirely, if the window failed to
    # raise. Falls back to a rectangle only when the id can't be found.
    id=""
    if [ -n "$WINID" ]; then
        "$WINID" "$pid" >"$OUT/$label.win" 2>&1
        id=$(awk '{ if ($4 * $5 > m) { m = $4 * $5; best = $1 } } END { print best }' \
             "$OUT/$label.win")
    fi
    if [ -n "${id:-}" ]; then
        screencapture -x -o -l "$id" "$OUT/$label.png" 2>"$OUT/$label.shot.err"
    else
        screencapture -x -R 400,300,700,400 "$OUT/$label.png" 2>"$OUT/$label.shot.err"
    fi
    shot=$(stat -f%z "$OUT/$label.png" 2>/dev/null || echo 0)

    # Full `vmmap`, not `-summary`: the summary row's last column counts *regions*
    # — which is where "5 buffers" came from — while only the region lines carry
    # each buffer's own size and pixel format.
    if ! vmmap -w "$pid" >"$vm" 2>"$vm.err"; then
        echo "  $label: vmmap could not inspect pid $pid ($(head -1 "$vm.err"))" >&2
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        : > "$OUT/$label.foot"
        return 0
    fi
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null

    foot=$(sed -n 's/^Physical footprint: *//p' "$vm" | head -1)
    peak=$(sed -n 's/^Physical footprint (peak): *//p' "$vm" | head -1)
    heap=$(sed -n 's/.*\[mem\] malloc=\([0-9]*\)KB.*/\1/p' "$log" | tail -1)
    printf '%s %s\n' "$foot" "$peak" > "$OUT/$label.foot"

    ndraw=$(grep -c '\[frame\]' "$log" 2>/dev/null)
    ivl=$(field_median "$log" interval)
    eng=$(field_median "$log" engine)
    oth=$(field_median "$log" other)

    printf '%-10s  now %7s   peak %7s   %s\n' \
        "$label" "${foot:-?}" "${peak:-?}" "${heap:+engine heap ${heap}KB}  crop ${shot:-0}B"
    if [ "${ndraw:-0}" -gt 1 ] 2>/dev/null; then
        printf '            %s frames: interval %sms' "$ndraw" "$ivl"
        awk -v i="$ivl" 'BEGIN { if (i > 0) printf " (%.0f fps)", 1000 / i }'
        printf '  engine %sms  other %sms\n' "$eng" "$oth"
    else
        printf '            only %s frame(s) logged — window never drew\n' "${ndraw:-0}"
    fi

    # The window store, verbatim — the rows are the evidence. `store` lines are
    # the buffers themselves, one per allocation, with size and pixel format
    # (`BGRA` = 4 bytes per pixel, `RGhA` = 8); the unmarked rows are the totals.
    grep '^IOSurface' "$vm" | grep 'SurfaceID' | sed 's/^/            store /'
    grep -E '^(IOSurface|IOAccelerator)' "$vm" | grep -v 'SM=' | sed 's/^/            /'
    if ! grep -q 'SurfaceID' "$vm"; then
        echo "            (no IOSurface at all: the owned path's pixels live in"
        echo "             malloc + CoreAnimation's texture, inside the footprint)"
    fi
}

echo "own_buffer: $entry   ($SECS s per path)"
echo "  binary $bin"
echo
run_one owned ""
echo
run_one surf "ZEUS_OWN_SURFACE=1"
echo
run_one appkit "APP_KIT=1"
echo
run_one wide "ZEUS_WIDE_GAMUT=1"

# Peak is the comparable number; `now` swings by tens of MB between runs.
if [ -s "$OUT/appkit.foot" ] && [ -s "$OUT/owned.foot" ]; then
    a_now=$(to_mb "$(cut -d' ' -f1 "$OUT/appkit.foot")")
    a_pk=$(to_mb "$(cut -d' ' -f2 "$OUT/appkit.foot")")
    o_now=$(to_mb "$(cut -d' ' -f1 "$OUT/owned.foot")")
    o_pk=$(to_mb "$(cut -d' ' -f2 "$OUT/owned.foot")")
    w_now=$(to_mb "$(cut -d' ' -f1 "$OUT/wide.foot" 2>/dev/null)")
    w_pk=$(to_mb "$(cut -d' ' -f2 "$OUT/wide.foot" 2>/dev/null)")
    s_now=$(to_mb "$(cut -d' ' -f1 "$OUT/surf.foot" 2>/dev/null)")
    s_pk=$(to_mb "$(cut -d' ' -f2 "$OUT/surf.foot" 2>/dev/null)")
    echo
    echo "comparison (MB)"
    printf '  %-14s %8s %10s\n' "" now peak
    printf '  %-14s %8.1f %10.1f\n' appkit "$a_now" "$a_pk"
    printf '  %-14s %8.1f %10.1f\n' owned "$o_now" "$o_pk"
    printf '  %-14s %8.1f %10.1f\n' surf "$s_now" "$s_pk"
    printf '  %-14s %8.1f %10.1f\n' wide "$w_now" "$w_pk"
    printf '  %-14s %8.1f %10.1f\n' "owned saves" \
        "$(awk -v a="$a_now" -v b="$o_now" 'BEGIN{printf "%.1f", a-b}')" \
        "$(awk -v a="$a_pk" -v b="$o_pk" 'BEGIN{printf "%.1f", a-b}')"
    printf '  %-14s %8.1f %10.1f\n' "surf saves" \
        "$(awk -v a="$a_now" -v b="$s_now" 'BEGIN{printf "%.1f", a-b}')" \
        "$(awk -v a="$a_pk" -v b="$s_pk" 'BEGIN{printf "%.1f", a-b}')"
    printf '  %-14s %8.1f %10.1f\n' "sRGB saves" \
        "$(awk -v a="$w_now" -v b="$a_now" 'BEGIN{printf "%.1f", a-b}')" \
        "$(awk -v a="$w_pk" -v b="$a_pk" 'BEGIN{printf "%.1f", a-b}')"
fi

if [ -s "$OUT/appkit.png" ] && [ -s "$OUT/surf.png" ]; then
    if cmp -s "$OUT/appkit.png" "$OUT/surf.png"; then
        echo
        echo "render: appkit and surf produced byte-identical crops — the IOSurface"
        echo "        path draws the same pixels as AppKit's store."
    else
        echo
        echo "render: appkit and surf crops differ — compare $OUT/appkit.png and"
        echo "        $OUT/surf.png by eye before reading it as a fault (the system"
        echo "        appearance can flip between runs, and screencapture needs"
        echo "        screen-recording permission or the crop is only a few KB)."
    fi
fi

if [ -s "$OUT/appkit.png" ] && [ -s "$OUT/owned.png" ]; then
    if cmp -s "$OUT/appkit.png" "$OUT/owned.png"; then
        echo
        echo "render: the two paths produced byte-identical crops."
        echo "        A tiny crop means screencapture had no screen-recording permission."
    else
        echo
        echo "render: crops differ — compare $OUT/appkit.png and $OUT/owned.png by eye."
        echo "        Do not read a difference as a rendering fault on its own: the app"
        echo "        follows the system light/dark appearance, which can flip between the"
        echo "        two runs and change every pixel. A crop of a few KB means"
        echo "        screencapture had no screen-recording permission; a crop the size of"
        echo "        the window that looks like some other app means the window never"
        echo "        raised (check \$OUT/<mode>.win)."
    fi
fi

echo
 echo "owned  = the owned bitmap, the DEFAULT (no variable)"
echo "surf   = ZEUS_OWN_SURFACE=1 (owned IOSurface, the lean one)"
echo "appkit = APP_KIT=1            wide = ZEUS_WIDE_GAMUT=1 (display profile)"
echo "raw: $OUT/<mode>.log and $OUT/<mode>.vmmap"
echo
echo "Reading it:"
echo "  - footprint/peak are Apple's phys_footprint, what Activity Monitor shows"
echo "  - only DIRTY counts; __TEXT, __OBJC_RO and mapped files are shared, not this app"
echo "  - WindowServer in Activity Monitor is a different process, not this one"
echo "  - the frame log prints one line per 30 frames, so ~16 lines is a full 8 s at"
echo "    60 fps; medians are over those samples, minus the first (its interval is 0.0)"
echo "  - the IOSurface rows carry the per-buffer detail: 'CA Whippet Drawable'"
echo "    lines show each store's size and pixel format (RGhA = 8 B/px, BGRA = 4)"
