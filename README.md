# Loam

Loam is a memory-safe system language: Odin-like syntax, Rust-like ownership.
The compiler (`loam`) is C11. It typechecks a program, lowers it to IR, emits
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
./bin/loam hello.loam -o hello
./hello
```

Language: [docs/yuga.md](docs/yuga.md). C vs Loam: [docs/boundary.md](docs/boundary.md).
Zeus backends and specifications: [packages/zeus/docs/spec.md](packages/zeus/docs/spec.md).
Browsable docs: `./run.sh www` — Zeus UI at http://127.0.0.1:5175, `Docs.Page` on `:8082`.

## Requirements

New to the repository? macOS: `./install.sh` (core) or `./install.sh android` (adds the
Android stack) installs everything below that Homebrew can. See [Setup](#setup).

- A C11 compiler (`cc`) and `make` (Apple Command Line Tools)
- macOS for native desktop GUI (Cocoa) and Maya present
- [raylib](https://www.raylib.com) (`brew install raylib`) for the `std:raygui`
  example (`examples/language/raygui.loam`); nothing else needs it
- [Xcode](https://developer.apple.com/xcode/) for the iOS Simulator target
- A `wasm32` clang (Homebrew LLVM, not Apple `/usr/bin/clang`) for web builds;
  `./install.sh` puts it where `loam` looks by default. Set `LOAM_WASM_CC`
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

That produces `bin/loam` and `bin/loam-lsp`. Then:

```
./bin/loam app.loam -o app          # native binary
./bin/loam app.loam --run           # compile and run
./bin/loam app.loam --emit-c -o a.c # C99
./bin/loam app.loam --emit-ir -o a.ir
./bin/loam --target wasm app.loam -o app.wasm
./bin/loam --target=ios --run examples/zeus/dashboard/dashboard.loam
./bin/loam --target=android examples/zeus/counter/android/app.loam
```

`make test` compiles and runs the language tests, golden programs, and
examples (GUI, Maya, and raygui in headless mode; the raygui example is
skipped when `pkg-config raylib` is absent). Set `LOAM_TIME=1` to print
check / codegen / cc timings.

## Libraries

Quoted imports only. `import "std:foo"` loads `packages/loam/std/foo.loam`.
Call imported items as `foo.bar(...)`.

| Import | What it is |
|---|---|
| `std:fmt` | Stdout. `fmt.println` is compile-time lowering to length-based writes, not `printf`. |
| `std:zeus` | UI toolkit + design system in one module: one `Node` tree, signals, `Box` / `Text` / `Button` / `App`, themed chrome (`Card`, `Button` with `LOOK` / `SIZE`, `Dialog`, `Tabs`, `Navbar`, charts, `DatePicker`). Same source on Cocoa, iOS, Android, and wasm Canvas2D (no HTML DOM). |
| `std:http` | Unary RPC over gRPC-Web (HTTP/1.1) and h2c. `#[proto]` structs, no REST routes. |
| `std:maya` | Tiny 3D/2D engine. Scene and tracer in Loam; C is the event loop and present. |
| `std:raygui` | Immediate-mode GUI: [raygui](https://github.com/raysan5/raygui) controls on [raylib](https://www.raylib.com). No retained tree; the frame loop lives in Loam. Host seam: `raygui_plat.c`; RAM probe included. |
| `std:thread` | Detached OS threads for CPU-bound work, plus Send-disciplined `channel<T>` between workers and the UI loop. `spawn` callbacks must be Send (plain data) and are checked to never touch module state or the C seam. Native/iOS/Android; wasm `spawn` is a no-op. |
| `std:net` | TCP connect / listen / read / write. Used by `http`; not an app-level import. |
| `std:sys` | `env_set` / `exit`. Language-level seam into `loam_rt`. |

Document items with `///` (and `//!` at the file top). Hover in the editor
shows those comments plus the type.

### Zeus

Zeus is Loam's UI library. A component is a function; hierarchy is a
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
charts, `DatePicker`) ships inside [`std:zeus`](packages/zeus/std/zeus.loam).
The catalog is [`examples/zeus/gallery`](examples/zeus/gallery). Zeus paints
its own theme on every host; Cocoa / UIKit / Android widgets are not used.
Map: [zeus/README.md](zeus/README.md). Architecture:
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
Cocoa 2D on macOS (`MAYA_HEADLESS=1` updates once and exits). raygui opens a
raylib window (`RAYGUI_HEADLESS=1` draws a few frames and exits).

The Android emulator reaches a Mac backend at `10.0.2.2:8080`, not
`127.0.0.1`. The Simulator and Cocoa apps share the Mac loopback.

## Examples

`./run.sh` with no arguments lists everything. It builds `loam` if needed.
GUI examples open a window and servers block until Ctrl-C. For one frame then
exit (what `make test` does), set `ZEUS_HEADLESS=1`, `MAYA_HEADLESS=1`, or
`RAYGUI_HEADLESS=1`.

```
./run.sh                      # list
./run.sh language/counter     # language demo
./run.sh solar                # Maya 3D (macOS window)
./run.sh raygui               # raygui immediate-mode controls + RAM (needs raylib)
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
| `raygui` | raygui immediate-mode controls (buttons, slider, combo, list) with a live process-RAM panel; prints a RAM summary on exit. Needs raylib. |
| `oob` | Out-of-bounds index; expected to trap. |
| `ppm/` | PPM (P6) frame generators encoded to mp4 by ffmpeg — `checker` and `plasma` ports of [rexim's gist](https://gist.github.com/rexim/ef86bf70918034a5a57881456c0a0ccf). [README](examples/language/ppm/README.md) |

```
./run.sh http_server          # then Ctrl-C to stop
./run.sh oob                  # compile, run, confirm the trap
./run.sh language/ppm/checker # raster frames -> out/checker.mp4
```

Equivalent without `run.sh`:

```
./bin/loam --run examples/language/http_server.loam
```

Golden programs under `packages/loam/tests/golden/` (hello, fib, fizzbuzz,
…) are compiled by `make test`. They are fixtures, not demos.

### Zeus (`examples/zeus/`)

| App | What it is |
|---|---|
| `gallery` | Every zeus component in isolation. Start here to see the component library. |
| `dashboard` | A small dashboard: stats, activity, dialog, signals. |
| `counter` | Full-stack: shared `#[proto]` contract, Loam backend, Zeus UI on web / macOS / iOS / Android. |
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
yuga/                  the language
  src/                 compiler (C11): lexer, parser, sema, ir, codegen_c
  std/                 language libraries (Loam), incl. zeuscore/httpcore/mayacore
  runtime/             loam_rt (language) + host shims
  tests/               compile_pass / compile_fail / golden / inlang / bench
zeus/                  the framework
  hosts/               desktop/ Cocoa, ios/ UIKit, android/ JNI, web/ Canvas2D
  docs/                spec.md = zeus backends and paint model
raygui/                immediate-mode GUI package (std:raygui)
  std/raygui.loam      the Loam API + frame loop
  vendor/raygui.h      vendored raygui (raysan5/raygui)
packages/tooling/tree-sitter-yuga/  grammar
packages/tooling/editors/       Zed extension, VSCode extension
install.sh             one-time macOS setup (core tools; android stack)
examples/language/     standalone .loam programs
examples/zeus/         gallery (component catalog), dashboard, full-stack counter
www/                   Zeus + gRPC docs (Vite serves wasm, no Svelte)
docs/                  yuga.md (language + architecture), boundary.md (C seam),
                       downsides.md (self-improvement phases),
                       zeus_roadmap.md (phase history)
bin/loam              compiler
bin/loam-lsp           diagnostics, hover, go-to-def, completion, semantic tokens
```

Cursor / VS Code: [packages/tooling/editors/vscode/README.md](packages/tooling/editors/vscode/README.md) (`make && make install-editor`).
Zed: [packages/tooling/editors/zed/README.md](packages/tooling/editors/zed/README.md).
