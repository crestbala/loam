# `raygui` — immediate-mode GUI on raylib

[raygui](https://github.com/raysan5/raygui) is a single-header immediate-mode
GUI built on [raylib](https://www.raylib.com). This package exposes it to Loam.

```loam
import "raygui"

let mut value: float = 0.5
let mut on: int = 1

fn main() {
    raygui.init("demo", 640, 400, 60)
    while raygui.open() != 0 {
        raygui.begin(raygui.rgb(244, 244, 245))
        raygui.label(16, 16, 200, 22, "hello")
        if raygui.button(16, 48, 120, 32, "press") != 0 { on = 1 - on }
        raygui.checkbox(16, 88, 160, 24, "on", &mut on)
        raygui.slider(16, 120, 300, 24, "", "", &mut value, 0.0, 1.0)
        raygui.end()
    }
    raygui.close()
}
```

Run the example: `./run.sh raygui` (needs `brew install raylib`).

## Layout

| Piece | Where | What |
|---|---|---|
| API + frame loop | `src/raygui.loam` | The public wrappers and the `init` / `open` / `begin` / `end` / `close` loop; window state, the headless rule, the `RAYGUI_FRAMES` cap and the RAM probe (`task_info` on macOS, `getrusage` on Linux). |
| Bindings | `src/raygui.loam` | `#[c]` fns straight to raylib and raygui: `Bounds` / `Rgba8` are `Rectangle` / `Color` passed by value, and the stateful controls share reused `[]bool` / `[]i32` / `[]f32` cells, so a frame allocates nothing. No C shim. |
| Vendored header | `src/vendor/raygui.h` | raygui 4.0 (matches raylib 5.x), compiled as itself by `#![c_source("vendor/raygui.h", "-x", "c", "-DRAYGUI_IMPLEMENTATION", "pkg-config:raylib")]`. |
| Link | `#![c_link("pkg-config:raylib")]` | raylib's flags from `RAYLIB_PREFIX`, else `pkg-config raylib`, else the Homebrew prefixes. |

The transcribed layouts (`Rectangle`, `Color`, `task_vm_info`, `rusage`) are
checked against the real headers by `nob integration c_layouts`.

Immediate mode keeps **no** retained state in the library: the app holds the
values (`let mut`) and re-issues every control each frame, which is why the
example doubles as a RAM test — a flat footprint means nothing is allocating
per frame.

## Headless

`RAYGUI_HEADLESS=1` (or `ZEUS_HEADLESS` / `LOAM_HEADLESS`, which `./nob.sh test`
sets) opens no window, makes the frame calls inert, and ends the loop after
`RAYGUI_FRAMES` frames (default 3).
