# Writing a Zeus host

A host is the small piece of platform code that gives Zeus a window, input
and a way to draw. Everything else (layout, styling, animation, text
wrapping, SVG, scrolling, focus) runs in the engine and is the same on every
platform. The C API is `packages/loam/runtime/zeus_rt.h`.

## Required

These are what a host has to provide. With only these, every app runs.

| Need | How |
|---|---|
| Start-up | `zeus_set_platform(run, measure, redraw)` from a constructor: `run` owns the event loop, `measure` returns a string's width and line height at a pixel size, `redraw` asks for a frame |
| A frame | each tick: `zeus_layout(w, h)`, `zeus_step(dt)`, then paint |
| Paint | `zeus_paint(ctx, draw)` with a `ZeusDraw` table, or `zeus_bind_frame(sink)` to decode the frame buffer yourself (`docs/frame-format.md`) |
| Input | `zeus_handle_click / hover / drag / mouseup`, `zeus_handle_scroll*`, `zeus_handle_key / key_up / text / marked` |
| View size | `zeus_set_window_size(w, h)`, and `zeus_window_opened()` once a window exists |

`ZeusDraw` callbacks are optional one by one: `zeus_replay` falls back for any
NULL entry (`fill_a` → `fill`, `fill4` → one radius, `stroke` → four edge
fills, `text_rot` → `text`), and skips what has no fallback (shadows,
transforms, images, paths). A host can start with `fill`, `text`, `clip`,
`save` and `restore`, and add the rest later.

## Optional

Each of these is a hook with a working default. Leave it unset and the app
still runs, without that feature.

| Feature | Hook | Without it |
|---|---|---|
| Window title | `zeus_set_title_hook` | the title is fixed at start-up |
| Clipboard | `zeus_set_clipboard_hook` | copy is kept in-process only |
| Image intrinsic size | `zeus_set_image_size` | images need an explicit size |
| Image picker | `zeus_set_pick_image` | the picker does nothing |
| Media playback, pickers, voice tools, full screen | `zeus_set_media(ZeusMedia)` (any field may be NULL) | a null player: state changes, the clock stays at 0 |
| Fonts | `zeus_set_font_hooks`, `zeus_set_font_bytes_hook` | the system font; files are read from disk |
| Software-rasterized frames | `zeus_set_blit` | not used unless the app opts in to `std:zeusraster` |
| Safe area, overlay scrollbars | `zeus_set_insets`, `zeus_set_overlay_scroll` | no insets; classic scrollbars |
| Initial window size | `zeus_set_initial_view` | 640 × 480 until the window opens |

## The hosts in this repo

| Host | Files |
|---|---|
| macOS | `hosts/desktop/mac.m`: window, view, input and IME, display path, pickers, voice tools, hot reload |
| iOS | `hosts/ios/ios.m`: window, view, touches, keyboard, pickers |
| macOS + iOS shared | `hosts/apple/`: `apple_draw.m` (every paint op, text, fonts, images) and `apple_media.m` (AVFoundation player), behind two hooks per platform (`za_cg`, `za_redraw`), see `zeus_apple.h` |
| Web | `hosts/web/wasm.c` + `loader.js`: the frame decoded in JS into Canvas2D |
| Android | `hosts/android/android.c` + `ZeusView.java` |
| Linux | `hosts/desktop/linux.c` (X11) |

## Checking a host

- The macOS host can render one frame offscreen through its real paint code,
  with no window: `ZEUS_SNAPSHOT=out.png ./app` writes a 2× PNG and exits.
  `ZEUS_SNAPSHOT_SIZE=1400x5200` sets the window size, so a tall one
  captures a whole scrolling page.
- `tests/draw_golden/replay_trace` pins the exact `ZeusDraw` call sequence.
- `hosts/web/wasm_smoke.ts` decodes every frame the wasm build paints.
