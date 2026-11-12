# Loam energy plan — Phase E (greenest, measured)

Goal: make Loam the greenest safe systems language *by measurement*, not
slogan. Energy = joules for the same work. This phase builds the ruler,
records the first baseline, then removes work.

Rule: every claim cites a number from `tools/energy-baseline.sh`.

## E0. Ruler (this step)

- Corpus: `packages/loam/tests/energy/*.loam` — small CLI programs only
  (no GUI), each printing one verifiable number:
  `hello`, `sum_loop` (1e9 wrapping adds), `fib` (1e8 wrapping adds),
  `vec_push` (1M pushes), `str_cat` (100k appends), `map_insert` (200k),
  `json_parse` (parse + reserialize std fixture).
- Runner: `tools/energy-baseline.sh [--quick]`
  - builds each with `bin/loamc` (fresh, `LOAM_NO_FRESH=1`)
  - captures `loamc --cost`, `LOAM_TIME=1` stage lines, object bytes
    (`--emit-obj`), binary bytes, best-of-3 wall time via python3
    `perf_counter`
  - energy: `powermetrics` (macOS arm64) when available and permitted,
    else RAPL/`perf stat -e power/energy-pkg/` on Linux, else wall*CPE
    estimate clearly marked `ESTIMATED`
  - writes `docs/energy/baseline-<date>-<host>.md` + machine-readable
    `docs/energy/baseline-<date>-<host>.csv`
- C twins: `packages/loam/tests/energy/c/*.c` — same algorithms in C11,
  compiled `cc -O2`, timed/measured by the same runner. The honest
  comparison target.
- Gate: runner exits 0 only if every program's stdout matches its
  `.expected`. Numbers are recorded, never asserted (hardware varies).

## E1. Remove work (joules follow)

1. **DCE the runtime.** `loam_rt` (~2.7k lines of native runtime) is linked
   whole into every program — see the `E0` object-bytes column. Moving
   `fmt/str/conv/buf` verbs to Loam (`runtime-in-loam.md` R1) lets DCE drop
   unused code. Target: hello object bytes `~120KB -> <25KB`.
2. **Elide provable traps.** Extend `boundscheck` (range analysis, u32
   loop counters, `wrapping_*` -> single instruction). Numeric-loop
   benchmarks must converge toward C.
3. **`--release` flag.** Today everything compiles `-O1`. Add
   `-O2 -flto` for release while keeping trap semantics. Dev stays `-O1`.
4. **Arena/node diet.** `UiNode 488B`, `~476-492 B/node`
   (`docs/mem-baseline.md`) — bytes streamed bound every walk
   (self-host §7e: `376B -> 56B` node cut walk `14.3ms -> 3.2ms`).
   Phases 2-4 of mem-baseline apply directly to energy.

## E2. Carbon-aware (differentiator)

`sys.grid_intensity()` + `loamc --defer-until-green` (already scoped in
`self-hosting-loam.md`). Long `nob build/test` and servers prefer
low-carbon windows. No other systems language ships this.

## Exit criteria

- `tools/energy-baseline.sh --quick` green on macOS arm64 + Linux x86_64
- `docs/energy/baseline-*.md` committed with the 7-program table
- `nob test` still green (energy corpus is *not* in `compile_pass`,
  so the suite is unaffected by design)
- Next: E1.1 DCE, measured against this baseline
