#!/bin/sh
# Emit what tools/win-run.sh needs to build and run the non-GUI compile_pass
# fixtures on Windows (see docs/platforms.md), with whatever bin/loamc is: the
# C compiler on Linux, the seed-built Loam compiler on Windows. The selfhost
# workflow runs it on both and compares the C. For each fixture, its generated C (`<name>.c`), the
# extra C it links (`<name>.deps`, lines `dir|path|flags` or `link||flags`),
# its `.expected` output when it has one, and its `.trap` (the panic message
# stderr must hold) when it must panic; and the Loam compiler's own C
# (`loamc.c.txt`, not `.c`, so win-run.sh does not take it for a fixture).
#
#   tools/win-bundle.sh OUTDIR
#
# Fixtures that call into std:net are left out: runtime/net.c has no Windows
# port yet (a known gap, not needed to self-host).
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${1:?usage: win-bundle.sh OUTDIR}
mkdir -p "$OUT"
. "$ROOT/tools/fixture-deps.sh"
n=0; gap=""
for f in packages/loam/tests/compile_pass/*.loam; do
    b=$(basename "$f" .loam)
    echo "$b" | grep -qE "$skip" && continue
    grep -qE "$gui" "$f" && continue
    bin/loamc "$f" --emit-c -o "$OUT/$b.c" >/dev/null 2>&1 || { rm -f "$OUT/$b.c"; continue; }
    if grep -q 'loam_net_' "$OUT/$b.c"; then rm -f "$OUT/$b.c"; gap="$gap $b"; continue; fi
    deps_of "$f" | sort -u > "$OUT/$b.deps"
    [ -f "packages/loam/tests/compile_pass/$b.expected" ] && cp "packages/loam/tests/compile_pass/$b.expected" "$OUT/"
    [ -f "packages/loam/tests/compile_pass/$b.trap" ] && cp "packages/loam/tests/compile_pass/$b.trap" "$OUT/"
    n=$((n+1))
done
# The Loam compiler's own C: tools/win-selfhost.sh checks stage 1 emits it.
bin/loamc packages/loam/compiler/loamc.loam --emit-c -o "$OUT/loamc.c.txt" >/dev/null
echo "win-bundle: $n fixtures and the compiler's C in $OUT"
[ -n "$gap" ] && echo "  left out (std:net has no Windows port):$gap"
exit 0
