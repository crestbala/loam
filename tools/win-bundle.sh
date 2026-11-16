#!/bin/sh
# Emit what tools/win-run.sh needs to build and run the non-GUI compile_pass
# fixtures on Windows, with bin/loamc. The selfhost
# workflow runs it on Linux and Windows and compares the C. For each fixture, its generated C (`<name>.c`), the
# extra C it links (`<name>.deps`, lines `dir|path|flags` or `link||flags`),
# its `.expected` output when it has one (`.expected-windows` instead, when
# that exists), and its `.trap` (the panic message
# stderr must hold) when it must panic; and the Loam compiler's own C
# (`loamc.c.txt`, not `.c`, so win-run.sh does not take it for a fixture).
#
#   tools/win-bundle.sh OUTDIR
#
# A fixture's `.deps` lists the libraries its kept bindings name for
# Windows (`link||-lws2_32` for std:net's Winsock calls).
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${1:?usage: win-bundle.sh OUTDIR}
mkdir -p "$OUT"
. "$ROOT/tools/fixture-deps.sh"
n=0
table="$OUT/.binding-libs"
binding_lib_table > "$table"
for f in packages/loam/tests/compile_pass/*.loam; do
    b=$(basename "$f" .loam)
    echo "$b" | grep -qE "$skip" && continue
    grep -qE "$gui" "$f" && continue
    bin/loamc "$f" --emit-c -o "$OUT/$b.c" >/dev/null 2>&1 || { rm -f "$OUT/$b.c"; continue; }
    deps_of "$f" | sort -u > "$OUT/$b.deps"
    libs=$(binding_libs_for windows "$table" "$OUT/$b.c")
    [ -n "$libs" ] && echo "link||$libs" >> "$OUT/$b.deps"
    [ -f "packages/loam/tests/compile_pass/$b.expected" ] && cp "packages/loam/tests/compile_pass/$b.expected" "$OUT/"
    # A fixture whose output names the OS keeps its Windows answer apart.
    [ -f "packages/loam/tests/compile_pass/$b.expected-windows" ] && cp "packages/loam/tests/compile_pass/$b.expected-windows" "$OUT/$b.expected"
    [ -f "packages/loam/tests/compile_pass/$b.trap" ] && cp "packages/loam/tests/compile_pass/$b.trap" "$OUT/"
    n=$((n+1))
done
# The Loam compiler's own C: tools/win-selfhost.sh checks stage 1 emits it.
bin/loamc packages/loam/compiler/loamc.loam --emit-c -o "$OUT/loamc.c.txt" >/dev/null
echo "win-bundle: $n fixtures and the compiler's C in $OUT"
rm -f "$table"
exit 0
