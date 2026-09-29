# dracula — design notes

A mutual-fund signal and backtest application in Loam + Zeus, over AMFI's
published NAV data. **dracula never executes a transaction.** It produces dated
buy/sell/hold signals from deterministic rules and reports whether those rules
would have made money after real costs. Every order is placed by hand.

Scope for v1: equity-oriented direct plans, daily NAV granularity. Stock
charts show NSE daily closes only (section 10); nothing about a stock is a
signal.

This file is the working record: what the language actually provides, what
Phase 0 has to build, and the order the layers land in. It is written against
the tree as it exists, not against an assumed tree.

---

## 1. What the repo provides, and what it does not

Verified by reading `packages/loam/std/`, `packages/zeus/std/`,
`packages/http/std/`, `docs/loam.md`, `docs/boundary.md`,
`docs/numeric-widths.md`, and `examples/zeus/myapp/`.

### Present

| Need | Where |
|---|---|
| `[]T` growable vector, `push` / `pop` / `.len` | language |
| `string` as bytes, `s[i]`, `s.len`, `string_from_bytes([]int)` | language |
| `map`, `json`, `result`, `test`, `unicode`, `font` | `std:` |
| File seam: `read_file`, `write_file`, `mkdir`, `rename`, `exists`, `list_dir`, `remove_path` | `std:sys` |
| Shell seam: `exec` (captures stdout), `exec_status` | `std:sys` |
| `open` / `close` / `listen` / `accept` / `read` / `write` | `std:net` |
| gRPC over `#[proto]`, `app.rpc(...)`, `http.client().call_async(...)` | `std:http` |
| File-backed string store, `open` / `get` / `set` / `list` | `std:kv` |
| Detached OS threads + Send-disciplined `channel<T>` | `std:thread` |
| Timers, `now_ms` (i64), `tick`, `future<T>` / `await_value` | `std:async` |
| `App`, `Box`, `Text`, `Button`, `Input`, signals, `engine_layout`, `engine_find_text`, `snapshot` | `std:zeus` |
| `Table`, `VirtualTable`, `LineChart`, `AreaChart`, `BarChart`, `Sparkline`, `Stat`, `Card`, `Tabs`, `DatePicker`, `Dialog`, `Alert`, `Badge`, `Empty`, `Skeleton`, `Pagination`, `Segmented`, `Select` | `std:zui` (`packages/zeus-components/`) |
| Route table, `navigate`, `param`, `param_live` | `std:router` |
| `Loading` / `Ready` / `Error` resource signals, route cache | `std:res`, `std:cache` |

### Absent — Phase 0 has to build these

| Need | Why the stdlib cannot supply it |
|---|---|
| `sqrt`, `ln`, `exp`, `pow`, `abs`, `floor` | There is no math library. `docs/loam.md` names a `math` module in the std *list*, but no `packages/loam/std/math.loam` exists. |
| Calendar arithmetic | `async.now_ms()` is a monotonic i64 and nothing else. No y/m/d type, no date parse, no civil-from-days. |
| Sort | No `sort` anywhere; `std:map` is a keyed container, not an order-by. |
| HTTP client | `std:http` is gRPC-only over `#[proto]`. `std:net` has TCP/TLS but no HTTP/1.1 framing. |
| Delimited parsing | No CSV module. |

Two documentation traps worth recording, because both mislead a reader of the
docs:

- `docs/loam.md` listed `math`, `str`, and `time` among `packages/loam/std/`
  when only `fmt`, `net`, `sys`, `thread`, `kv`, `json`, `result`, `test`,
  `unicode`, `font`, `map`, `async` were there. Phase 0 adds the missing
  `packages/loam/std/math.loam` and corrects both doc lists; `str` and `time`
  are struck from them rather than implemented, since nothing in dracula needs
  either.
- `std:kv` is the only consumer of the app-data directory;
  `kv.data_dir(app)` is the usable spelling of it.


### The exact widths that matter

`int` is `i32`, `float` is `f32`. From `docs/numeric-widths.md` and a probe
program compiled against this tree:

- `f64` is a real, usable type: a fn with an `f64` parameter takes an untyped
  `2.0` literal directly, and `f64` arithmetic mixes freely.
- `i64` is a real, usable type: `i64(1234567890123)` survives the round trip.
- An untyped integer literal does **not** adopt an `i64` parameter type, so
  every i64 that enters this program is converted explicitly: `i64(a + b)`.
- **An untyped float literal is `float` (f32), not `f64` — including at module
  scope.** `let LOG2 = 0.6931…` is f32, so `f64(k) * LOG2` is an
  f64-times-f32 mix and a compile error. Every float constant and every
  `let mut acc = 1.0` accumulator that mixes with `f64` carries an explicit
  `: f64`. This is not style; it is the difference between compiling and not.
- `fmt.write_float` takes `float` (f32). Every f64 printed is `f32(x)` first —
  which costs visible precision in a debug print but does not affect the
  computed value. A test that compares printed output instead of values will
  therefore pass when it should not.
- Mixed-width arithmetic is a compile error. `i32` + `i64` will not typecheck.
- `+ - *` trap on overflow at every width; `wrapping_*` / `saturating_*` opt out.
- `f64(i64(x))` truncates toward zero; `i32(x)` narrows and traps on overflow.

Consequences, applied everywhere below:

- **Money is `i64` paise.** `int` silently overflows on a portfolio well under
  ₹2.1 crore in paise. NAV is `i64` paise. No float for money, ever.
- **Ratios and indicator intermediates are explicit `f64`.**

### `#[proto]` limits

`#[proto]` fields are `int`, `string`, and a list of either. No floats, no
nested messages. A NAV series therefore crosses as parallel arrays
`dates []int` + `navs []string` (paise as decimal strings — an `int` field is
i32, and a NAV series in paise crosses 2^31 at about ₹2.1 crore, which a long
history of one scheme can reach).

`routes/scheme/[code]/loader.loam` decodes with `string_to_i64`.

---

## 2. Correctness rules — the non-negotiables

1. **No look-ahead.** Day *t*'s NAV publishes after day *t*'s close. An order
   placed before the 3pm cutoff on *t* is allotted the NAV of *t*, which was
   unknown when the order went in. A signal computed from day *t*'s NAV is
   therefore executable only at **day *t+1*'s NAV**. Every fill uses the next
   published NAV strictly after the signal date. No flag turns this off.
2. **Warm-up bars are excluded from all reported results.** A 200-day SMA has
   no opinion before day 200.
3. **Costs always.** Exit load (per scheme, per holding period, typically 1%
   inside 365 days), stamp duty 0.005% on every purchase and switch-in, STT
   0.001% on redemption of equity-oriented funds. Gross and net are reported
   separately; **net is the headline**. The expense ratio is *inside* the NAV —
   subtracting it again would be double-counting, and it is not done anywhere.
   Capital-gains tax is a separate optional post-tax view, never folded into net.
4. **Walk-forward or it does not ship.** Rolling train/test, parameters frozen
   from train to test, in-sample and out-of-sample side by side, and plain
   language when OOS degrades.
5. **Benchmark on every report:** buy-and-hold of the same scheme, and a
   monthly SIP in the same scheme, identical window, identical costs.
6. **Money is i64 paise.**
7. **Deterministic rules only. No machine learning in v1.** Every signal names
   the rule that fired and the inputs it saw.
8. **Survivorship bias must be visible.** A scheme that leaves the AMFI file is
   marked dormant with its last-seen date; its history is never deleted.

---

## 3. Phase 0 — the missing primitives

Each one is small, and each one is a place where a silent numerical bug would
poison every downstream result. Each lands with its own tests before anything
depends on it.

| Module | Contents | Tests |
|---|---|---|
| `std:math` — `packages/loam/std/math.loam` | `sqrt`, `ln`, `exp`, `pow`, `abs`, `floor`, `ceil`, `round`, `trunc`, `min`, `max`, `clamp`, `is_finite` — pure Loam, `f64` | `packages/loam/tests/inlang/math_tests.loam`: 20 hand-checked values per fn incl. edges, relative error < 1e-10; `exp(ln(x)) ≈ x` across a decade range; `ln(exp(x)) ≈ x` over the exponent range |
| `std:date` — `packages/loam/std/date.loam` | Dates as `int` `YYYYMMDD`; `parse_amfi`, `to_days`, `from_days`, `days_between`, `weekday`, `add_months`, `month_start`, `month_end`, `fmt_date`, `fmt_iso`, `parse_iso` | `packages/loam/tests/inlang/date_tests.loam`: `from_days(to_days(d)) == d` over every day of four years, across the 2000 century boundary, and sparsely over 1900–2200; known weekdays; `days_between` across a leap day; every AMFI rejection case |
| `core/sortx.loam` | Stable in-place merge sort over `[]i64`, plus a companion that permutes a parallel index/label array | sorted, reverse-sorted, all-equal, single, empty; ties keep input order |
| `core/stats.loam` | mean, variance, sample sd (Welford), downside deviation, max drawdown + duration, CAGR | hand-computed 20-point fixture; Welford agrees with naive to 1e-9 when well-conditioned and stays sane where naive does not |
| `server/fetchx.loam` | `curl -sS --max-time 30` behind `fetch_text(url) -> string` + explicit ok/fail; a non-zero status or empty body is a failure that leaves the cache untouched | status / empty-body handling; server target only |
| `core/csvx.loam` | Tolerant `;`-delimited split: skips blanks and section headers, reports the line number of anything unparseable | blank lines, section headers, short rows, quote-free fields |

**Where a Phase 0 module goes.** Two of the six are general-purpose arithmetic
and calendar code and now live in the language std:

- `std:math` and `std:date` know nothing about funds, so they sit in
  `packages/loam/std/` next to `fmt` and `kv`, are importable from any app, and
  are tested by the language suite (`nob test`) rather than by dracula's.
- `sortx`, `stats`, `csvx` and `fetchx` stay in dracula: `sortx` is a
  `[]i64`-specific ranking primitive, `stats` takes NAV-shaped series, `csvx`
  parses AMFI's file layout, and `fetchx` is a server-only seam. They are tested
  by `loam test examples/zeus/dracula/app.loam`.

Both std modules are implemented in Loam rather than via a C seam because
`docs/boundary.md` reserves C for platform bindings, and a pure-Loam
implementation is deterministic across every target. `docs/loam.md` was already
listing a `math` module among `packages/loam/std/` that did not exist; that
listing is now true, and `date` joins it.


---

## 4. Framework constraints

**RPC and the client/server boundary**

- `#[proto]` is `int` / `string` / list-of-those. Floats are not available; a
  series crosses as `dates []int` + `navs []string`.
- Register with `app.rpc(name, |body| ...)`; call with
  `http.client().call_async(name, bytes, cb)`.
- **A dead server is an empty body.** `call_async` hands a refused connect
  or its 8-second timeout to `cb` as `""`. An all-default reply also encodes
  to nothing, so every reply the client must tell apart from a dead server
  carries `live: 1` (`StatusResp`, `CatalogResp`, the `Stock*` replies), and
  a reply that echoes the request (`SchemeResp.code`) is matched by the
  code the closure captured. Each screen shows "The data server did not
  answer" with a Retry, never a loading line that does not end.
- **`#[server] fn` is not a boundary.** On wasm, codegen replaces the body with
  `loam_panic` and generates no client stub, so calling one from the UI traps.
  What keeps server code out of a client build is the **import graph**, exactly
  as `myapp/server/db.loam` demonstrates. `server/db.loam` and
  `server/main.loam` are imported by nothing a client compiles;
  `app_routes.loam` and the page modules import only `server/api.loam`.

**Signals — two traps that cost `myapp` real debugging time**

- **Never create a signal at module scope.** It segfaults inside
  `zeus.snapshot` while building the tree. Create it inside a `start()` called
  from `app.loam` before `zeus.App`, and use the zero handle
  (`Signal { id: 0 }`) as both the uninitialised marker and the
  already-started latch — the pattern `myapp/routes/blog/loader.loam` uses.
- **`Signal<struct owning a list>` loses the list's elements** when the store
  happens inside an async callback: the value reads back fine and nothing
  paints. One signal per field.

**Storage**

- `std:kv` for small settings: watchlist, strategy parameters, last-sync date.
- NAV history goes to CSV files under the app-data directory via
  `sys.write_file` to a `.tmp` path then `sys.rename`. There is no database and
  none is added.

**Threads**

- Backtests are CPU-bound and run on `std:thread` workers. `spawn` callbacks
  must be Send: plain data in, results back over a `channel<T>`, **no module
  state and no C seam inside the worker**. No file reads inside a worker —
  load the series first and hand it in as data.

**UI**

- `std:zeus` for primitives, `std:zui` for components. The components already
  exist; none are rebuilt.
- `zui.LineChart(labels, names, vals, w, h)` takes flat `[]int`. A ten-year
  daily NAV series is ~2,500 points: it is downsampled to roughly 200 before it
  reaches a chart, and scaled out of paise so it fits i32. Downsampling takes
  the **last value in each bucket**, never an average — an average invents
  prices that never existed and would not line up with the trade markers.
- `LineChart` takes optional `LineChartProps`: `fit` (the y axis spans the
  values instead of starting at 0), `decimals` (paise print as rupees),
  `label_every` (0 is about eight x labels), `dots`. A value equal to
  `zui.CHART_GAP` is not drawn and breaks its line. The defaults draw what
  the chart always drew. The chart math is i64, so an equity curve in paise
  times the plot height no longer overflows i32.
- The scheme chart is two series read at the same kept bars: NAV, and the
  200-day average computed over the whole history. The average is
  `CHART_GAP` wherever it is not ready — warm-up is a gap, never a zero
  line. The range control (1 year, 3 years, full history) only picks the
  first bar; it does not restart the average's warm-up.
- A table row that navigates is `zui.TableRowCols(..., on_click = ...)`; an
  action on the row (Remove, Add) sits beside the row, not inside it.
- Catalog search is a server RPC that returns one page of at most 25. The
  1,341 names never cross as a list and never become 1,341 widgets.
  `zui.Combobox` is not used for it: its options are fixed when it is built
  and an empty query renders every option.
- No new host code, no C, no GPU.

---

## 5. Layout

Anything general-purpose — math, dates, sorting, statistics, CSV — belongs in
the **language std** under `packages/loam/std/`, imported as `std:<name>`, with
its tests in `packages/loam/tests/inlang/`. Nothing in those modules knows about
funds. `core/` holds only what is specific to this domain: the NAV series type,
fund indicators, the Indian cost model, strategies and the backtester.

```
packages/loam/std/           the language std — shared, reusable
  math.loam    Phase 0 — f64 elementary functions
  date.loam    Phase 0 — YYYYMMDD calendar + day numbers
packages/loam/tests/inlang/
  math_tests.loam  date_tests.loam

examples/zeus/dracula/
core/
  sortx.loam      Phase 0 — stable sort + index permutation
  stats.loam      Phase 0 — Welford, drawdown, CAGR
  csvx.loam       Phase 0 — tolerant delimited parsing
  series.loam     NavSeries: scheme code, parallel dates []int + navs []i64 (paise)
  indicators.loam pure fns over a NavSeries: sma, ema, rsi, macd, rolling vol,
                  drawdown, momentum over n months, relative strength vs a
                  reference scheme. No hidden state, no allocation surprises.
  costs.loam      exit load, stamp duty, STT — one place, auditable in isolation;
                  optional capital-gains module
  strategy.loam   a strategy is data: entry rule + exit rule + sizing.
                    (a) NAV above its 200-day SMA, exit below, with a minimum
                        holding period to dodge the exit load
                    (b) 12-month momentum across the universe, monthly rebalance,
                        hold top N
                    (c) plain monthly SIP — the baseline everything is measured
                        against
  backtest.loam   event-driven; fills at the NEXT published NAV after the signal
                  date; fractional units, cash in paise, exit load per-lot by
                  holding period (FIFO). Out: CAGR, XIRR, Sharpe, Sortino, max
                  drawdown, drawdown duration, win rate, profit factor,
                  turnover, full transaction log.
  walkforward.loam rolling windows, parameter freeze, IS/OOS comparison
server/
  api.loam      #[proto] contracts + RPC names
  catalog.loam  catalog search: every query word in the name or at the start
                of the code; one bounded page (Data.Catalog, Stocks.Search)
  nse.loam      NSE bhavcopy URL, fetch (curl + unzip), and parse (section 10)
  tape.loam     stock close cache, apart from the NAV cache (section 10)
  fetchx.loam   Phase 0 — curl wrapper
  amfi.loam     AMFI ingestion: parse NAVAll.txt, backfill from the historical
                NAV report, map scheme codes to names, flag dormant schemes
  store.loam    CSV cache under the app-data directory, one file per scheme
                code, incremental append, .tmp then rename
  validate.loam ingest guards (section 6)
  main.loam     http.app(); app.rpc(...) per method; app.listen(8080)
components/
  book.loam     watchlist, scheme cache, catalog search, the focused chart
  lab.loam      backtest and walk-forward runs
  data.loam     fund sync status and the day walk
  stocks.loam   stock search, the focused stock chart, the stock day walk
  present.loam  formatting, the last-in-bucket downsample, chart series
  ui.loam       stat grid, server-down state, a table row with an action
routes/
  page.loam                        holdings: search and add, rows open the chart
  screener/page.loam               rank the watchlist, rows open the chart
  scheme/page.loam                 Charts: search every cached fund
  scheme/[code]/page.loam          NAV chart with range control, 200-day
                                   average, indicator stats, signal history
  stocks/page.loam                 search cached NSE symbols
  stocks/[symbol]/page.loam        daily close chart, same range control
  backtest/page.loam               strategy picker, equity curve, transaction table
  backtest/walkforward/page.loam   IS vs OOS, side by side
  data/page.loam                   sync status, last successful fetch, rejected rows
```

Targets for v1: `macos` and `server`. `web` is kept compiling; it is not chased.

`app_routes.loam` is generated by `zeli routes` and is never hand-edited.

### Route shape

The pages above are the app's own route table, mounted through the generated
`app_routes.loam` like any other Zeus tree. `myapp`'s `routes/layout.loam` is
the model: a retained root shell (theme + top bar + nav) with only the page
slot rebuilding on navigation. Route groups (`(marketing)`) and the scaffold's
`blog/` are `zeli new` boilerplate for a docs site; dracula's route set
replaces them, and the scaffold's `blog/` subtree is deleted rather than left
dead in the tree.

---

## 6. Data validation — `server/validate.loam`

Silent data corruption is the most likely way this application loses money, and
it will not look like a bug. It will look like a signal. Every row is checked on
ingest; a rejected row is logged with its reason and surfaced on the data page,
never dropped quietly.

- Reject a NAV that moves more than 20% from the previous published NAV for that
  scheme. For an equity fund that is almost always a data error or a scheme
  merger, not a market move. It is a *warning* when the scheme also moved its
  plan or option (a direct/regular switch shows up as a large jump).
- Reject a non-positive or unparseable NAV.
- Reject an out-of-sequence or duplicate date; the cache is append-only in date
  order.
- Flag a gap of more than 5 calendar days between consecutive NAVs as
  suspicious; refuse to compute indicators across it without an explicit
  acknowledgement.
- A scheme that vanishes from the daily file is marked dormant with its
  last-seen date. Its history is not deleted.
- Record the fetch timestamp and source URL for every sync, so a bad run can be
  identified and rolled back.

Stock closes are validated in `server/nse.loam` and `server/tape.loam`, not
here; see section 10.

---

## 7. Tests

Run headless:

```sh
# dracula's own tests (sortx, stats, csvx, costs, backtest, routes)
ZEUS_HEADLESS=1 ./bin/loamc test examples/zeus/dracula/app.loam

# server: fetch seam, validation, ingest, catalog search, the stock cache
ZEUS_HEADLESS=1 MAYA_HEADLESS=1 ./bin/loamc test examples/zeus/dracula/server/main.loam

# route drive: every page paints with no server and no start()
ZEUS_HEADLESS=1 ./bin/loamc examples/zeus/dracula/tests/routes.loam -o /tmp/dracula-routes && /tmp/dracula-routes

# the language std, which now includes math and date
make test            # or: ./bin/nob test inlang
```

- Every Phase 0 module, as specified in section 3.
- Each indicator against a hand-computed 20-point fixture.
- The backtest engine against a hand-computed 20-point scenario with known
  entries, known exits, and ending cash correct to the paisa, exit load
  included.
- **The look-ahead test:** feed a NAV series truncated at date *t*, assert the
  signals up to *t* are identical to those from the full series. Assert every
  fill is dated strictly after its signal.
- **The cost test:** one buy-and-redeem inside 365 days, asserting exit load,
  stamp duty and STT each land at the expected paise, and that the expense ratio
  was subtracted nowhere.
- A headless route drive modelled on `myapp/tests/routes.loam`, asserting each
  page paints.
- The chart series: the 200-day average is `CHART_GAP` until it is ready, a
  late range keeps the average from earlier history, and a bucket keeps its
  last price (`present_tests`).
- Catalog search: every word must match, exact code first, the page is
  bounded (`catalog_tests`).
- Stock closes: header-driven parse, a trailing-comma header, a comma in a
  name, a file for another day refused, the cache append-only in date order,
  and a price file ahead of its index not written twice (`tape_tests`).

---

## 8. Non-goals for v1

No order placement. No broker integration. Equities as **daily closes only**
(section 10): no stock signals, no stock backtest, no costs applied to a
stock. No intraday data, no options. No machine learning. No automation of
any kind — dracula displays, the operator decides.

---

## 9. Build order

1. Phase 0 modules, each with its tests green.
2. `server/` — fetch, parse AMFI, validate, cache. Prove a real sync end to end.
3. `core/series` + `indicators`, with tests.
4. `core/costs` + `backtest`, with tests.
5. Strategies, then walk-forward.
6. Routes and UI last.

Each layer is reviewed before the next one depends on it.

---

## 10. Stock closes

A chart of a share's daily close, from a real published file, cached apart
from the AMFI NAVs. Nothing else about stocks is in scope.

**Source.** NSE's capital-market bhavcopy, UDiFF layout, one zip per trading
day, public, no key:

```
https://nsearchives.nseindia.com/content/cm/BhavCopy_NSE_CM_0_0_0_<YYYYMMDD>_F_0000.csv.zip
```

- Files exist at this URL from **2 Jan 2024**. Earlier days answer 404
  (checked back to Jan 2023); the pre-2024 layout lives at a different URL
  and is not read.
- A weekend or exchange holiday answers 404 with an HTML page. That is
  "closed", not a failure, and nothing is written.
- The CDN resets a request with no browser User-Agent, so `nse.fetch_day`
  sends one. curl writes the zip to `/tmp`, `unzip -p` extracts it, and both
  temporary files are removed on every path.

**Columns.** Comma-separated, one header row, no quoting. Fields are found by
header name. Early 2024 headers end in a trailing comma: an empty last name
that no row carries, and not a field.

| Field         | Used as                                   |
|---------------|-------------------------------------------|
| `TradDt`      | trade date `YYYY-MM-DD`; must equal the requested day, or the whole file is refused |
| `TckrSymb`    | symbol (`RELIANCE`, `M&M`, `BAJAJ-AUTO`)  |
| `SctySrs`     | series; only `EQ` is kept                 |
| `FinInstrmNm` | company name                              |
| `ISIN`        | ISIN                                      |
| `ClsPric`     | close, rupees to 2 places → i64 paise     |

**Validation**, as for funds:

- A close that is not positive, or does not parse, is rejected.
- A row whose field count is not the header's (a comma in a name) is rejected.
- A close dated on or before the symbol's last cached close is rejected
  (`duplicate date`, `out of sequence`). The check reads the price file's own
  last line as well as the index, so a run that stopped between the two
  cannot write a day twice.
- Closes are **unadjusted**. A one-day move over 20% is almost always a split
  or a bonus, so it is cached and counted as a warning, not rejected;
  rejecting it would leave every later close judged against a price the
  share no longer has. The stock page shows the count.
- Every fetch records its id, stamp, URL, day, and counts. Rejected rows keep
  their file line and reason and are shown on the Data page, first 20.

**Cache.** `kv.data_dir("dracula")/stocks`, beside `nav/`, never inside it:

```
stocks/symbols.txt     symbol;name;isin;last_date;last_paise;bars
stocks/fetches.txt     id;stamp;url;day;accepted;rejected;warnings
stocks/rejects.txt     id;line;symbol;date;reason
stocks/px/<file>.csv   date;paise;fetch_id
```

`<file>` is the symbol with any byte outside `A-Z 0-9` written as `_` and
two hex digits (`M&M` → `M_26M`). Append-only; each write is `.tmp` then
rename.

**RPCs.** `Stocks.Search` (one page of symbols and names), `Stocks.Series`
(one symbol's closes as `dates []int` + `closes []string` paise),
`Stocks.Day` (fetch and cache one day), `Stocks.Status` (last fetch, symbol
count, first 20 rejects). The Data page walks a date range one day per
`Stocks.Day`, like the fund history.

**What does not apply.** The 200-day rule, momentum ranks, exit load, stamp
duty, STT, and the backtest are fund machinery. The stock page draws the
close (last close in each bucket, the same range control) and reports the
last close, the number of closes cached, and the count of moves over 20%.
