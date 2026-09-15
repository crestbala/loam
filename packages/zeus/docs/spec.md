# Zeus

Full-stack UI for Yuga. Theme, layout, and the draw list live in Yuga.
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
| View type | `zeus.Node`. Yuga has no traits / `impl View`. |
| Imports | `import "std:zeus"`. No glob prelude. |
| Events | `on_click = handler` on `Box` / `Button`. Intern copies the closure env (`yuga_fn.env_size`). Captures stay Copy-only. `bind` on `Input` writes a `Signal<string>`. |
| HTTP types | Shared `.loam` module with `#[proto]` structs + `*_rpc()` name helpers. `http.client()` fills the addr per `--target` (wasm same-origin, mac/iOS loopback, Android emulator `10.0.2.2:8080`). |
| Wire protocol | gRPC-first: first-party traffic is always gRPC-Web / h2c. REST + JSON (`std:json`, `http.rest_*`, `app.json`) exist only as third-party interop, reachable from `#[server]` code. |
| Routing | `std:router`: a `[]Route` table in a retained shell; loaders registered with `on_load` start before paint. |
| Data | `std:res` resource signals (`Loading`/`Ready`/`Error`); `std:cache` over `std:kv` for `revalidate` + `invalidate`. |
| SSR | Out of scope. First paint is client WASM. |

## Routing and data

`import "std:router"` mounts a route table into a retained shell; only the
page slot rebuilds on navigation, so shell state survives. The current path is
a `Signal<string>`, and each page builds under `zeus.Boundary`.

```yuga
router.Router([]Route {
    Route { pattern: "/", build: Home },
    Route { pattern: "/post/:slug", build: Post },
}, RouterProps {})
```

Route loaders are registered separately and start the moment navigation
begins, before the page paints — every loader for the target route starts, so
independent fetches run concurrently (no waterfalls, §5.5):

```yuga
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

First-party traffic (Zeus client ↔ your Yuga server) is **always gRPC**:
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

`zeus build --targets web` emits **one HTML file per route** (never a single
`index.html`), plus `sitemap.xml` and `robots.txt`. It does this by generating a
small Yuga program that calls each route's `meta()` — and `paths()`, for a
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
built by `zeus build` opens on the right route.

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
ZEUS_HEADLESS=1 ./bin/yugac packages/yuga/tests/compile_pass/zeus_snap.loam -o packages/yuga/tests/tmp/zeus_snap
```

WASM (needs a clang that has `wasm32`, e.g. Homebrew `llvm` or wasi-sdk).
Apple `/usr/bin/clang` does **not**. Set `YUGA_WASM_CC` if the compiler is not
on `PATH`:

```
./examples/zeus/counter/run.sh
```

That starts `backend/run.sh` (`http.listen` on `:8080`) then `frontend/run.sh`
(Vite on `:5173`, wasm rebuild, `/Counter` proxied to the backend). Run the two
scripts in separate terminals to start them apart.

```
./bin/yugac build --target=wasm32 examples/zeus/counter/frontend/app.loam
cd examples/zeus/counter/frontend && npm install && npm run dev
./examples/zeus/counter/backend/run.sh
```

`npm run dev` deletes `frontend/build/` then runs `yugac --target=wasm32` before Vite
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
./bin/yugac --target=ios --run examples/zeus/counter/ios/app.loam
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
./bin/yugac --target=android --run examples/zeus/counter/android/app.loam
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
./bin/yugac --target=native --run examples/zeus/counter/macos/app.loam
```

Output is `examples/zeus/counter/macos/build/app`.

## Packages, inspector, animation

`import "pkg:name"` resolves to `vendor/name/name.loam` (or `main.loam`),
searched upward from the entry file. `zeus pkg sync <appdir>` reads
`<appdir>/yuga.deps` — one `name source [rev]` per line, `#` comments —
materializes each package under `<appdir>/vendor/<name>/`, and writes
`<appdir>/yuga.lock`. A `path:../dir` source is copied locally (no network);
anything else is a git URL, cloned and pinned to a SHA.

For the devtools, two core dumps read the arena directly:
`zeus.engine_tree_dump()` (one `id,parent,x,y,w,h,role,label` line per visible
node — the tree with its layout boxes) and `zeus.engine_signals_dump()` (one
`id,value` line per signal slot). `std:devtools` is the inspector itself: it
parses both into display lines (`tree_lines`, `signal_lines`) and `Panel()`
renders them as a scrollable list you drop into a dev build. It is a Zeus app
reading the same arena everything else uses, so it needs no second renderer.

Animation is a signal tween: `zeus.animate(sig, to, ms)` steps the signal
toward `to` one frame at a time. `engine_step` reports the frame as live until
it settles, so the host keeps drawing, and everything bound to the signal
repaints through the normal signal path. `ms == 0` jumps; a second call
retargets mid-flight.

## Scaffold

`zeus new <name> [dir]` writes the §6.2 app tree: `zeus.toml`, a single
`app.loam` entry (one entry for every host), `routes/` (root
layout/page/loading/error/not-found, a nested `blog/` with `loader.loam`, a
`[slug]` route with `paths()` + `meta`, and a `(marketing)` group), plus
`components/`, `server/` (`#[proto]` contracts + a `#[server]` fn), `theme.loam`,
`assets/`, `public/`, and `tests/`. It refuses a non-empty target. The output is
checked in at `examples/zeus/myapp/` and built by the test gate, so the tree
`zeus build` accepts is the tree it scaffolds.

## Formatter

`yugafmt` is one style with no options: 4-space indentation by bracket depth,
single spaces between tokens, no space before `,` `;` `)` `]` `:` or after
`(` `[`, one space around `=` and `->`, one space before `{`, at most one blank
line, one trailing newline. `yugafmt <file>` prints to stdout, `yugafmt -w
<file>` rewrites in place, `yugafmt` reads stdin. Comments (`//`, `///`, `//!`,
block) and string literals are copied verbatim — it scans rather than using the
parser, because the parser discards comments.

## Dev server

`zeus dev <appdir> [--port N]` builds the web target, serves `build/web` on
localhost, and watches the app plus the framework `std/` trees for `.loam`
changes, rebuilding on each. Served HTML gets a live-reload script: on a new
revision it saves the signal arena (`zeus_state_snapshot` → `sessionStorage`)
and reloads, and the loader restores it with `zeus_state_load` on boot — a
component edit reflects without resetting state. Signal ids are positional, so
state carries over as long as the signal set lines up. `--build-only` does the
build and exits (the test gate uses it).

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
runtime/net.c              TCP trampolines (`packages/http/std/http` is Yuga)
examples/zeus/counter/frontend   wasm UI (`http.client("").call`)
examples/zeus/counter/macos      Cocoa UI (`http.client(api.native_addr())`)
examples/zeus/counter/ios        Simulator UI (`http.client(api.native_addr())`)
examples/zeus/counter/android    emulator UI (`http.client(api.android_addr())`)
examples/zeus/counter/screen.loam  shared page (signals + components)
examples/zeus/counter/backend    shared `api.loam` + native `server.loam`
```
