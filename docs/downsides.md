# Loam / Zeus self-improvement

Loam is **Odin-shaped syntax with Rust-shaped ownership**. It is not Rust.
A type, method, or syntactic form lands only when a program needs it.
Missing traits, lifetime syntax, algebraic enums, macros, `u32`, and
`for x in xs` are the point — not a backlog.

This file is the **next series of phases**: make the same Zeus app easier
to write and run on every host. The production stack (async, images, IME,
virtual lists, TLS, KV, DCE, focus, scale, threads) is in
[zeus_roadmap.md](zeus_roadmap.md). Do not re-plan those. Language
architecture: [yuga.md](yuga.md). C seam: [boundary.md](boundary.md).

Status: tick a phase only when its exit criteria run green in `make test`
(headless) unless a box says otherwise. One phase at a time.

---

## Language (accepted; not a phase)

Do not add these because another language has them. Prefer a std `fn`
over a new keyword.

| Keep | Do not add |
|---|---|
| `int` / `float` / `bool` / `string` / `[]T` / `[N]T` / `Box<T>` / `&T` / `&mut T` / `fn` | Width integers, `&str` vs `String`, trait objects |
| Structs, C-style `enum` as named ints, `match` as switch | Algebraic enums, language `Option` / `Result` |
| Quoted `import "std:foo"`, `mod.name` | Glob, `use`, `pub`, re-exports |
| Ownership + exclusive vs shared, auto-borrow | Lifetime parameters, NLL, `unsafe` |
| Monomorphized generics, UFCS | `impl`, traits, operator overloading |
| Bodyless `fn` as the C hook | `extern`, macros, comptime |
| `for i in lo..hi`, trailing constant defaults | Iterators, mid-list defaults, labeled break |

Costs that still bite and stay unless a later phase names them: C99 + `cc`
backend (debuggers see generated C); `yugac` is C11; no package registry;
borrowck is not NLL; closures capture Copy only; `+ - *` and indexes trap;
wasm `thread.spawn` is a no-op; no async TLS (Phase 8 here); HTTP server is
one thread; native GUI is macOS.

---

## Phases

Definition of done = the exit line, verified by `make test` unless noted.

### Phase 1 — One host entry

Four copies of `app.loam` (macos / ios / android / frontend) share a
`screen.loam` and differ by `http.client(addr)` and sometimes size. That is
the first thing a new app copies wrong.

- [x] `http.client()` with no addr (or `http.client("")` on every target)
      resolves per `--target`: wasm same-origin, mac/iOS `127.0.0.1:8080`,
      Android emulator `10.0.2.2:8080`. An explicit addr still wins.
      `sys.target()` is `"wasm"` / `"ios"` / `"android"` / `"native"`.
- [x] Physical Android device: `LOAM_RPC_ADDR=<lan>:8080`, not a fourth
      `app.loam`.
- [x] Counter, gallery, and www: shared `ui.loam` + a three-line host stub
      (`import "../ui.loam"`; `ui.start()`).
- **Exit:** `./run.sh counter` / `counter macos` / `counter ios` /
  `counter android` all build from `ui.start`; changing an RPC in
  `screen.loam` is the only UI edit. **Green** (`http_client.loam` pins
  native default `127.0.0.1:8080`; counter/gallery macos compile).

### Phase 2 — Honest `App` size

`zeus.App(title, w, h, build)` is ignored after the first layout on iOS and
Android (the host view wins). Gallery already calls `window_size()`;
counter android/ios still pass `640, 680`.

- [x] `App(title, build)` uses `window_size()`. `width` / `height` stay
      optional trailing defaults for macOS/wasm windows that need a first
      size before the view exists.
- [x] iOS/Android docs: size args do not pin the view (android `guide.md`).
- [x] Counter android/ios stop passing a fake 640×680 (shared `ui.start`).
- **Exit:** one `App` call shape in gallery + counter on all four hosts;
  headless goldens unchanged (`snapshot` / trailing `App` size). **Green.**

### Phase 3 — Host table + toolchain errors

What differs per host is folklore across READMEs.

- [x] One table in this file; `http.default_addr` / `sys.target` `///`.
- [x] `yugac --target=wasm` without a `wasm32` clang names `./install.sh`
      / `LOAM_WASM_CC`.
- [x] Missing iOS SDK / Android SDK still point at Xcode / `install.sh`.
- **Exit:** a new contributor can pick a host from the table; a missing
  wasm clang is a one-line fix. **Green.**

Host table (fill as Phase 3 lands; values today):

| | macOS | iOS Sim | Android emu | wasm |
|---|---|---|---|---|
| Window | Cocoa, `App` size is the window | UIKit view; `App` size ignored after first layout | JNI Canvas; same | Canvas2D; window = canvas |
| RPC addr | `127.0.0.1:8080` | loopback | `10.0.2.2:8080` | `""` (Vite proxy) |
| TLS | blocking `http.call` https | same sockets | same | browser `fetch` |
| `call_async` https | rejected | rejected | rejected | browser TLS |
| Threads | pthread | pthread | pthread | `spawn` no-op |
| KV | file | file | file | memory |
| IME | `NSTextInputClient` | host text | host text | paste + `compositionend` |
| Scroll | painted thumb | overlay, touch only | overlay, touch only | painted thumb |
| a11y | roles + focus ring | roles + focus ring | roles + focus ring | no ARIA |
| Hover | yes | no | no | yes |
| Device | — | signing out of scope | physical ≠ `10.0.2.2` | — |

### Phase 4 — Touch vs pointer

Hover wash, CSS `cursor`, and stub methods (`hover_delay`, `hover_leave`,
`dismiss` return the node unchanged) pretend every host has a mouse.

- [x] Deleted `hover_delay` / `hover_leave` / `dismiss`. No silent no-ops.
- [x] Hover wash and `cursor` skip on `plat_overlay_scroll` (iOS/Android).
- [x] Gallery / chrome DRAW goldens byte-identical.
- **Exit:** `compile_fail/zeus_hover_stub.loam` (`no method 'hover_delay'`).
  DRAW goldens byte-identical. **Green.**

### Phase 5 — Safe area and root scroll

`App` wraps a vertical scroller that already insets. Inner `VirtualList` /
`Scroll` fights that bar. Inner panes need `safe = 1` or they sit under
the notch.

- [x] Recipe documented on `App`: full-bleed `VirtualList` uses `grow = 1`.
      Nested momentum chaining stays Phase 8.
- [x] Root already insets (`inset_here(root)`). Overlays / `Dialog` layout
      to the safe-area box, not the raw window.
- [x] Counter jobs list + gallery still compile (headless).
- **Exit:** DRAW goldens hold; overlay is inset. Nested *chaining* is
  Phase 8. **Green.**

### Phase 6 — Dev loop

Gallery Vite `spawnSync` blocks the dev server on every `.loam` change.
Hidden gallery tabs still build and lay out (wasm first paint ~0.5 s after
the heap fix).

- [x] Gallery Vite watcher compiles wasm async (`spawn`); previous
      `app.wasm` stays served; `full-reload` when the new one is ready.
- [x] Won’t: lazy-build hidden gallery tabs — retained tree builds once;
      first paint is already ~0.5 s after the heap fix.
- **Exit:** saving `screen.loam` does not `spawnSync`-block the Vite
  thread. **Green.**

### Phase 7 — `zeus.loam` API tidy

Same module, overlapping ways to hide a node, a `Button` default that
contradicts its comment, arity-3 tables, a `Navbar` that always ships Dark.

- [x] Hide API: live hide is `If` / `zeus.show` on `Signal<int>` (0/1);
      `Box.show` is the bool-thunk form of the same flag (documented).
- [x] `Button`: default `LOOK.Solid` (gallery Primary); `look = -1` is
      plain (`zeus_btn_plain.loam`). Themed chrome honors `width`/`height`.
- [x] `TableHead` / `TableRow` / `Breadcrumbs` take `[]string`.
- [x] `Navbar(brand, dark = 1)`; pass `dark = 0` to omit the switch.
- [x] `App` is the retained entry; `app` / `view` rebuild every layout.
- **Exit:** gallery DRAW goldens byte-identical at scale 1.0; `make test`
  green. **Green.**

### Phase 8 — Engine leftovers

- [x] Nested scroll chaining: leftover wheel at an inner bound goes to the
      parent scroller (`zeus_scroll_chain.loam`).
- [x] wasm IME: hidden `<textarea>` in `packages/zeus/hosts/web/loader.js` takes
      composition / input when `zeus_captures_text`; window-level composition
      listeners are gone.
- [x] Async TLS: `net.tls_nb_connect` / `tls_nb_ready` (SecureTransport,
      non-blocking). `http.call_async("https://…")` is stage 5 handshake then
      the existing send/read pump (`http_https_async.loam`). Wasm still uses
      browser `fetch`.
- [x] Variable-height windowed lists: `VirtualListVar(items, heights,
      fallback, build)` (`zeus_virt_var.loam`).
- [x] Gradients: draw op `grad` / `plat_fill_g` / `node.bg2`. Cocoa
      `NSGradient`, Canvas2D `createLinearGradient`, headless dump.
      **Won’t:** rich text spans — still gated on in-tree font metrics.
- [x] Linux GUI host: `packages/zeus/hosts/desktop/linux.c` (X11 fill + text).
      Linked when `yugac --target=native` is not headless on Linux (`-lX11`).
      iOS device signing stays out of scope.
- **Exit:** headless tests green. **Green** (`make test`). Rich text remains
  its own project.

### Phase 9 — Component system (Skin + slots)

Widgets were recipes you had to fork. Props were a mix of positionals,
tokens, and raw Box fields. No one overlay restyled a control.

- [x] `Skin` is the chrome overlay: `skin = Skin(radius = 4, …)` on
      `Button` / `Card` / `Chip` / `Comp` / `IconButton` / `Badge` /
      `Alert` / `Field` / `TextInput`. Empty / `-1` keeps the recipe.
      Construct with `Skin(...)` (named fields), not `Skin { }`.
- [x] Last-param props structs: named args (`skin =`, `width =`,
      `on_click =`) fill the trailing struct. `Card()` / `Chip("ok", TINT.Ok)`
      still work (all-defaulted last param).
- [x] `Comp` is the function-widget primitive (trailing block = slot).
      `Button(...) { … }` extra children land in the same slot.
- [x] One `component(node, bind=/visible=/press=)` finish: parent attach,
      signal bind, signal hide, press overlay. `slot(host, build)` is the
      parent stack (`__ui_scope` is `slot`). `If(open)`, `Box(visible=)`,
      `Card(visible=)`, `Overlay(visible=)`, `Switch`/`Slider`/`Progress`
      `bind=` all go through it. Recipes do not call `__ui_push` / `bind` /
      `show` themselves.
- [x] Motion: hover eases (`d/5`); press overlay eases (`press_amt`);
      enter fade is ease-out. Rest `press_amt` / `hover_amt` are 0 so DRAW
      goldens stay pixel-identical at scale 100.
- **Exit:** `zeus_skin.loam` + existing component tests + DRAW goldens
  green in `make test`. Gallery Foundations shows a Customize row. **Green.**

---

## What will not change

- Not HTML/DOM, not UIKit/AppKit/Material, not WebGPU.
- Not a `View` trait. A component is a `fn`. `Node` is `{ id: int }`.
- Not REST/JSON. Wire is `#[proto]` + gRPC-Web / h2c.
- Not a multi-threaded UI. Completion returns to the one UI thread.
- Not a Rust-shaped type system. See Language above.
