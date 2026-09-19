# myapp — the Zeus example app

A docs/marketing site built with Zeus: a nav shell, seven routes with a layout
and its own theme, a small server API, and two headless test programs. One
entry (`app.loam`), every host the framework targets.

```
app.loam            entry: mounts the generated route table
zeus.toml           targets (web, macos, ios, android, server), routes dir, theme
routes/             page / layout / loading / error / not-found, plus route groups
app_routes.loam     generated from routes/ by `zeus routes` — do not hand-edit
components/         nav, catalog, foundations, patterns (the design-system pages)
theme.loam          palette + type roles
server/             api.loam
tests/              smoke.loam (`#[test]` fns), routes.loam (headless route drive)
```

Routes: `/`, `/about`, `/pricing`, `/blog`, `/blog/:slug`, `/components`,
`/patterns`.

## Run it

```sh
./run.sh myapp            # native macOS window (default)
./run.sh myapp web        # wasm + Vite at http://127.0.0.1:5174
./run.sh myapp ios        # iOS simulator
./run.sh myapp android    # Android device/emulator
./run.sh myapp backend    # the gRPC backend on 127.0.0.1:8080
./run.sh myapp build      # emit every target
```

`run.sh` regenerates `app_routes.loam` (`zeus routes`) and runs `loam check`
before building, so a broken route fails before a window opens. Directly:

```sh
./bin/loamc --run examples/zeus/myapp/app.loam
```

## Data

`/blog` gets its list from the backend over gRPC: `Blog.Posts`, declared in
`server/api.loam`, served by `server/main.loam`, consumed by
`routes/blog/loader.loam`.

```sh
./run.sh myapp backend    # terminal 1 — serves 127.0.0.1:8080
./run.sh myapp            # terminal 2 — the app; /blog fills in on arrival
```

`./run.sh myapp` brings up both halves — the app and the backend — so `/blog`
fills in. `./run.sh myapp backend` runs just the backend, and `LOAM_RPC_ADDR`
overrides the address on both ends.

Without a backend the route paints its heading plus `loading...` and stays
there: the client transport is `call_async` (non-blocking, no error channel), so
an absent server reads as a pending request rather than an error. That is the
deliberate trade — the synchronous `call` would block the frame. A timeout is a
follow-up.

On the **web** target the browser's RPC is same-origin (`default_addr()` returns
`""` for wasm), so `POST /Blog/Posts` arrives at the `zeus dev` server rather
than at the backend. The dev server forwards any `application/grpc-web*` POST to
`LOAM_RPC_ADDR` and returns the reply verbatim; with nothing on that port it
answers 502, so the failure is loud instead of being the SPA shell. Native needs
no such hop — `Blog.Posts` round-trips over h2c.

Three things to know before adding a method:

- **`#[server] fn` is not the boundary.** On a wasm build codegen replaces its
  body with `loam_panic("<server>", ...)` and generates no client stub, so
  calling one from the UI traps. Register with `app.rpc(...)` and call with
  `http.client().call_async(...)`, as `examples/zeus/counter` does.
- **`#[proto]` fields are `int`, `string`, or a list of those** — protobuf's
  `repeated`, sent as one key per element and decoded back into a `[]T`. Nested
  messages are not supported yet, so a list of structs has nowhere to go. Hence
  `Blog.Posts` returns `titles: []string` directly.
- **`res.resource` does not compile for a list `T`.** `std:res` is the intended
  abstraction for async data and its loader signature matches `call_async`, but a
  non-scalar `T` fails inside its own body (`assigning to 'Resource__string' from
  incompatible type 'Resource__int'`); `T = int` and `T = string` are fine. The
  loader here is that state machine written out, which is also what
  `examples/zeus/greeninfer` does for its `Signal<[]string>`.

## Display paths

Three of them, chosen by environment variable. This is the whole memory/frame-rate
trade in the native host.

```sh
./run.sh myapp                          # default: AppKit's store
ZEUS_WIDE_GAMUT=1 ./run.sh myapp        # the same, with the display profile (2x the store)
ZEUS_OWN_BUFFER=1 ./run.sh myapp        # owned bitmap
ZEUS_OWN_SURFACE=1 ./run.sh myapp       # owned IOSurface (the lean one)
APP_KIT=true ./run.sh myapp             # explicit spelling of the default
```

| path | how it draws | copies per frame | IOSurface | measured (screen-filling) |
|---|---|---|---|---|
| **default** (AppKit) | `drawRect:` paints straight into the window's backing store, which the compositor reads | **0** | 3 buffers, 54 MB reserved | **60 fps** (16.7 ms); peak 103–137 MB |
| `ZEUS_WIDE_GAMUT=1` | the same, but the window keeps the display's ICC profile | **0** | 3 buffers, 109 MB reserved | **60 fps** (16.6–16.7 ms); peak 176–219 MB |
| `ZEUS_OWN_BUFFER=1` | paints into a bitmap handed to the layer as `contents` | **1** — CoreAnimation materialises the whole frame into its own texture | 1 (~18 MB) plus that texture | **60 fps** (16.6 ms) with the sRGB window, 39 fps (25.4 ms) with `ZEUS_WIDE_GAMUT=1`; 51–55 MB |
| `ZEUS_OWN_SURFACE=1` | draws into an IOSurface and hands the layer the SURFACE, so our memory *is* the texture | **0** | 2 surfaces, reused (one is the layer's texture) | *to be measured* — see below |

The `ZEUS_OWN_SURFACE=1` row is deliberately blank: it is implemented and the
harness measures it, but no windowed run has been taken on the machine that
writes this file, so there is no number to copy. The expectation is the ~35 MB
recorded beside the IOSurface experiment in `hosts/desktop/mac.m`: it removes the
bitmap path's per-frame copy AND the compositor's own store, so it should be the
leanest of the four — and, unlike `ZEUS_OWN_BUFFER=1`, it should not cost a
full-frame copy. `sh packages/loam/tests/bench/own_buffer.sh` runs all four and
byte-compares their window crops.

The **two surfaces** are not an optimisation: CoreAnimation holds a surface as the
layer's texture while compositing, so locking it to draw the next frame deadlocks
the app after a few frames (this is the measured reason the earlier IOSurface
experiment was not shipped). They are used alternately with a NON-BLOCKING lock,
so a frame the compositor is still reading is **skipped** rather than waited on.

**Where the frames go.** Both paths run the same `ZeusDraw` callbacks. The engine's
own work (layout + step + paint) is 1.4–2.5 ms on the AppKit path and 5.0–6.6 ms on
the owned one. AppKit wins because its context *is* the surface the compositor
reads, so a finished frame never moves. The owned path paints into memory the GPU
cannot see, so every frame is copied into a texture — and what that copy costs
depends on the **store's depth**, because it also has to convert our 8-bit bitmap
into the surface's format: it fits in a 16.7 ms frame against an sRGB window
(16.6 ms) and does not against a half-float one (25.4 ms, `engine` unchanged at
5.0/6.1 ms — the difference is all on CoreAnimation's side of the frame).

A window only draws when the engine has something pending (an idle window draws
nothing at all — the frame clock stops), but any draw repaints the **whole
viewport**, so that copy is always a full frame.

**Where the memory goes.** The default path's footprint is almost all AppKit's
window store. At this window's pixel size one full-window buffer is **18.1 MB**
(2940 × 1612 × 4 bytes), and the compositor keeps **three** of them: 54 MB
reserved, plus a few 16 KB bookkeeping regions.

The **pixel format decides the buffer size**, and the *window* decides the pixel
format. A window with no colour space of its own inherits the display's ICC
profile ("Color LCD" → wide gamut), and CoreAnimation then backs it with an RGBA
*half-float* surface — `vmmap` names it `(RGhA)`, **8 bytes per pixel**, 36.2 MB a
buffer. Pinning the window to sRGB gets `(BGRA)` at 4 bytes per pixel. Every
colour the host can name is already a 24-bit packed value, so 8 bits per channel
holds all of them, and the store is 54 MB instead of 109 MB:

| window colour space | one buffer | all three | what `vmmap` reports | footprint peak |
|---|---|---|---|---|
| sRGB (the default) | 2940×1612 `BGRA`, 18.1 MB | 54 MB | `IOSurface 54.4M … 5` | 103–137 MB |
| display profile (`ZEUS_WIDE_GAMUT=1`) | 2940×1612 `RGhA`, 36.2 MB | 109 MB | `IOSurface 109.1M … 5` | 176–219 MB |

The **reservation** is the reproducible number: 54.4 MB against 109.1 MB, identical in
every run measured. The **footprint** is a sample, and `phys_footprint` *now* swings
over a wide range for the same window because it follows how many of the three
buffers the compositor has resident at that instant — one run caught the wide
window with two of its three buffers marked `PURGE=V` and **0K** resident, reading
62 MB `now` while peaking at 219 MB. That is the whole of the "same pixels, 60 to
150 MB" effect. Compare **peak**, and never a `now` from one mode against a `now`
from another.

The store is sized by the **window**, not by how much is painted, so it does not
shrink when the app draws less. Drawing volume only changes how much of the
reservation is *resident*: at rest the compositor can leave a buffer empty and
`PURGE=V`, and while drawing it fills them — the same window can be sampled at
36 MB or at 109 MB, which is the difference between a low and a high reading of
one unchanging UI.

There is **no scale knob**: the store is always the display's native resolution.
No GPU path is involved anywhere in this and none is needed — both paths are Core
Graphics onto a surface the compositor already reads, and the difference between
them is one copy, not a renderer.

Measured on a screen-filling window on a Retina MacBook Air: a 1470×837 pt window
whose content view is 1470×806 pt, so the store is 2940×1612 px. Both memory and
frame cost scale with window area × display scale², so a smaller window is much
cheaper in either mode.

## Measuring it yourself

```sh
ZEUS_MEM_DEBUG=1 ./run.sh myapp
# [mem] malloc=1094KB blocks=18452 phys=10066KB win=640x480 view=640x480
```

`phys` is Apple's `phys_footprint` — the same number Activity Monitor shows and
what a canvas app's cost really is here. `malloc` is the engine's own heap
(`std:zeus` nodes, signals, layout): it stays ~1–2 MB and is the number to watch
for a *leak*.

```sh
vmmap -summary <pid> | grep -iE "IOSurface|IOAccelerator"
```

`IOSurface` is the window's own store and, on the AppKit path, essentially the
whole footprint. **Grep the region lines, not the summary row**: `vmmap -summary`
gives one aggregate row whose last column counts *regions*, which is not a buffer
count — it is 2–3 full-window buffers plus 2–3 read-only 16 KB bookkeeping regions,
so "5" reads like five buffers when it is three. Plain `vmmap` prints each buffer,
and each line carries its own size and pixel format:

```
IOSurface  11087c000-111a98000  [ 18.1M 18.1M 18.1M 0K] rw-/rw- SM=SHM PURGE=N
           SurfaceID: 0x6a  2940x1612 (BGRA) 18.1M  'CA Whippet Drawable', shared with WindowServer[164]
```

The size and format on that line are what to compare between runs. `IOAccelerator`
is 64 KB of driver bookkeeping; `IOAccelerator (graphics)` is the interesting one
— 5–9.6 MB over 32–60 regions on the AppKit path, 16 KB over 1 region on the owned
path. Grep for both.

- **Read it while redrawing.** Individual buffers can be marked `PURGE=V` and drop
  to `0K` resident between frames, so `RESIDENT`/`DIRTY` for the window store
  swings between one buffer's worth and all three — 36 MB against 109 MB for the
  same window, and the reason two readings of an unchanging UI rarely match. The
  *reserved* total (the `IOSurface` row's size column) is the number that holds
  still, and the one to compare between runs.
- **Ignore the read-only numbers.** `__TEXT`, `__OBJC_RO`, `mapped file` and the
  ~1.6 GB of shared libraries are clean, shared pages — they are not this app's
  memory. In a `vmmap -summary`, only `DIRTY` counts.

`WindowServer` in Activity Monitor is **not** this app. It is the shared system
display server that composites every window on the desktop; its footprint moves
when any app opens a window and no app can shrink it.

### All three paths at once

```sh
sh packages/loam/tests/bench/own_buffer.sh examples/zeus/myapp 10
```

Runs the app three times — default, `ZEUS_OWN_BUFFER=1`, then `ZEUS_WIDE_GAMUT=1` —
and prints each one's footprint, peak, per-buffer IOSurface lines and frame-time
medians, with a crop of each run's own window (by window id, via `bench/winid.m`)
so a hand-off that blanks or glitches the window cannot pass by logging frames. It
sets `ZEUS_FRAME_BENCH=1`, which keeps the frame clock running: an idle window has
nothing pending and draws nothing, so without it the paths get sampled at
different workloads, or not at all.

The crop is per-window for a reason: a screen rectangle shows whatever is
frontmost, so a window that failed to raise is captured as some other app and
reads as a rendering difference that isn't one. The app also follows the system
light/dark appearance, so two runs can differ wholesale — check the two crops
against each other before believing a difference.

## Frame timing

```sh
ZEUS_FRAME_DEBUG=1 ./run.sh myapp
# [frame] interval=24.5ms setup=0.00 engine=5.9 image=0.00 set=0.00 other=18.6 more=1 next=344ms
# [tick]  period=16.7ms want=0
```

- `interval` — wall time between frames; the inverse is your frame rate. The log
  prints one line per 30 frames, so a 10 s run at 60 fps is ~20 lines, not 600.
- `setup` / `engine` / `image` / `set` — our work: graphics-context plumbing, then
  layout + step + paint, then the bitmap→`CGImage` hand-off, then the `contents`
  assignment and its commit.
- `other` — everything else in `interval`: CoreAnimation's work and the wait for
  the next vsync. Nothing of ours can time it.
- `more` / `next` — whether the engine wants another frame, and ms until its next
  async deadline. `more=1` with a large `next` means a frame is being drawn for a
  connection's sake even though nothing is due.
- `[tick]` — the display clock's own cadence. When it reads 16.7 ms the clock is a
  clean 60 Hz, so a slower `interval` is work, not scheduling.

## Tests

```sh
ZEUS_HEADLESS=1 ./bin/loamc test examples/zeus/myapp/app.loam   # tests/smoke.loam
ZEUS_HEADLESS=1 ./bin/loamc examples/zeus/myapp/tests/routes.loam -o /tmp/r && /tmp/r
```

`tests/routes.loam` drives the generated route table headlessly: it navigates to
every route, re-lays out, and asserts the page's marker text painted *and* that
the root shell outside the page slot survived — `zeus build` only proves the
targets link, this proves navigation actually paints.

`make test` runs both, plus `zeus build` (web + macOS artifacts) and a wasm smoke
run, as `zeus example myapp (…)`.
