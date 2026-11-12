# Energy baseline — 20261112-231913 (Darwin-arm64)

Runner: `tools/energy-baseline.sh`. Backend: `ESTIMATED`.
Compiler: `8364d8e4`
CC: `Apple clang version 17.0.0 (clang-1700.6.3.2)`

```
program      lang  run_s_best3  bin_bytes    c_bytes ir_allocs ir_drops ir_bounds ir_traps energy
hello        loam        0.00      33560     120124       0       1       0       0 n/a (ESTIMATED from wall time)
hello        c           0.00      33432          0       0       0       0       0 n/a (ESTIMATED from wall time)
sum_loop     loam        0.00      33696     120671       0       0       0       1 n/a (ESTIMATED from wall time)
sum_loop     c           0.00      33432          0       0       0       0       0 n/a (ESTIMATED from wall time)
fib          loam        0.05      33688     120798       0       0       0       1 n/a (ESTIMATED from wall time)
fib          c           0.05      33432          0       0       0       0       0 n/a (ESTIMATED from wall time)
vec_push     loam        0.00      34272     121569       0       3       0       2 n/a (ESTIMATED from wall time)
vec_push     c           0.00      33560          0       0       0       0       0 n/a (ESTIMATED from wall time)
str_cat      loam        0.00      34240     120779       0       4       0       1 n/a (ESTIMATED from wall time)
str_cat      c           0.00      33560          0       0       0       0       0 n/a (ESTIMATED from wall time)
map_insert   loam        0.09      34880     140787       0      24      21      21 n/a (ESTIMATED from wall time)
map_insert   c           0.05      33744          0       0       0       0       0 n/a (ESTIMATED from wall time)
json_parse   loam        0.07      68992     197828       0     335      60      94 n/a (ESTIMATED from wall time)
```

Columns: best-of-3 wall seconds, native binary bytes, generated C
bytes, IR alloc/drop/bounds-check/trap counts from `loamc --cost`.
