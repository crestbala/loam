#!/bin/sh
# Emit what tools/win-run.sh needs to build and run the non-GUI compile_pass
# fixtures on Windows (see docs/platforms.md), with bin/loamc. The selfhost
# workflow runs it on Linux and Windows and compares the C. For each fixture, its generated C (`<name>.c`), the
# extra C it links (`<name>.deps`, lines `dir|path|flags` or `link||flags`),
# its `.expected` output when it has one (`.expected-windows` instead, when
# that exists), and its `.trap` (the panic message
# stderr must hold) when it must panic; and the Loam compiler's own C
# (`loamc.c.txt`, not `.c`, so win-run.sh does not take it for a fixture).
#
#   tools/win-bundle.sh OUTDIR
#
# Fixtures that open a socket (their C keeps a `tcp_` or `tls_` fn of std:net)
# are left out: std:net's TCP answers -1 on Windows until it has a Winsock
# port (a known gap, not needed to self-host). Fixtures that only import the
# http package to parse or encode run as usual.
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
    if grep -q 'loam_net_tcp_\|loam_net_tls_' "$OUT/$b.c"; then rm -f "$OUT/$b.c"; gap="$gap $b"; continue; fi
    deps_of "$f" | sort -u > "$OUT/$b.deps"
    [ -f "packages/loam/tests/compile_pass/$b.expected" ] && cp "packages/loam/tests/compile_pass/$b.expected" "$OUT/"
    # A fixture whose output names the OS keeps its Windows answer apart.
    [ -f "packages/loam/tests/compile_pass/$b.expected-windows" ] && cp "packages/loam/tests/compile_pass/$b.expected-windows" "$OUT/$b.expected"
    [ -f "packages/loam/tests/compile_pass/$b.trap" ] && cp "packages/loam/tests/compile_pass/$b.trap" "$OUT/"
    n=$((n+1))
done
# The Loam compiler's own C: tools/win-selfhost.sh checks stage 1 emits it.
bin/loamc packages/loam/compiler/loamc.loam --emit-c -o "$OUT/loamc.c.txt" >/dev/null
echo "win-bundle: $n fixtures and the compiler's C in $OUT"
[ -n "$gap" ] && echo "  left out (they open sockets; std:net has no Winsock port):$gap"
exit 0
