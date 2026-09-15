# Zeus / Loam — production roadmap

Phase-by-phase plan for turning `packages/zeus/std/zeus.loam` (engine + design system)
(+ `packages/yuga/std/net`, `packages/http/std/http`, the C runtime) into a stack you can build real
products on — ecommerce tools, team chat (Slack/Teams-shaped), social apps.

Related: the next series (one host entry, honest `App` size, touch vs
pointer, `zeus.loam` tidy) lives in [downsides.md](downsides.md). This
file is the living Zeus/product plan (Phases 1–8 here); each phase
re-states its own rationale.
Status: tick phases off as they land, with their exit criteria as the
definition of done.

---

## 1. Where the stack stands today

| Layer | Today | Gap for real apps |
|---|---|---|
| UI core | Retained tree, per-prop thunks, arena recycling, windowed `For`/`VirtualList`/`VirtualTable`, scroll momentum + overscroll, headless layout/paint tests, DRAW goldens | nested scroll chaining, IME/multiline, images |
| Reactivity | `Signal<int>`/`Signal<float>` scalars (byte-typed cells), windowed `For`/`VirtualList`, reactive props (`width`, `position`, `grow`, `show`, …) | date/time helpers on float timestamps |
| Data plane | `#[proto]` codecs + unary gRPC-Web / h2c client **and** server, `Result<T>`, async `call_async` client, SSE + WebSocket streams (Phase 1b), bearer-token auth (`set_token` + `app.before`) | no bidirectional streaming; no async TLS |
| Networking | `std/net`: blocking POSIX sockets + TLS client (`net.tls_connect`, SecureTransport on macOS) + Phase 1 async transport (timers, non-blocking connect/poll/send, `http.call_async`/`sse_open`/`ws_open` steppers); wasm: `fetch_rpc` + async fetch slots | streaming bodies on wasm, async TLS |
| Storage | `sys.read_file` / `write_file` (whole-file), env | KV/table store, structured persistence, fs tree, app-data paths |
| Media | inline SVG + raster `Image` (PNG / JPEG / WebP / GIF, host decode) | gradients |
| Text | one weight per host; kind 10 multiline + caret/selection; mac IME; wasm paste | rich text spans, emoji metrics |
| Server | single-threaded Loam accept loop (HTTP/1.1 + h2c + one SSE/ws client per connection) | concurrency model for chat-class servers |
| Design system | ~50 shadcn widgets, palette tokens, goldens | monolith namespace, no DCE, fixed-pixel tuning |

## 2. Already fixed (do not re-plan)

Landmarks that are done and tested — the roadmap starts past them:

- Arena recycling: `For`/`refit` rebuilds free nodes, signals, handlers, effects (`zeus_for_recycle.loam`).
- Windowed lists: `VirtualList` / `For(..., row_h)` / `VirtualTable` of row structs + `TableCol`; `insert_item`; scroll momentum, overscroll, `scroll_to` (`zeus_virt.loam`).
- Lazy DatePicker: one 7×6 signal-driven grid, no rebuild on month/year change.
- Row shrink-before-wrap; `grow = 1` is elastic by default (CSS `flex: 1`), `shrink` prop overrides.
- Single following chart tooltip + cursor + band driven by one `act` signal.
- Palette tokens; no bare hex left in the chrome.
- Headless layout tests + byte-exact DRAW goldens wired into `make test`.
- Declarative props: `position`/`left`/`top`/`right`/`bottom`/`z_index`, `width`/`height` (all widgets incl. `Svg`), `grow`/`shrink`, `show` (bool thunk; signal shows win).
- Language: default/optional trailing parameters (`press: fn() = __noop`), incl. the declaring-module binding fix.
- Clean-exit tests (no trap-style failures in `compile_pass`).

## 3. The data plane is gRPC — no REST/CRUD layer needed

`#[proto]` structs give you schema + codecs; `http.app().rpc(...)` /
`http.rpc_call` give unary calls over gRPC-Web (HTTP/1.1) and h2c, in pure
Loam, client and server, native and wasm. That covers catalog, carts,
messages, presence payloads, sync — whatever the app is. Build on it:

1. **RPC is the only data API, and gRPC/proto the only wire format.** New
   endpoints = new proto structs + `app.rpc` / client calls. No JSON, no
   form-urlencoded, no ad-hoc route bodies, no other wire format — not even
   for interop. A `#[proto]` struct is the one way to describe data; a new
   endpoint is never a new format.
2. **No CRUD layer, ever.** No REST resources, no generic collection routes
   (`/users`, `/items/{id}`, PUT/DELETE conventions), no ORM-ish tables.
   State changes are named RPCs (`Cart.AddItem`, `Message.Send`), because
   they carry intent, versioning, and auth at the method level.
3. **What's missing is transport:**
   - **SSE** (server → client push; trivial on the existing accept loop) for
     notifications and coarse presence.
   - **WebSocket** (client + server, native and wasm) for chat/social realtime.
   - **Auth/session** convention at the RPC layer (token header, middleware fn).
4. **Streaming later, not now.** Unary RPC + SSE covers v1 products; true
   bidirectional streams can ride the WebSocket once it exists.

## 4. Remaining downsides, ranked

Severity is *for the target apps* (ecommerce / team chat / social), not for
demos. Phase column points into §6.

| # | Downside | Blocks | Effort | Phase |
|---|---|---|---|---|
| 1 | ~~No raster image primitive (draw op + host decode)~~ **Phase 2** | `zeus.Image`; hosts decode PNG/JPEG/WebP/GIF | — | 2 |
| 2 | ~~Single-line text, no IME on canvas hosts~~ **Phase 3** | chat compose exists; wasm IME is paste+composition, not a hidden field | — | 3 |
| 3 | ~~`For` rebuilds the whole list per push (recycling yes, O(n) still)~~ **Phase 5** | `VirtualList` / windowed `For` / `VirtualTable`; `push_item` / `insert_item` | — | 5 |
| 4 | ~~Nested scroll chaining~~ **self-improvement Phase 8** | leftover wheel at an inner bound goes to the parent (`zeus_scroll_chain.loam`) | — | 8 |
| 5 | ~~`Signal<int>` only (no float/date/bool scalar store)~~ **Phase 6** | float signals + float timestamp scalars (the byte-typed cell store was already generic; exit test proves it) | — | 6 |
| 6 | ~~Zeus monolith, no dead-code elimination~~ **Phase 7** | web bundle size, componentization | — | 7 |
| 7 | ~~a11y = labels only; no focus ring, thin keyboard model~~ **Phase 7** | visible keyboard focus + tab order across interactive chrome | — | 7 |
| 8 | ~~Fixed-pixel tuning; no density/font scaling~~ **Phase 7** | `set_scale` percent + `tk` tokens; components read the scale, not constants | — | 7 |
| 9 | ~~No TLS~~ **Phase 6** | blocking `net.tls_connect` (SecureTransport, macOS) + `https://` `Client` addresses; wasm keeps browser TLS via `fetch` | — | 6 |
| 10 | ~~No KV/persistence beyond whole-file read/write~~ **Phase 4** | `std:kv` file-backed; wasm is memory-only | — | 4 |
| 11 | Gallery wasm first paint ~3 min (counter wasm is instant) | catalog in the browser | med | follow-up |

Resolved by the landed phases: blocking sockets/async runtime (Phase 1), and no realtime transport (Phase 1b) — ws/SSE + the auth/session convention exist; what remains for chat-class servers is the §5.3 reactor (concurrent connections) and wasm ws via the browser's WebSocket. Phase 7 adds: per-widget dead-decl elimination (reachability-pruned codegen — the monolith stays one cycle-free module, but unused decls stop shipping), a keyboard focus ring with document-order tab traversal and Enter/Space activation, and a UI-scale token (`set_scale` / `tk`) that every chrome size reads.

## 5. Architecture decisions (the reasoning to preserve)

### 5.1 UI thread: single, forever
`arena.nodes`, `sigs`, `track`, focus, edit buffers are global and painting is
stateful; threading zeus would be a rewrite with no user-visible gain. The
industry answer (Android/UIKit/Swing) is: background I/O, completion **on the
one UI thread**. Keep it.

### 5.2 Async = one UI thread + Loam queues (no language threads)
Loam fns cannot run on arbitrary threads (globals, arenas). Shape:

```yuga
let c = http.client("127.0.0.1:8080")
c.call_async("Identity.Me", "", fn(resp) {   // gRPC I/O runs off the UI thread
    profile.set(decode_Me(resp))                // callback runs ON the UI thread
})
// Text("{{profile.get().name}}") updates. Nothing else moves.
```

The wire format is always proto over gRPC-Web — never JSON or an ad-hoc
format, so the callback hands back `#[proto]` bytes `decode_Me` already
understands.

- C side: **Phase 1 shipped queues in Loam with a minimal C seam** — monotonic
  clock + blocking sleep (`loam_rt.h`), non-blocking connect/poll/send
  (`net.c`), wasm async-fetch slots (JS XHR → per-frame drain). Every host
  frame calls `engine_layout`, which ticks the async world first (steppers,
  spawns, due timers) and lays out over whatever signals changed. Hosts
  sleep while idle and wake at the next deadline (`engine_next_ms`). Phase 6
  adds a **blocking** TLS client (`net.tls_connect`, SecureTransport) for
  console/headless/`http.call`; the UI's per-frame async world (`call_async`)
  still rejects `https://` addresses — a task pool or async TLS stays the
  option for truly non-blocking encrypted RPC.
- Loam side: `spawn(fn)` + completion callbacks run everything; the
  `async fn` / `await` sugar (Phase 1c) re-enters the same pump — sequential
  code reads like JS, still on the one thread.
- Timers (`sleep`, `after(ms, fn)`, interval) ship with the same queue.

### 5.3 Server: reactor, not threads
One thread, non-blocking `net` (poll/select in C), resuming Loam handlers on
readiness. Thousands of concurrent connections, zero locks. This is the chat
server story. Thread-per-connection in C *under* the API is a later option.

### 5.4 Concurrency primitives
- v1: **message passing only** (`channel<T>` between tasks and the UI loop) —
  composes with signals, keeps the no-shared-mutable-state property.
- **Phase 8 added language threads** for CPU-bound workloads, under a
  compile-time Send discipline: `thread.spawn` runs a detached worker; Send
  payloads (`Chan<T>`, captures) are plain data only, and worker code is
  verified to never touch module state or the C seam — the single-threaded
  runtime (globals, arenas, refcounts) stays sound because the compiler
  proves workers cannot reach it. No shared-memory threading: values cross
  threads, never memory.

### 5.5 New primitives ride the existing pipeline
Any draw primitive (image, gradient, rounded-text) travels
`scene.loam → platform.loam → zeus_plat.c → zeus_rt.h → mac/iOS/wasm/android
hosts` + both canvas loaders. Budget that blast radius per primitive; add
decode (PNG/JPEG) in the hosts, never in Loam.

## 6. Phases

Definition of done per phase = exit criteria, all verified by `make test`
(headless runs) unless noted. Tick boxes as phases land.

### Phase 1 — Async runtime, timers, non-blocking net  (foundation)
- [x] Async on the one UI thread, queues in Loam: `std/async.loam` (spawn / after / interval / cancel / per-frame steps), a minimal C seam (clock + sleep in `loam_rt.h`), hosts drain per frame via `engine_layout`'s tick (mac + wasm; mac sleeps idle and wakes at `engine_next_ms`).
- [x] `after(ms, fn)` / `interval(ms, fn)` / `spawn` / `cancel` in std (feed signals; `async_timers.loam`).
- [x] Non-blocking transport: `net.tcp_nb_connect` / `tcp_poll` / `tcp_send` / `tcp_so_error` — the UI never blocks on sockets.
- [x] First async-to-UI proof: `compile_pass` fake async op (`spawn`) completes, sets a signal, and a prop repaints through the headless pump (`zeus.pump`, no manual `engine_layout`).
- [x] `http.call_async(c, name, body, fn(resp))` gRPC-Web callback client (native sockets via per-frame steppers; wasm async fetch slots) — same `#[proto]` contract as `call`, no JSON layer.
- **Exit:** an app completes a gRPC round-trip while animating; headless test asserts completion → repaint without a manual `engine_layout`. **Green** (`zeus_async_rpc.loam` runs a real child-process `Echo.Ping` server; wasm fetch path compiles, needs a browser to run).

### Phase 1b — Realtime transport
- [x] SSE on the existing HTTP/1.1 server (server → client push): `read_head` / `sse_start` / `sse_send` per connection, `http.sse_open` streaming client (events per `data:` line, UI-thread callback).
- [x] WebSocket client (native **and** wasm) and server: pure-Loam RFC 6455 codec in `httpcore/ws.loam` (base64, SHA-1, accept key, masked/unmasked frames), `http.ws_upgrade` / `ws_send_text` server push, `http.ws_open` streaming client — native TCP steppers, wasm via the browser's WebSocket (JS loader bridge, per-frame drain). Live demo: `examples/zeus/ws` (native tick server + wasm page).
- [x] Auth/session convention: bearer token header (`http.set_token`), parsed server-side into `req_token()`, `app.before(fn)` middleware hook (alias of `use`) that can `reject()` → handler skipped, grpc-status 16 (`http_auth.loam`).
- **Exit:** a headless loop test streams N events over SSE/ws and the UI shows each (golden or probe assertions). **Green** (`zeus_stream.loam`: 5 SSE events + 5 ws messages → signal-fed label `events: 10`, repaint probes, child-process servers).

### Phase 1c — JS-shaped `async fn` / `await` (language sugar)
Callback APIs (`call_async`) are the runtime; this phase adds the syntax the
apps read and write. `await` never blocks and never threads — it re-enters
the one-thread async pump until the awaited mailbox fills (§5.2 model), so
sequential code reads like JS and still never touches a socket on the UI
thread.

- [x] Grammar + sema: `async fn name(...)`, `await expr` (contextual keywords — parser desugars `await e` to `async.await_value(e)`; typecheck rejects `await` outside an `async fn` body or inside a closure, like JS).
- [x] Awaitable surface: `Future<T>` mailboxes in `std:async` (`future` / `future_str` / `resolve` / `await_value` — Copy `T`; cells in `loam_rt.h`); `http.async_call(c, name, body)` is the awaitable gRPC-Web call (`Future<string>`); awaiting pumps timers/spawns/socket steppers until `resolve`, then returns the value.
- [x] Demo + tests: `examples/zeus/counter/macos/app.loam` is `async fn main` with two awaited RPCs (`let raw = await c.async_call(...)`) feeding the App; `compile_pass/async_await.loam` (sequential awaits, sync call of an async fn, pre-resolved future, `Future<int>` / `bool` / `float` / Copy struct / `[]int`); `compile_pass/async_await_stress.loam` (for/while/if/match/continue/break around awaits); `compile_fail/async_await_ctx.loam`, `compile_fail/async_future_not_copy.loam`.
- **Exit:** sequential awaited values flow in order through the pump and repaint; awaits outside `async fn` are compile errors. **Green** — caveat: awaiting re-enters the pump on the UI thread, so an await inside an event handler pauses host repaint until it returns; init/startup awaits (the counter App) and headless flows are the sweet spot, callbacks stay the reactive path.
- [x] Follow-up: `async`/`await` highlighting landed in the tree-sitter grammar + Zed/VSCode grammars (violet scopes in VSCode).
- [x] Follow-ups: widen `Future<T>` beyond string payloads; loop-and-branch-heavy bodies are already fine (no CPS split in this design) but want a stress test.

### Phase 2 — Image primitive
- [x] New draw op kind (raster) through scene → platform → C trampoline → hosts + canvas loaders.
- [x] Host decode of PNG / JPEG / WebP / GIF (ImageIO, Canvas `Image`, BitmapFactory); `zeus.Image(...)` widget + `SvgProps`-style props (`width` / `height` / `radius` clip).
- [x] Network image + cache by `src` (path, `data:` URI, or `http(s):` URL; http loads off the UI thread, next frame paints).
- [x] Intrinsic-size auto-sizing: a new `platform.plat_image_size` seam reports a decoded image's size in layout points (hosts answer from their decode cache; the C runtime falls back to parsing PNG/GIF/JPEG/WebP headers of a `data:` URI or local file, which is what makes headless goldens deterministic). An `Image` with no explicit height takes the source aspect ratio, and a widthless `Image` behaves like web `max-width: 100%` — it fills the container (device) width but never grows past the source. The Upload previews no longer wire `max_width` / `aspect` / `set_height` by hand. **Exit green** (`draw_golden/golden_image_auto`).
- **Exit:** a golden fixture with an image draw op; raster images render in the gallery (Upload preview; the static avatar/product `data:` placeholders were later removed — see the follow-up below). **Green** (`draw_golden/golden_image`).
- [x] Follow-up (resolved — wasm first-paint stall): `examples/zeus/gallery` wasm sat blank for ~40 s (up to minutes in some browsers) before first paint, while Cocoa/iOS/Android were instant. `app.wasm` + `loader.js` arrived in ms, so the stall was inside `zeus_start`. **Root cause: the hand-rolled wasm heap (`runtime/zeus_wasm_libc.c`) linearly scanned every block in the heap on each `malloc`, so the gallery's tree build (tens of thousands of allocations) was O(n²) — later widgets cost more, which is why the delay grew as content was added and why the 1×1 image blobs looked like the culprit.** Fixed by replacing the bump scan with a coalescing free list (boundary-tag footers, O(1) free, first-fit over free blocks only): `zeus_start` 40 s → ~0.6 s. Two more wins on top: wasm codegen now compiles at `-O2` (was `-O1`) and `loader.js` caches `measureText` widths per (px, string) (layout re-measures every label every frame) → `zeus_start` ≈ 0.46 s, idle frames 1–2 ms, and the engine idles cleanly (`engine_next_ms` already stops rAF like Cocoa). Profiling method: temporary per-stage timers in the gallery/sections + a headless-Chromium console pass, bisecting build → tab → section → widget.
- [ ] Follow-up (dev server, not the app): the gallery Vite plugin recompiles wasm **synchronously** (`spawnSync`) on every source change, blocking the dev server for the whole compile — in-flight requests (`/node_modules/vite/dist/client/env.mjs` and friends) stall and HMR lags. Switch the watcher to an async compile (`spawn`, serve the previous wasm meanwhile, `full-reload` when the new one is ready).
- [ ] Follow-up (optional): lazy-build `show_eq`-hidden tabs so startup skips off-screen catalog work, and skip layout of hidden subtrees on wasm — today the whole tree (all three tabs) builds and lays out every frame; the remaining cost is one-time and ~0.5 s, so this is polish.

### Phase 3 — Multiline text + IME
- [x] Multiline text input (kind 10 `multiline` / `wrap`): line breaks, caret, click-to-caret, arrow/home/end, shift-select.
- [x] IME composition on mac (`NSTextInputClient`); canvas/wasm ASCII + paste + compositionend.
- [x] Components: `TextArea` widget (shadcn-style chrome reusing `Input` tokens).
- **Exit:** a chat-compose headless test (type, edit, bind signal) + golden. **Green** (`zeus_textarea.loam`, `draw_golden/golden_textarea`).

### Phase 4 — KV persistence
- [x] `std/kv.loam` over the `loam_sys_*` seam: `get/set/delete/list`, file-backed, atomic-ish (`write` + `rename`).
- [x] App-data path helper (`kv.data_dir`, `~/Library/Application Support/<app>`; wasm: empty → in-memory).
- **Exit:** a compile_pass test that round-trips rows across two "processes"
  (reopen), plus gallery draft/autosave demo. **Green** (`kv_roundtrip.loam`; gallery Forms draft).

### Phase 5 — Virtualized lists + Virtualized table + scroll
- [x] Windowed `For` (render visible rows ± margin; recycle beyond the window).
- [x] Append/insert row ops that do not rebuild the whole list.
- [x] Scroll physics: momentum, overscroll clamp, `scroll_to` (signal-driven).
- **Exit:** headless test: 10k-row feed stays under a node/effect budget while
  scrolling (`arena_nodes()` bounded), items paint on demand. **Green**
  (`zeus_virt.loam`: 10k `VirtualList` + 10k `VirtualTable` of row structs /
  `TableCol` columns; append/insert; `scroll_to`; overscroll rubber-band).
  Demo: `examples/zeus/counter` (wasm / iOS / macOS / Android share `screen.loam`).

### Phase 6 — TLS + float/date signals
- [x] TLS on `net` (SecureTransport on macOS; wasm keeps browser TLS):
  blocking `net.tls_connect` (DNS + TCP + cert-verified handshake) whose
  handle drives `tcp_read/write/close`, so `http.call(http.client("https://…"))`
  is a plain-text-free `rt_call`.
- [x] `Signal<float>` / timestamp scalar store — the byte-typed sig cell
  store already covers any Copy type (int fast-path only), so floats and
  float timestamps need no backend change; the exit test proves it.
- **Exit:** `https` client test (skipped when offline), float-driven animation
  prop test. **Green** (`http_https_live.loam`: TLS GET against
  `example.com`, retries DPI-mangled first handshakes, skips when offline;
  `zeus_sig_float.loam`: float round-trips, a float timestamp scalar over
  `async.now_ms`, and a float-driven styled prop; `http_client.loam` pins
  `https://` address parsing).

### Phase 7 — Design system & a11y growth discipline
- [x] Zeus namespace pass or per-widget opt-in (DCE) before the monolith doubles.
- [x] Focus ring painting + visible tab order on native hosts.
- [x] Density/font-scale tokens (components read a scale signal, not constants).
- **Exit:** gallery unchanged visually at scale 1.0 (golden); a11y probe test asserts roles/focus order. **Green** — chose per-widget opt-in over a namespace split: the monolith stays one cycle-free module, and codegen now emits only decls reachable from the entry module + the C-seam roots (`loam_zeus_engine_*` etc. in `src/dce.c`, gated in `codegen_c.c`; typecheck/IR still verify everything, `LOAM_NO_DCE=1` restores full emission). Generated C: spec −64%, counter −47%, gallery −8%; counter wasm byte-identical (it reaches the whole engine).
- [x] Focus ring + tab order: `kb_focus` (keyboard-only ring — clicks clear it), 4-fill ring in theme role 36 painted in `scene`, every click target is a tab stop (Enter/Space activate it), Tab leaves single-line fields and soft-tabs (4 spaces) in multiline ones (`input.loam` + `zeus_plat.c` reroute plain Tab through the engine), roles/labels on interactive chrome (checkbox / radio / switch / slider / tab / combobox).
- [x] Scale tokens: `set_scale(50..200)` percent + `tk(x)` (identity at 100, so goldens hold); fonts, control geometry, glyph icons, and form chrome read `tk` instead of constants. Retained trees read the scale at build; `zeus.view` apps re-read it every frame (live zoom).
- **Tests:** `zeus_focus.loam` (roles → document-order tab traversal → ring paint deltas → Enter/Space activation → Tab semantics), `zeus_scale.loam` (150/100/clamp round-trips), `golden_focus_ring` (ring fills byte-exact), `golden_scale_gallery` (gallery chrome at scale 1.0); every pre-existing DRAW golden stays byte-identical.

### Phase 8 — Revisit only if demanded
- [x] Language threads + Send discipline + channels (CPU-bound workloads).
      `std:thread`: `spawn(fn)` runs a closure (or a same-module fn name) on a
      detached OS thread; `Chan<T>` is a bounded, mutex/condvar FIFO of Send
      payloads (`recv` blocks for workers, `try_recv`/`ready`/`try_send` never
      block for the UI drain). The discipline is compile-time, like the rest
      of the language:
      - **Send types** (`type_is_send`): plain data — int/float/bool/string
        and structs/arrays of those. Not Send: `[]T` (non-atomic refcount),
        `fn` (body unchecked), `Box`, any borrow. Enforced on `Chan<T>`
        payloads (typecheck, like the `Future<T>` Copy bound) and on `spawn`
        closure captures (threadcheck pass).
      - **Worker purity** (`src/threadcheck.c`, post-typecheck AST pass): the
        spawned fn and every statically-reachable callee may not touch module
        state (globals, arenas) or the C seam outside a pure allowlist
        (channel cells, fmt writes, string conversion, clock/sleep,
        wrapping ops, `spawn`/`running` itself). Callbacks must be closures
        or fn names — a fn value of unknown origin is rejected. Results come
        back through channels only; the UI drains and feeds signals.
      - C stays minimal: `loam_rt.h` gains only the pthread entry + a generic
        byte-FIFO + mutex/condvars (same category as the fut-slot table);
        queue policy, docs, and API shape live in `std/thread.loam`. wasm
        compiles with inert stubs (no threads); `async.busy()` includes
        `thread.running()`, so zeus hosts keep drawing frames while workers
        run and a UI drain sees results the frame they arrive.
- [ ] Rich text spans (bold/italic/inline color) once text is measured in-tree.
      **Still gated**: per-span advance/wrap/hit-test would fork the one-string
      one-font wrap/caret engine per span. Phase 13 landed the parser
      (`std:font`) and external loading (`zeus.use_font`), which route layout
      measurement through in-tree table parsing when a font is bound; hosts
      still draw with `plat_text`, and there is no in-tree font *embed*, so the
      web host needs a fetch bridge before it can measure in-tree. The gate the
      roadmap set (embed + parse a font) is now half done — parse and external
      load are green, embed remains open.

**Exit (threads):** a headless test proves N real workers compute on other
cores, return results through bounded channels, and the UI drains them
without blocking (`thread_chan.loam` — 3-worker pool, blocking recv + send,
try_send/ready drains, string + struct payloads, sentinel shutdown;
`thread_spawn_mod.loam` — spawn from an imported module, DCE keeps the
worker alive; `thread_busy.loam` — `async.busy()` true while workers run,
false after). compile_fail locks the discipline: non-Send captures/payloads
(`thread_spawn_cap_vec`, `thread_chan_vec`, `thread_chan_fn`), worker global
access (`thread_spawn_global`), opaque fn-value callbacks (`thread_spawn_cb_value`),
C-seam calls from workers (`thread_worker_seam`), and calls through fn
params (`thread_worker_fn_val`). **Green.** Caveat: native + iOS/Android
only (pthreads); wasm `spawn` is a documented no-op.

---

## 7. Cross-cutting notes

- **Validation is the moat.** Every phase ships a headless test; the DRAW
  goldens make visual regressions cheap to catch. Keep adding probe-style
  tests (`engine_find_text`, `node_x/y`) per phase.
- **Wasm parity** per phase where possible (net: `fetch`; ws exists in
  browsers; images decode via canvas) — note deviations in the phase notes.
- **Don't gold-plate.** Unary RPC + SSE + KV + images + multiline input
  already clears "useful ecommerce tool" and most social-app shapes; ws +
  virtualized lists clear chat. Ship those before streaming/bidi/rich text.
- **Tracking:** tick a phase only when its exit criteria run green in
  `make test` on this branch.
