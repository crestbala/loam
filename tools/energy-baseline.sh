#!/bin/sh
# Energy baseline for Loam (Phase E0, see docs/energy-plan.md).
#
# Builds every program in packages/loam/tests/energy/*.loam (plus C twins
# in energy/c/*.c when present), captures --cost, LOAM_TIME stages, object
# bytes (--emit-obj),
# binary bytes, and best-of-3 wall time. Energy via powermetrics (macOS,
# needs sudo) or RAPL/perf (Linux); otherwise wall-time estimate marked
# ESTIMATED.
#
# Usage:
#   tools/energy-baseline.sh           # full (slow: sum_loop ~1e9 iters)
#   tools/energy-baseline.sh --quick   # skip sum_loop/map_insert/json_parse
#
# Run from anywhere; paths resolve relative to the repo root.
set -u

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
LOAM=$ROOT/bin/loamc
SRC=$ROOT/packages/loam/tests/energy
OUT=$ROOT/out/energy
DOC=$ROOT/docs/energy

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

if [ ! -x "$LOAM" ]; then
  echo "energy-baseline: build the compiler first: cd \"$ROOT\" && ./nob.sh" >&2
  exit 1
fi

# Which programs to run.
if [ "$QUICK" = 1 ]; then
  PROGS="hello fib vec_push str_cat"
else
  PROGS="hello sum_loop fib vec_push str_cat map_insert json_parse"
fi

HOST="$(uname -s)-$(uname -m)"
STAMP=$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT" "$DOC"
CSV="$DOC/baseline-$STAMP-$HOST.csv"
MD="$DOC/baseline-$STAMP-$HOST.md"

size_of() { stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; }

# --- energy backend -------------------------------------------------------
ENERGY_MODE="ESTIMATED"
if [ "$(uname -s)" = "Darwin" ] && command -v powermetrics >/dev/null 2>&1; then
  # powermetrics needs root; try once, fall back silently.
  if sudo -n powermetrics --samplers cpu_power -n 1 >/dev/null 2>&1; then
    ENERGY_MODE="powermetrics"
  fi
elif command -v perf >/dev/null 2>&1 && perf stat -e power/energy-pkg/ true 2>/dev/null; then
  ENERGY_MODE="rapl"
fi

# Print joules for running "$@" once, or "n/a" when no backend.
measure_joules() {
  case "$ENERGY_MODE" in
    powermetrics)
      out=$(sudo -n powermetrics --samplers cpu_power -n 1 --show-process-energy 2>/dev/null & PM=$!
        sleep 0.3; "$@" >/dev/null 2>&1; sleep 0.3; kill $PM 2>/dev/null; wait 2>/dev/null) || { echo "n/a"; return; }
      echo "$out" | awk '/CPU Power|package energy|Energy:/ {print $NF; exit} END {if (NR==0) print "see-log"}'
      ;;
    rapl)
      perf stat -e power/energy-pkg/ -x, "$@" 2>&1 >/dev/null | awk -F, '/energy-pkg/ {gsub(/[^0-9.]/,"",$1); print $1 " J"; exit}'
      ;;
    *) echo "n/a (ESTIMATED from wall time)" ;;
  esac
}

best_of_3() {
  # High-resolution best-of-3 via python3 (macOS time -p has only 2 decimals).
  python3 - "$@" <<'EOF'
import subprocess, sys, time
best = None
for _ in range(3):
    t0 = time.perf_counter()
    subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, check=False)
    dt = time.perf_counter() - t0
    best = dt if best is None or dt < best else best
print(f"{best:.3f}")
EOF
}

{
echo "program,lang,run_s_best3,bin_bytes,obj_bytes,ir_allocs,ir_drops,ir_bounds,ir_traps,energy,energy_mode"
for p in $PROGS; do
  echo "energy-baseline: $p" >&2
  bin="$OUT/${p}_loam"
  LOAM_NO_FRESH=1 "$LOAM" "$SRC/$p.loam" -o "$bin" >/dev/null 2>&1 || { echo "energy-baseline: $p compile failed" >&2; continue; }
  objfile="$OUT/${p}.o"
  "$LOAM" --emit-obj "$SRC/$p.loam" -o "$objfile" >/dev/null 2>&1 || objfile=""
  cost=$("$LOAM" --cost "$SRC/$p.loam" 2>&1)
  allocs=$(echo "$cost" | awk '/allocs/ {print $NF}'); drops=$(echo "$cost" | awk '/drops/ {print $NF}')
  bounds=$(echo "$cost" | awk '/bounds checks/ {print $NF}'); traps=$(echo "$cost" | awk '/overflow traps/ {print $NF}')
  stages=$(LOAM_TIME=1 LOAM_NO_FRESH=1 "$LOAM" "$SRC/$p.loam" -o "$bin" 2>&1 | grep -o 'loam: [a-z]* [0-9.]*s' | tr '\n' ';')
  echo "energy-baseline:   stages: $stages" >&2
  obytes=0; [ -n "$objfile" ] && obytes=$(size_of "$objfile")
  bbytes=$(size_of "$bin")
  wall=$(best_of_3 "$bin")
  joules=$(measure_joules "$bin")
  # Gate: stdout must match the committed .expected (recorded, not asserted
  # on timing — hardware varies).
  exp="$SRC/$p.expected"
  got=$("$bin")
  if [ -f "$exp" ]; then
    want=$(cat "$exp")
    if [ "$got" != "$want" ]; then
      echo "energy-baseline: FAIL $p output mismatch: got [$got] want [$want]" >&2
      echo "$p,loam,$wall,$bbytes,$obytes,$allocs,$drops,$bounds,$traps,$joules,$ENERGY_MODE,OUTPUT-MISMATCH"
      continue
    fi
  fi
  echo "$p,loam,$wall,$bbytes,$obytes,$allocs,$drops,$bounds,$traps,$joules,$ENERGY_MODE"
  # C twin when present.
  if [ -f "$SRC/c/$p.c" ]; then
    cbin="$OUT/${p}_c"
    cc -O2 "$SRC/c/$p.c" -o "$cbin" 2>/dev/null || continue
    cwall=$(best_of_3 "$cbin")
    cbbytes=$(size_of "$cbin")
    cjoules=$(measure_joules "$cbin")
    echo "$p,c,$cwall,$cbbytes,0,0,0,0,0,$cjoules,$ENERGY_MODE"
  fi
done
} | tee "$CSV" | awk -F, 'NR==1 || NF>5 {printf "%-12s %-5s %10s %10s %10s %7s %7s %7s %7s %s\n", $1,$2,$3,$4,$5,$6,$7,$8,$9,$10}'

{
echo "# Energy baseline — $STAMP ($HOST)"
echo
echo "Runner: \`tools/energy-baseline.sh${1:+ $1}\`. Backend: \`$ENERGY_MODE\`."
echo "Compiler: \`$(cd "$ROOT" && git rev-parse --short HEAD)\`"
echo "CC: \`$(cc --version | head -n 1)\`"
echo
echo '```'
cat "$CSV" | awk -F, 'NR==1 || NF>5 {printf "%-12s %-5s %10s %10s %10s %7s %7s %7s %7s %s\n", $1,$2,$3,$4,$5,$6,$7,$8,$9,$10}'
echo '```'
echo
echo "Columns: best-of-3 wall seconds, native binary bytes, codegen object"
echo "bytes, IR alloc/drop/bounds-check/trap counts from \`loamc --cost\`."
} > "$MD"
echo "energy-baseline: wrote $CSV and $MD" >&2
