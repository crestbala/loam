# `std:raygui` — immediate-mode GUI on raylib

[raygui](https://github.com/raysan5/raygui) is a single-header immediate-mode
GUI built on [raylib](https://www.raylib.com). This package exposes it to Loam.

```yuga
import "std:raygui"

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
| API + frame loop | `std/raygui.loam` | Bodyless `plat_*` hooks plus the public wrappers and the `init` / `open` / `begin` / `end` / `close` loop. |
| Host seam | `packages/loam/runtime/raygui_plat.c` | Includes `RAYGUI_IMPLEMENTATION`, owns the raylib window, and converts each call (NUL-terminate a `loam_str`, mirror raygui's `bool *` state as `int *`). No widget logic. |
| Declarations | `packages/loam/runtime/raygui_rt.h` | The `loam_raygui_plat_*` prototypes the generated C calls. |
| Vendored header | `vendor/raygui.h` | raygui 4.0 (matches raylib 5.x). |
| Link | `packages/loam/src/driver.c` | `import "std:raygui"` links the seam and raylib (`RAYLIB_PREFIX`, else `pkg-config raylib`, else Homebrew). |

Immediate mode keeps **no** retained state in the library: the app holds the
values (`let mut`) and re-issues every control each frame, which is why the
example doubles as a RAM test — a flat footprint means nothing is allocating
per frame.

## Headless

`RAYGUI_HEADLESS=1` (or `ZEUS_HEADLESS` / `LOAM_HEADLESS`, which `make test`
sets) opens no window, makes the frame calls inert, and ends the loop after
`RAYGUI_FRAMES` frames (default 3).
