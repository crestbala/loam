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
| HTTP types | Shared `.yuga` module with `#[proto]` structs + `*_rpc()` name helpers. `http.client()` fills the addr per `--target` (wasm same-origin, mac/iOS loopback, Android emulator `10.0.2.2:8080`). |
| SSR | Out of scope. First paint is client WASM. |

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
ZEUS_HEADLESS=1 ./bin/yugac packages/compiler/tests/compile_pass/zeus_snap.yuga -o packages/compiler/tests/tmp/zeus_snap
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
./bin/yugac build --target=wasm32 examples/zeus/counter/frontend/app.yuga
cd examples/zeus/counter/frontend && npm install && npm run dev
./examples/zeus/counter/backend/run.sh
```

`npm run dev` deletes `frontend/build/` then runs `yugac --target=wasm32` before Vite
listens, and again when `.yuga` / runtime sources change. The wasm page calls
`http.client("").call` (gRPC-Web) on the same origin; Vite forwards `/Counter` to `:8080`.

iOS Simulator (same RPC contracts as wasm; Zeus theme, not UIKit controls).
Needs the backend on `:8080` (the script starts it if missing):

```
./examples/zeus/counter/ios/run.sh
```

Or:

```
./examples/zeus/counter/backend/run.sh
./bin/yugac --target=ios --run examples/zeus/counter/ios/app.yuga
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
./bin/yugac --target=android --run examples/zeus/counter/android/app.yuga
```

Output is the Gradle tree `examples/zeus/counter/android/build/app`.
`--target=android` always writes that project. `--run` calls `gradle installDebug`
and `adb` when the SDK is present; otherwise it points at `install.sh`.

A physical device cannot use `10.0.2.2` — pass the Mac's LAN IP in
`api.android_addr()` (and the backend currently binds loopback only).

macOS Cocoa (same RPC contracts and `screen.yuga`; Zeus theme, not AppKit
controls). Needs the backend on `:8080` (the script starts it if missing):

```
./examples/zeus/counter/macos/run.sh
```

Or:

```
./examples/zeus/counter/backend/run.sh
./bin/yugac --target=native --run examples/zeus/counter/macos/app.yuga
```

Output is `examples/zeus/counter/macos/build/app`.

## Layout

```
std/zeus.yuga              public API + design system
std/zeuscore/              layout, paint, hit-test (shared)
examples/zeus/                 same apps on every host
packages/zeus/desktop/mac.m         Cocoa present
packages/zeus/ios/ios.m             iOS Simulator present (paint only)
packages/zeus/android/android.c     JNI + Android Canvas present (paint only)
packages/zeus/web/loader.js         Canvas2D host
packages/zeus/web/wasm.c            WASM entry / JS imports
runtime/zeus_plat.c        shared C seam
runtime/zeus_key.c         keyboard
runtime/net.c              TCP trampolines (`std/http` is Yuga)
examples/zeus/counter/frontend   wasm UI (`http.client("").call`)
examples/zeus/counter/macos      Cocoa UI (`http.client(api.native_addr())`)
examples/zeus/counter/ios        Simulator UI (`http.client(api.native_addr())`)
examples/zeus/counter/android    emulator UI (`http.client(api.android_addr())`)
examples/zeus/counter/screen.yuga  shared page (signals + components)
examples/zeus/counter/backend    shared `api.yuga` + native `server.yuga`
```
