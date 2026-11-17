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
# A fixture links what the driver would add: every `#![c_source(path,
# flags...)]` / `#![c_link(...)]` of the fixture and of the packages it
# imports. Fixtures that import a GUI
# package (zeus, zui, maya, raygui) are skipped. Needs zig on PATH
# (`brew install zig`). Each binary and object is deleted once its link is
# checked; only zig's compile cache stays in out/cross/cache (`nob clean`).
#
# The work runs on every core (CROSS_JOBS to change it): each fixture's C is
# emitted once (it is the same on every target), then every target × fixture
# link is its own job, writing only files named after both.
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT="$ROOT/out/cross"
export ZIG_GLOBAL_CACHE_DIR="$OUT/cache" ZIG_LOCAL_CACHE_DIR="$OUT/cache"
. "$ROOT/tools/fixture-deps.sh"

# `cross-check.sh --link <target> <name> <fixture.loam>`: one job. Links
# `$OUT/c/<name>.c` for <target> and leaves `$OUT/res/<target>/<name>`: empty
# on success, the first error lines otherwise.
if [ "${1:-}" = --link ]; then
    t=$2 b=$3 f=$4
    res="$OUT/res/$t/$b"
    objs=""; links=""; i=0
    deps=$(deps_of "$f" | sort -u)
    IFS='
'
    for dep in $deps; do
        IFS=' '
        dir=${dep%%|*}; rest=${dep#*|}; path=${rest%%|*}; flags=${rest#*|}
        if [ "$dir" = link ]; then links="$links $flags"; continue; fi
        i=$((i+1)); o="$OUT/obj/$t-$b-dep$i.o"
        # shellcheck disable=SC2086
        e=$(zig cc -target "$t" -O1 -fno-sanitize=undefined -w -c "$dir/$path" $flags -o "$o" 2>&1 | grep -E "error" | head -3)
        if [ -n "$e" ]; then
            echo "$path: $e" > "$res"
            rm -f "$OUT"/obj/"$t-$b"-dep*.o
            exit 0
        fi
        objs="$objs $o"
    done
    IFS=' '
    # The libraries the kept bindings name for this OS; on Linux, stubs.
    case $t in *-windows-*) os=windows ;; *) os=linux ;; esac
    libs=$(binding_libs_for "$os" "$OUT/stubs/table" "$OUT/c/$b.c")
    [ -n "$libs" ] && [ "$os" = linux ] && links="$links -L$OUT/stubs/$t"
    links="$links $libs"
    bin="$OUT/obj/$t-$b"
    # shellcheck disable=SC2086
    zig cc -target "$t" -std=gnu99 -O1 -w "$OUT/c/$b.c" $objs -o "$bin" $links 2>&1 | grep -E "error" | head -3 > "$res" || true
    # Only "it links" is checked: drop what this link made (the binary, a
    # Windows .pdb/.lib, the dep objects) right away.
    rm -f "$bin" "$bin".* "$OUT"/obj/"$t-$b"-dep*.o
    exit 0
fi

command -v zig >/dev/null || { echo "cross-check: zig not found (brew install zig)" >&2; exit 2; }
[ -x bin/loamc ] || ./nob.sh >/dev/null
targets=${*:-"x86_64-linux-gnu aarch64-linux-gnu x86_64-windows-gnu aarch64-windows-gnu"}
jobs=${CROSS_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
rm -rf "$OUT/c" "$OUT/obj" "$OUT/res"
mkdir -p "$OUT/cache" "$OUT/c" "$OUT/obj"
for t in $targets; do mkdir -p "$OUT/res/$t"; done

# 1. Each target's binding stub libraries and each fixture's C, at once.
# The libraries bindings name (`#[c("SSL_new", "link -lssl -lcrypto")]`),
# with each binding's `#[os]`. On Windows they are MinGW's own (`ws2_32`).
# On Linux zig ships no OpenSSL, so each library is a stub shared library
# whose every bound symbol is a stub fn: the link still checks that all else
# resolves, and `nob integration c_layouts` checks the names against the
# real headers.
rm -rf "$OUT/stubs"
mkdir -p "$OUT/stubs"
binding_lib_table > "$OUT/stubs/table"
for t in $targets; do
    case $t in *-linux-*) ;; *) continue ;; esac
    mkdir -p "$OUT/stubs/$t"
    while IFS='|' read -r oses sym libs; do
        case ",$oses," in ,,|*,linux,*|*,posix,*) ;; *) continue ;; esac
        for lib in $libs; do
            echo "void $sym(void) {}" >> "$OUT/stubs/$t/${lib#-l}.c"
        done
    done < "$OUT/stubs/table"
    for c in "$OUT/stubs/$t"/*.c; do
        [ -e "$c" ] || continue
        lib=$(basename "$c" .c)
        zig cc -target "$t" -shared -w "$c" -o "$OUT/stubs/$t/lib$lib.so" || echo "stub lib$lib" > "$OUT/res/$t.net"
    done
done
list="$OUT/fixtures"
: > "$list"
bin/loamc nob.loam --emit-c -o "$OUT/c/nob.c" >/dev/null && echo "nob nob.loam" >> "$list"
for f in packages/loam/tests/compile_pass/*.loam; do
    b=$(basename "$f" .loam)
    echo "$b" | grep -qE "$skip" && continue
    grep -qE "$gui" "$f" && continue
    echo "$b $f"
done > "$list.all"
# A fixture the compiler cannot emit is left out, as before.
# shellcheck disable=SC2016
# (xargs appends each `name path` pair after `_ $OUT`: they are $2 and $3.)
xargs -P "$jobs" -n 2 sh -c 'bin/loamc "$3" --emit-c -o "$1/c/$2.c" >/dev/null 2>&1 && echo "$2 $3"' _ "$OUT" < "$list.all" > "$list.ok" || true
cat "$list.ok" >> "$list"
wait

# 2. Every target × fixture link. On Windows std:net's TCP is Winsock.
work="$OUT/work"
: > "$work"
for t in $targets; do
    [ -e "$OUT/res/$t.net" ] && continue
    while read -r b f; do
        echo "$t $b $f" >> "$work"
    done < "$list"
done
xargs -P "$jobs" -n 3 sh "$ROOT/tools/cross-check.sh" --link < "$work"

# 3. One line per target, as before.
fail=0
for t in $targets; do
    if [ -e "$OUT/res/$t.net" ]; then
        echo "cross-check $t: binding stubs: $(cat "$OUT/res/$t.net")"
        fail=1
        continue
    fi
    n=0; bad=0
    for r in "$OUT/res/$t"/*; do
        [ -e "$r" ] || continue
        b=$(basename "$r")
        [ "$b" = nob ] || n=$((n+1))
        if [ -s "$r" ]; then
            echo "$t $b: $(cat "$r")"
            bad=$((bad+1))
        fi
    done
    echo "cross-check $t: nob + $n fixtures, $bad failed"
    [ "$bad" -eq 0 ] || fail=1
    rm -f "$OUT/net-$t.o"
done
rm -rf "$OUT/c" "$OUT/obj" "$OUT/res" "$OUT/stubs" "$OUT/work" "$list" "$list.all" "$list.ok"
exit $fail
