# Yuga

Yuga is a memory-safe system language: Odin-like syntax, Rust-like ownership.
The compiler (`yugac`) is C11. It typechecks a program, lowers it to IR, emits
C99, and invokes `cc`. C is the platform binding target, not the language's
semantics.

```yuga
import "std:fmt"

fn main() {
    fmt.println("hello, yuga")
}
```

```
make
./bin/yugac hello.yuga -o hello
./hello
```

Language: [docs/yuga.md](docs/yuga.md). C vs Yuga: [docs/boundary.md](docs/boundary.md).
Zeus backends and specifications: [packages/zeus/docs/spec.md](packages/zeus/docs/spec.md).
Browsable docs: `./run.sh www` — Zeus UI at http://127.0.0.1:5175, `Docs.Page` on `:8082`.

## Requirements

New to the repository? macOS: `./install.sh` (core) or `./install.sh android` (adds the
Android stack) installs everything below that Homebrew can. See [Setup](#setup).

- A C11 compiler (`cc`) and `make` (Apple Command Line Tools)
- macOS for native desktop GUI (Cocoa) and Maya present
- [Xcode](https://developer.apple.com/xcode/) for the iOS Simulator target
- A `wasm32` clang (Homebrew LLVM, not Apple `/usr/bin/clang`) for web builds;
  `./install.sh` puts it where `yugac` looks by default. Set `YUGA_WASM_CC`
  only if your LLVM lives somewhere else
- Node.js for the Vite dev servers behind the wasm examples (`gallery web`, `www`)
- Android SDK, NDK, Gradle 8.2+, a JDK, and `adb` for `--target=android`
  (installer: `./install.sh android` or `examples/zeus/counter/android/install.sh`)

The compiler itself is C11 + libc. CLI programs link with the host `cc`. GUI
and 3D hosts are listed under [Platforms](#platforms).

## Setup

One-time, on macOS with Homebrew (the script can install Homebrew too):

```
./install.sh          # core: Command Line Tools check, LLVM (wasm32), Node
./install.sh android  # + Android SDK/NDK/Gradle + emulator AVD (several GB)
```

Both are idempotent. The Android step also links the shared `.sdk-env` into
every zeus example that has an `android/` host. Full Xcode (App Store) is a
manual install if you want `./run.sh … ios`.

## Build

From the repo root:

```
make
```

That produces `bin/yugac` and `bin/yuga-lsp`. Then:

```
./bin/yugac app.yuga -o app          # native binary
./bin/yugac app.yuga --run           # compile and run
./bin/yugac app.yuga --emit-c -o a.c # C99
./bin/yugac app.yuga --emit-ir -o a.ir
./bin/yugac --target wasm app.yuga -o app.wasm
./bin/yugac --target=ios --run examples/zeus/dashboard/dashboard.yuga
./bin/yugac --target=android examples/zeus/counter/android/app.yuga
```

`make test` compiles and runs the language tests, golden programs, and
examples (GUI and Maya in headless mode). Set `YUGA_TIME=1` to print
check / codegen / cc timings.

## Libraries

Quoted imports only. `import "std:foo"` loads `packages/compiler/std/foo.yuga`.
Call imported items as `foo.bar(...)`.

| Import | What it is |
|---|---|
| `std:fmt` | Stdout. `fmt.println` is compile-time lowering to length-based writes, not `printf`. |
| `std:zeus` | UI toolkit + design system in one module: one `Node` tree, signals, `Box` / `Text` / `Button` / `App`, themed chrome (`Card`, `Button` with `LOOK` / `SIZE`, `Dialog`, `Tabs`, `Navbar`, charts, `DatePicker`). Same source on Cocoa, iOS, Android, and wasm Canvas2D (no HTML DOM). |
| `std:http` | Unary RPC over gRPC-Web (HTTP/1.1) and h2c. `#[proto]` structs, no REST routes. |
| `std:maya` | Tiny 3D/2D engine. Scene and tracer in Yuga; C is the event loop and present. |
| `std:thread` | Detached OS threads for CPU-bound work, plus Send-disciplined `channel<T>` between workers and the UI loop. `spawn` callbacks must be Send (plain data) and are checked to never touch module state or the C seam. Native/iOS/Android; wasm `spawn` is a no-op. |
| `std:net` | TCP connect / listen / read / write. Used by `http`; not an app-level import. |
| `std:sys` | `env_set` / `exit`. Language-level seam into `yuga_rt`. |

Document items with `///` (and `//!` at the file top). Hover in the editor
shows those comments plus the type.

### Zeus

Zeus is Yuga's UI library. A component is a function; hierarchy is a
trailing block. No `View` trait, no HTML DOM. Backend is a `--target`
flag, not an import. Web is Canvas2D wasm.

```yuga
import "std:zeus"

fn main() {
    let n = zeus.signal(0)
    zeus.App("Count", fn() {
        zeus.Box(align_direction = DIRECTION.Column, padding = 16, spacing = 8) {
            zeus.Text("Count", font = 22)
            zeus.Text("{{n.get()}}", font = 28)
            zeus.Box(align_direction = DIRECTION.Row, spacing = 8) {
                zeus.Button("-", on_click = fn() => n.set(n.get() - 1))
                zeus.Button("+", on_click = fn() => n.set(n.get() + 1))
            }
        }
    })
}
```

The themed look (zinc palette, `Card`, `Button` with `LOOK` / `SIZE`, dialogs,
charts, `DatePicker`) ships inside [`std:zeus`](packages/compiler/std/zeus.yuga).
The catalog is [`examples/zeus/gallery`](examples/zeus/gallery). Zeus paints
its own theme on every host; Cocoa / UIKit / Android widgets are not used.
Map: [packages/zeus/README.md](packages/zeus/README.md). Architecture:
[packages/zeus/docs/spec.md](packages/zeus/docs/spec.md).

The component catalog running in the browser (same source as the Cocoa,
iOS, and Android hosts):

<video src="docs/media/gallery-demo.mp4" poster="docs/media/gallery-demo.jpg" controls preload="metadata" width="100%"></video>

_Open the recording in a new tab: [docs/media/gallery-demo.mp4](docs/media/gallery-demo.mp4)._

## Platforms

| Target | Flag | Host | Notes |
|---|---|---|---|
| Native desktop | `--target=native` (default) | macOS Cocoa | Zeus paints; AppKit is the window, not the widgets. |
| Web | `--target=wasm` / `wasm32` | Canvas2D | Same Zeus tree as native. No HTML/DOM widgets. Needs a `wasm32` clang. |
| iOS | `--target=ios` | UIKit Simulator | Window and touch only. Needs Xcode. Device signing is out of scope. |
| Android | `--target=android` | JNI Canvas | Writes a Gradle project. Layout is density-independent pixels. |

CLI and `std:http` servers are ordinary native binaries. Maya present is
Cocoa 2D on macOS (`MAYA_HEADLESS=1` updates once and exits).

The Android emulator reaches a Mac backend at `10.0.2.2:8080`, not
`127.0.0.1`. The Simulator and Cocoa apps share the Mac loopback.

## Examples

`./run.sh` with no arguments lists everything. It builds `yugac` if needed.
GUI examples open a window and servers block until Ctrl-C. For one frame then
exit (what `make test` does), set `ZEUS_HEADLESS=1` or `MAYA_HEADLESS=1`.

```
./run.sh                      # list
./run.sh language/counter     # language demo
./run.sh solar                # Maya 3D (macOS window)
./run.sh dashboard            # Zeus app, Cocoa
./run.sh dashboard wasm32     # same app, browser
./run.sh dashboard ios        # same app, Simulator
./run.sh counter              # full-stack: API :8080 + web UI :5173
./run.sh counter macos        # same UI as a Cocoa client
./run.sh www                  # docs: Zeus wasm :5175 + Docs.Page :8082
```

`counter` is both a language example and the full-stack Zeus app. A bare
`./run.sh counter` (and `./run.sh counter web`) is the Zeus stack; the
language demo is `./run.sh language/counter`. `zeus/counter` is an alias.

### Language (`examples/language/`)

| Name | What it does |
|---|---|
| `minimal` | Smallest `fn main()`. |
| `counter` | A struct and a `let mut` binding. |
| `http_server` | `std:http` unary RPC on `:8080`. |
| `solar` | Maya solar-system scene (orbit camera). |
| `studio` | Maya 3D toy with orbiting bodies. |
| `oob` | Out-of-bounds index; expected to trap. |

```
./run.sh http_server          # then Ctrl-C to stop
./run.sh oob                  # compile, run, confirm the trap
```

Equivalent without `run.sh`:

```
./bin/yugac --run examples/language/http_server.yuga
```

Golden programs under `packages/compiler/tests/golden/` (hello, fib, fizzbuzz,
…) are compiled by `make test`. They are fixtures, not demos.

### Zeus (`examples/zeus/`)

| App | What it is |
|---|---|
| `gallery` | Every zeus component in isolation. Start here to see the component library. |
| `dashboard` | A small dashboard: stats, activity, dialog, signals. |
| `counter` | Full-stack: shared `#[proto]` contract, Yuga backend, Zeus UI on web / macOS / iOS / Android. |
| `greeninfer` | Local vector memory engine: mmap + NEON row sweep in a Zeus harness. [README](examples/zeus/greeninfer/README.md) |

```
./run.sh gallery              # Vite wasm UI at http://127.0.0.1:5174
./run.sh greeninfer           # Cocoa engine check (mmap + NEON)
```
./run.sh gallery web          # same
./run.sh gallery macos        # Cocoa
./run.sh gallery ios          # Simulator (Xcode)
./run.sh gallery android      # emulator (see below)
./run.sh dashboard ios
```

The gallery is pure UI (no backend). Its per-host launchers, Android setup,
and SDK notes live in `examples/zeus/gallery/{macos,ios,android,frontend}`.

Full-stack counter (backend on `:8080`, then a client):

```
./run.sh counter              # web — UI http://127.0.0.1:5173
./run.sh counter macos        # Cocoa
./run.sh counter ios          # Simulator (Xcode)
./run.sh counter android      # emulator (see below)
./run.sh counter backend      # API only
```

Android, from the repo root, on macOS with Homebrew (either installs the
SDK stack; the counter one is canonical and `./install.sh android` wraps it):

```
./install.sh android                # once; several GB, SDK licenses
./run.sh counter android            # boots the emulator if none is connected
./run.sh gallery android            # or the gallery on the emulator
```

`run.sh` auto-boots the shared `yuga` AVD when no device is connected (stop
it later with `adb emu kill`); `emu.sh` runs it in its own terminal if you
prefer. Step-by-step: [examples/zeus/counter/android/guide.md](examples/zeus/counter/android/guide.md)
and [examples/zeus/gallery/android/guide.md](examples/zeus/gallery/android/guide.md).
All zeus `android/run.sh` scripts share one `yuga` AVD.

## Repository

```
packages/compiler/     yugac, yuga-lsp, std/, runtime/, tests
packages/zeus/         Zeus UI hosts: desktop/ Cocoa, ios/, android/, web/ (Canvas2D)
packages/zeus/docs/    spec.md = zeus backends and paint model
packages/tree-sitter-yuga/
packages/editors/      Zed extension, VSCode extension
install.sh             one-time macOS setup (core tools; android stack)
examples/language/     standalone .yuga programs
examples/zeus/         gallery (component catalog), dashboard, full-stack counter
www/                   Zeus + gRPC docs (Vite serves wasm, no Svelte)
docs/                  yuga.md (language + architecture), boundary.md (C seam),
                       downsides.md (self-improvement phases),
                       zeus_roadmap.md (phase history)
bin/yugac              compiler
bin/yuga-lsp           diagnostics, hover, go-to-def, completion, semantic tokens
```

Cursor / VS Code: [packages/editors/vscode/README.md](packages/editors/vscode/README.md) (`make && make install-editor`).
Zed: [packages/editors/zed/README.md](packages/editors/zed/README.md).
