# myapp — the Zeus example app

A docs/marketing site built with Zeus: a nav shell, seven routes with a layout
and its own theme, a small server API, and two headless test programs. One
entry (`app.yuga`), every host the framework targets.

```
app.yuga            entry: mounts the generated route table
zeus.toml           targets (web, macos, ios, android, server), routes dir, theme
routes/             page / layout / loading / error / not-found, plus route groups
app_routes.yuga     generated from routes/ by `zeus routes` — do not hand-edit
components/         nav, catalog, foundations, patterns (the design-system pages)
theme.yuga          palette + type roles
server/             api.yuga
tests/              smoke.yuga (`#[test]` fns), routes.yuga (headless route drive)
```

Routes: `/`, `/about`, `/pricing`, `/blog`, `/blog/:slug`, `/components`,
`/patterns`.

## Run it

```sh
./run.sh myapp            # native macOS window (default)
./run.sh myapp web        # wasm + Vite at http://127.0.0.1:5174
./run.sh myapp ios        # iOS simulator
./run.sh myapp android    # Android device/emulator
./run.sh myapp build      # emit every target
```

`run.sh` regenerates `app_routes.yuga` (`zeus routes`) and runs `yugac check`
before building, so a broken route fails before a window opens. Directly:

```sh
./bin/yugac --run examples/zeus/myapp/app.yuga
```

## Display paths

Two of them, chosen by environment variable. This is the whole memory/frame-rate
trade in the native host.

```sh
./run.sh myapp                     # default: AppKit's store
ZEUS_OWN_BUFFER=1 ./run.sh myapp   # owned bitmap
APP_KIT=true ./run.sh myapp        # explicit spelling of the default
```

| path | how it draws | copies per frame | surfaces | measured |
|---|---|---|---|---|
| **default** (AppKit) | `drawRect:` paints straight into the window's backing store, which the compositor reads | **0** | 3 | ~60 fps, ~100–130 MB |
| `ZEUS_OWN_BUFFER=1` | paints into a heap bitmap handed to the layer as `contents` | **1** — CoreAnimation uploads the whole frame | 2 | ~40 fps, ~60 MB |

**Why AppKit wins on speed.** It isn't that it draws faster — both paths run the
same `ZeusDraw` callbacks. It's that AppKit's context *is* the surface the
compositor reads, so a finished frame never has to move. The owned path paints
into memory the GPU cannot see, so every frame is copied into a texture
(measured ~18–30 ms for a screen-filling window). Zeus repaints every pixel every
frame, so that copy is always a whole frame — which is why the owned path caps
around 40 fps no matter how cheap the UI itself gets.

**Why the owned path is leaner.** CoreAnimation needs no store for a view that
supplies its own content, so there are two surfaces instead of three. That
difference is the whole ~60 MB against ~100–130 MB.

There is **no scale knob**: the bitmap is always the display's native resolution.
If you need both 60 fps and a smaller footprint, the remaining option is a
`CAMetalLayer` host, where the drawable *is* the buffer Core Graphics paints
into and nothing is copied.

Measured numbers are from a screen-filling window on a Retina MacBook Air. Both
memory and frame cost scale with window area × display scale², so a smaller
window is much cheaper in either mode.

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

`IOSurface` is the window's drawable; `IOAccelerator` is graphics-driver
bookkeeping (~64 KB always — it is never the drawable). Two readings matter:

- **Read the peak while interacting.** These surfaces are marked `VOLATILE`, so
  macOS compresses and purges them when the app is idle and re-materialises them
  on the next frame. An idle reading can be half the peak.
- **Ignore the read-only numbers.** `__TEXT`, `__OBJC_RO`, `mapped file` and the
  ~1.6 GB of shared libraries are clean, shared pages — they are not this app's
  memory. In a `vmmap -summary`, only `DIRTY` counts.

`WindowServer` in Activity Monitor is **not** this app. It is the shared system
display server that composites every window on the desktop; its footprint moves
when any app opens a window and no app can shrink it.

## Frame timing

```sh
ZEUS_FRAME_DEBUG=1 ./run.sh myapp
# [frame] interval=24.5ms setup=0.00 engine=5.9 image=0.00 set=0.00 other=18.6 more=1 next=344ms
# [tick]  period=16.7ms want=0
```

- `interval` — wall time between frames; the inverse is your frame rate.
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
ZEUS_HEADLESS=1 ./bin/yugac test examples/zeus/myapp/app.yuga   # tests/smoke.yuga
ZEUS_HEADLESS=1 ./bin/yugac examples/zeus/myapp/tests/routes.yuga -o /tmp/r && /tmp/r
```

`tests/routes.yuga` drives the generated route table headlessly: it navigates to
every route, re-lays out, and asserts the page's marker text painted *and* that
the root shell outside the page slot survived — `zeus build` only proves the
targets link, this proves navigation actually paints.

`make test` runs both, plus `zeus build` (web + macOS artifacts) and a wasm smoke
run, as `zeus example myapp (…)`.
