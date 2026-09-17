# Zeus

Full-stack UI for Loam. Theme, layout, and the draw list live in Loam.
Hosts only replay that list: Cocoa, iOS, and Android use their native 2D
APIs; the browser uses **Canvas2D**. Backend APIs stay `std:http`.

This is not GPUI-on-the-web. GPUI's `gpui_web` crate talks **WebGPU** (wgpu)
with a WebGL2 fallback. Zeus does **not** use WebGPU or WebGL. The scene list
zeus already records (`fill` / `text` / `clip`) is replayed as Canvas2D calls.
iOS does **not** use UIKit widgets or iOS look-and-feel. Android does **not**
use Material widgets or system colors.

## Decisions

| Question | Choice |
|---|---|
| Rasterizer | **Canvas2D**. No WebGPU, no WebGL. |
| Text | `fillText` / `measureText` in the loader (v1). Native Core Text will not match pixel-for-pixel. |
| Reactivity | **Signals**, retained tree. `zeus.mount` builds once. `zeus.view` (rebuild every frame) is the VDOM alternative and is not the Zeus default. |
| View type | `zeus.Node`. Loam has no traits / `impl View`. |
| Imports | `import "std:zeus"`. No glob prelude. |
| Events | `on_click = handler` on `Box` / `Button`. Intern copies the closure env (`loam_fn.env_size`). Captures stay Copy-only. `bind` on `Input` writes a `Signal<string>`. |
| HTTP types | Shared `.loam` module with `#[proto]` structs + `*_rpc()` name helpers. `http.client()` fills the addr per `--target` (wasm same-origin, mac/iOS loopback, Android emulator `10.0.2.2:8080`). |
| Wire protocol | gRPC-first: first-party traffic is always gRPC-Web / h2c. REST + JSON (`std:json`, `http.rest_*`, `app.json`) exist only as third-party interop, reachable from `#[server]` code. |
| Routing | `std:router`: a `[]Route` table in a retained shell; loaders registered with `on_load` start before paint. |
| Data | `std:res` resource signals (`Loading`/`Ready`/`Error`); `std:cache` over `std:kv` for `revalidate` + `invalidate`. |
| SSR | Out of scope. First paint is client WASM. |

## Routing and data

`import "std:router"` mounts a route table into a retained shell; only the
page slot rebuilds on navigation, so shell state survives. The current path is
a `Signal<string>`, and each page builds under `zeus.Boundary`.

```loam
router.Router([]Route {
    Route { pattern: "/", build: Home },
    Route { pattern: "/post/:slug", build: Post },
}, RouterProps {})
```

Route loaders are registered separately and start the moment navigation
begins, before the page paints — every loader for the target route starts, so
independent fetches run concurrently (no waterfalls, §5.5):

```loam
router.on_load("/post/:slug", fn(p) {
    http.client("").call_async(get_post_req(router.param(p, "slug")), fn(reply) {
        post.set(decode_Post(reply))
    })
})
```

Async data is a resource signal (`std:res`): a `Signal` holding
`Loading | Ready | Error`, re-runnable with `reload` (stale completions are
dropped). `std:cache` adds a route/params-keyed cache over `std:kv` —
`revalidate` is the `ttl` in seconds, `invalidate` forces a recompute, and the
cache is file-backed on native (offline) and memory-only on wasm.

First-party traffic (Zeus client ↔ your Loam server) is **always gRPC**:
`#[proto]` over gRPC-Web in the browser, h2c natively, with no JSON option.
Third-party traffic gets the optional interop path: `http.rest_get` /
`http.rest_send` on the `#[server]` side, `app.json(path, handler)` for inbound
webhooks, and `#[json]` structs sharing the `#[proto]` codegen machinery. That
path is unreachable from client code (wasm has no sockets), so REST stays an
escape hatch rather than a second calling convention.

## Metadata and the web shell

No SSR. Each route may define `fn meta(p: Params) -> Meta` (`title`,
`description`, `canonical`, `og_image`, `og_type`, `robots`, `jsonld`). On web
that fills the document `<head>`; on native the same struct is meant to fill
the window title and share sheet.

`zeli build --targets web` emits **one HTML file per route** (never a single
`index.html`), plus `sitemap.xml` and `robots.txt`. It does this by generating a
small Loam program that calls each route's `meta()` — and `paths()`, for a
dynamic route that enumerates its concrete URLs — printing JSON the CLI reads
back. `<body>` holds only the canvas and the loader script: HTML here is a
document-header format for crawlers, not a UI runtime. Pass `--base
https://host` for absolute canonical URLs and OG images.

## Accessibility mirror

The UI is canvas, but a screen reader needs structure. `zeus.engine_a11y_dump()`
serializes the visible semantic tree as `depth<TAB>role<TAB>label<TAB>x<TAB>y<TAB>w<TAB>h`
lines from the roles/labels widgets already set (`button`, `textbox`, `slider`,
`list`, …). The web loader relays it each frame into a visually-hidden,
never-painted DOM mirror (`aria-hidden` on the canvas, real roles on the
mirror), so the UI stays canvas while VoiceOver has something to read. On
macOS `ZeusView.accessibilityChildren` builds `NSAccessibilityElement`s from
the same dump, with real screen frames from the geometry columns. iOS and
Android are not wired yet.

Routing fills the host title too: `router` applies the matched route's
`Meta.title` on navigation, deep link, and back, which sets `document.title` on
web and retitles the open window on macOS. On web the router's stack is
mirrored to `history.pushState` / `replaceState` / `back`, and the loader
routes the app to `location.pathname` at boot and on `popstate` — so a URL
built by `zeli build` opens on the right route.

## Pipeline

```
component fn
  → Node tree (zeus)
  → layout / hit-test (zeus, shared)
  → scene draw list (zeus)
  → native: Cocoa / iOS / Android  plat_fill / plat_text / plat_image
     wasm:   Canvas2D imports in loader.js
```

Browser events → `loader.js` → exported `zeus_pointer_*` / `zeus_key` →
`zeus_handle_*` → signal writes → next `requestAnimationFrame` paint.

## Build

Native (headless snapshot):

```
ZEUS_HEADLESS=1 ./bin/loamc packages/loam/tests/compile_pass/zeus_snap.loam -o packages/loam/tests/tmp/zeus_snap
```

WASM (needs a clang that has `wasm32`, e.g. Homebrew `llvm` or wasi-sdk).
Apple `/usr/bin/clang` does **not**. Set `LOAM_WASM_CC` if the compiler is not
on `PATH`:

```
./examples/zeus/counter/run.sh
```

That starts `backend/run.sh` (`http.listen` on `:8080`) then `frontend/run.sh`
(Vite on `:5173`, wasm rebuild, `/Counter` proxied to the backend). Run the two
scripts in separate terminals to start them apart.

```
./bin/loamc build --target=wasm32 examples/zeus/counter/frontend/app.loam
cd examples/zeus/counter/frontend && npm install && npm run dev
./examples/zeus/counter/backend/run.sh
```

`npm run dev` deletes `frontend/build/` then runs `loam --target=wasm32` before Vite
listens, and again when `.loam` / runtime sources change. The wasm page calls
`http.client("").call` (gRPC-Web) on the same origin; Vite forwards `/Counter` to `:8080`.

iOS Simulator (same RPC contracts as wasm; Zeus theme, not UIKit controls).
Needs the backend on `:8080` (the script starts it if missing):

```
./examples/zeus/counter/ios/run.sh
```

Or:

```
./examples/zeus/counter/backend/run.sh
./bin/loamc --target=ios --run examples/zeus/counter/ios/app.loam
```

Needs Xcode and an iPhone Simulator. Output is
`examples/zeus/counter/ios/build/app.app`.

Android emulator (same RPC contracts as wasm; Zeus theme, not Material
controls). Needs the backend on `:8080` (the script starts it if missing).
The emulator reaches the Mac loopback at `10.0.2.2`, not `127.0.0.1`.
Needs the Android SDK, NDK, Gradle 8.2+, and a running emulator or device.
Step-by-step: `examples/zeus/counter/android/guide.md`. On macOS with Homebrew:

```
./examples/zeus/counter/android/install.sh
./examples/zeus/counter/android/emu.sh
./examples/zeus/counter/android/run.sh
```

`install.sh` writes `.sdk-env` (gitignored); `run.sh` sources it. Or:

```
./examples/zeus/counter/backend/run.sh
./bin/loamc --target=android --run examples/zeus/counter/android/app.loam
```

Output is the Gradle tree `examples/zeus/counter/android/build/app`.
`--target=android` always writes that project. `--run` calls `gradle installDebug`
and `adb` when the SDK is present; otherwise it points at `install.sh`.

A physical device cannot use `10.0.2.2` — pass the Mac's LAN IP in
`api.android_addr()` (and the backend currently binds loopback only).

macOS Cocoa (same RPC contracts and `screen.loam`; Zeus theme, not AppKit
controls). Needs the backend on `:8080` (the script starts it if missing):

```
./examples/zeus/counter/macos/run.sh
```

Or:

```
./examples/zeus/counter/backend/run.sh
./bin/loamc --target=native --run examples/zeus/counter/macos/app.loam
```

Output is `examples/zeus/counter/macos/build/app`.

## Packages, inspector, animation

`import "pkg:name"` resolves to `vendor/name/name.loam` (or `main.loam`),
searched upward from the entry file. `zeli pkg sync <appdir>` reads
`<appdir>/loam.deps` — one `name source [rev]` per line, `#` comments —
materializes each package under `<appdir>/vendor/<name>/`, and writes
`<appdir>/loam.lock`. A `path:../dir` source is copied locally (no network);
anything else is a git URL, cloned and pinned to a SHA.

For the devtools, two core dumps read the arena directly:
`zeus.engine_tree_dump()` (one `id,parent,x,y,w,h,role,label` line per visible
node — the tree with its layout boxes) and `zeus.engine_signals_dump()` (one
`id,value` line per signal slot). `std:devtools` is the inspector itself: it
parses both into display lines (`tree_lines`, `signal_lines`) and `Panel()`
renders them as a scrollable list you drop into a dev build. It is a Zeus app
reading the same arena everything else uses, so it needs no second renderer.

Animation is a paint-only track pool, not a signal tween. `zeus.animate(sig, to,
ms)` moves the signal's *state* to `to` once (every effect bound to it re-runs
once), then interpolates a paint-only overlay over `ms` on a fixed-capacity
arena of tracks. The tick writes the overlay value into paint state and marks
that node's PAINT bit: it never calls a signal setter, so no effect re-runs and
no component rebuilds while an animation plays. `engine_step(dt)` advances every
track by the host-supplied delta (read once per frame, capped at 64 ms), so 60 Hz
and 120 Hz hosts move at the same speed; `engine_step` reports the frame as live
until the tracks settle, then the host idles.

Tracks are keyed by `(node, prop)` and retarget in place: a second call keeps
the current value as the new `from`, so an interrupted transition never pops or
restarts. Before the first frame completes, a scheduled track jumps to its final
value — nothing animates at boot, which also keeps the DRAW goldens
deterministic. `zeus.set_reduced_motion(true)` (and each host's OS preference)
skips to the final value as well. The chrome animations — hover, press, the
switch / checkbox thumb, mounted-surface fade, scrollbar fade — are the same
track pool; they are retargeted from discrete state by the shared interaction
overlay, not scanned per frame.

`zeus.proof_components()`, `proof_effects()`, `proof_signal_writes()`,
`proof_allocs()`, and `proof_anim_live()` are the Phase 3 harness: across an
animation the first four must stay flat.

## Interaction state

One shared overlay (`UISTATE`) carries hover, pressed, focus-visible, disabled,
loading, selected, and invalid. Components set it (`enabled`, `visible`,
`disabled()`, `loading()`, `invalid()`); paint and hit-test read it through
`interaction_state`, so no recipe re-derives it. The states are discrete, so a
change may use a signal; the *transition* between them is a track.

## Paint-only motion

Scale, translate, rotate, opacity, and corner radius are animatable on the same
track pool: `animate_scale` / `animate_x` / `animate_y` / `animate_rotate` /
`animate_opacity` / `animate_radius` (`node`, target, ms). They write paint
state only — `paint_node` wraps the node in a `save` / `xform` / `restore` — so a
transform never runs the layout pass. `motion_scale` / `motion_rot` / … read the
current value for tests. Continuous indicators are clock-driven, not tracks: a
`Spinner` paints its rotation from the frame clock (deterministic on the first
frame, settled under reduced motion), so nothing is scheduled or allocated per
frame.

**Feedback components:** `Spinner(size, color, period, label)` is an
indeterminate `progressbar`; `Empty(icon, title, body)` is a centred empty state
whose trailing block is its action.

## Scaffold

`zeli new <name> [dir]` writes the §6.2 app tree: `zeus.toml`, a single
`app.loam` entry (one entry for every host), `routes/` (root
layout/page/loading/error/not-found, a nested `blog/` with `loader.loam`, a
`[slug]` route with `paths()` + `meta`, and a `(marketing)` group), plus
`components/`, `server/` (`#[proto]` contracts + a `#[server]` fn), `theme.loam`,
`assets/`, `public/`, and `tests/`. It refuses a non-empty target. The output is
checked in at `examples/zeus/myapp/` and built by the test gate, so the tree
`zeli build` accepts is the tree it scaffolds.

## Formatter

`loam-fmt` is one style with no options:

- **4 spaces per block (`{`)**, never per call paren. A trailing block passed as
  an argument indents one level from its statement:

      zeus.App("x", fn() {
          body
      })

- **A continuation line aligns under the token after its opener** when the
  opener's line has content after it (`f(a,\n  b)`), and indents one level from
  the opener's line when the opener ends its line (`f(` alone). A closer on its
  own line returns to the opener's level.
- Single spaces between tokens; none before `,` `;` `)` `]` or after `(` `[`;
  one space around `=` and `->`; one space before `{`.
- A one-line `{ ... }` keeps the space its author wrote after the brace on both
  sides: `{ x, y }`, never `{ x, y}`.
- At most one blank line; one trailing newline.
- Comments (`//`, `///`, `//!`, and block comments including every continuation
  line) and string literals are copied verbatim — it scans rather than using the
  parser, because the parser discards comments.

`loam-fmt <file>` prints to stdout, `loam-fmt -w <file>` rewrites in place,
`loam-fmt` reads stdin.

`zeli fmt [dir]` runs that over every `.loam` under a tree (the current
directory by default, skipping output and cache directories) and writes the
result back in place: one command, no flags. Files the build generates are left
alone, since formatting one would only be undone by the next build.

## Serve

`zeli serve <appdir> [--target T] [--port N]` builds one target and watches the
app plus the framework `std/` trees for `.loam` changes, so an edit is reflected
without a manual rebuild. `T` is `web` (the default), `macos`, `server`, `ios`, or
`android`.

`web` builds the wasm target, serves `build/web` on localhost, and pushes a frame
per rebuild down `/.zeli-live` over Server-Sent Events, so an idle page holds one
connection and does no work. The watch is filesystem-driven (the kernel watcher,
not a timer), so an edit is noticed as it lands, and a *new* file — a route, say —
is noticed at all. On a new revision the page saves the signal arena
(`zeus_state_snapshot` → `sessionStorage`) and reloads, and the loader restores
it with `zeus_state_load` on boot — a component edit reflects without resetting
state. Signal ids are positional, so state carries over as long as the signal set
lines up. `<appdir>/app_routes.loam` is regenerated before every build, so a new
file under `routes/` needs nothing run by hand. A failed compiler check is not a
reload: the previous build keeps being served. `--build-only` does the build and
exits (the test gate uses it).

`macos` and `server` build the native binary, start it with its output appended to
`build/<target>/<stem>.log`, and start it again on every rebuild: the launcher
keeps the pid the shell reports, stops that process first (`SIGTERM`, then
`SIGKILL` if it holds on), and only then starts the replacement. A build that
fails leaves the process that is up alone, and an app that exits by itself is
reported once rather than restarted on a timer. `ios` and `android` build, install
and launch through the compiler's own `--run` path — that packaging is host code,
and a second copy of it here would be a second thing to keep in step — with the
previous instance stopped first (`simctl terminate` for the bundle id in the
`.app`, `adb shell am force-stop` for the id in the Gradle tree), because both
platforms otherwise leave the old build on the screen. A missing SDK reports
itself, in the toolchain's own words, and the last install stays on the device.

## Layout

```
packages/zeus/std/zeus.loam         public API + design system
packages/zeus/std/zeuscore/         layout, paint, hit-test (shared)
examples/zeus/                 same apps on every host
packages/zeus/hosts/desktop/mac.m        Cocoa present
packages/zeus/hosts/ios/ios.m            iOS Simulator present (paint only)
packages/zeus/hosts/android/android.c    JNI + Android Canvas present (paint only)
packages/zeus/hosts/web/loader.js        Canvas2D host
packages/zeus/hosts/web/wasm.c           WASM entry / JS imports
runtime/zeus_plat.c        shared C seam
runtime/zeus_key.c         keyboard
runtime/net.c              TCP trampolines (`packages/http/std/http` is Loam)
examples/zeus/counter/frontend   wasm UI (`http.client("").call`)
examples/zeus/counter/macos      Cocoa UI (`http.client(api.native_addr())`)
examples/zeus/counter/ios        Simulator UI (`http.client(api.native_addr())`)
examples/zeus/counter/android    emulator UI (`http.client(api.android_addr())`)
examples/zeus/counter/screen.loam  shared page (signals + components)
examples/zeus/counter/backend    shared `api.loam` + native `server.loam`
```
