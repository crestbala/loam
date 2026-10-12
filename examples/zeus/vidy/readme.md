# vidy — video player (Zeus)

A video player for mouse, keyboard and touch. One entry runs as a native
macOS window (AVFoundation), in the browser (wasm + `<video>`), and on iOS.
Formats AVFoundation refuses (MKV, WebM, AVI, FLV, WMV…) are converted by
ffmpeg on open, when it is installed (`brew install ffmpeg`).

## Install (macOS)

```sh
sh examples/zeus/vidy/scripts/install.sh            # /Applications/Vidy.app
sh examples/zeus/vidy/scripts/install.sh --user     # ~/Applications/Vidy.app
sh examples/zeus/vidy/scripts/install.sh --uninstall
```

The script builds the macOS target, wraps it in `Vidy.app` (icon rendered
from `assets/icon.svg`, ad-hoc signed) and registers it with Launch Services,
so Finder offers **Open With > Vidy** for video and audio files and a file
dropped on the Dock icon plays. Run it again after a change to update the
installed app.

## Run from source

```sh
./bin/zeli serve examples/zeus/vidy --target macos
./bin/zeli serve examples/zeus/vidy --target web
./bin/zeli serve examples/zeus/vidy --target ios
```

A path or URL after the binary opens straight away; on iOS use `VIDY_OPEN`.
Press **H** in the app for every shortcut.

## Layout

```
app.loam                entry (all targets)
theme.loam              zeus theme roles + the palette components paint with
lib/                    pure, UI-free, reusable
  timecode.loam         clock(ms) → "1:02:03", rate_label(150) → "1.5×"
  aspect.loam           aspect modes, labels, fit_box (picture size on a stage)
  media_name.loam       file_name(path or URL), ext_of(name)
  transcode.loam        the ffmpeg fallback job: command, start, check
player/
  store.loam            signals + every action; the only code that talks to the host player
  keys.loam             keyboard bindings and the help sheet's rows
components/             widgets; each reads the store, none touches the host
  screen.loam           the stage, stacking every layer below
  overlays.loam         picture, empty state, spinner, big play, OSD, tap ripples, error card
  topbar.loam           title pill + round buttons
  controls.loam         the glass control panel
  seekbar.loam          seek bar
  menus.loam            speed / aspect popovers
  playlist.loam         playlist drawer
  help.loam             shortcut sheet
  url_dialog.loam       Open URL dialog
  kit.loam              shared building blocks (Centered, Glyph, When, Pill, Round)
  icons.loam            SVG glyphs
routes/                 the one route + shell pages
scripts/                install.sh, make_icns.sh, render_icon.js
tests/                  see below
```

String work everywhere goes through `std:str` (`packages/loam/std/str.loam`):
`url.trim()`, `src.starts_with("blob:")`, `name.to_upper()`, `str.shell_quote(…)`.

## Tests (headless, a few seconds)

```sh
sh examples/zeus/vidy/tests/run.sh
```

runs, in order:

| Check | File | What it covers |
|---|---|---|
| `std:str` | `packages/loam/tests/inlang/str_tests.loam` | the string helpers vidy uses |
| unit | `tests/unit.loam` | `lib/`: clocks, rate labels, names, extensions, aspect boxes, ffmpeg command and markers |
| e2e | `tests/e2e.loam` | the real screen driven by keys and clicks: open, play/pause, seek, speed and aspect (keys and menus), mute, loop, playlist, help, Open URL, close |
| resize | `tests/probe_resize.loam` | the stage follows the window; nothing scrolls or overflows |
| builds | — | `zeli build --targets macos,web` |

The e2e needs no window or media: the runtime's null player stands in for
the host and moves through the same states. Single checks:

```sh
./bin/loamc test examples/zeus/vidy/tests/unit.loam
ZEUS_HEADLESS=1 ./bin/loamc examples/zeus/vidy/tests/e2e.loam -o /tmp/vidy-e2e && /tmp/vidy-e2e
ZEUS_HEADLESS=1 ./bin/loamc examples/zeus/vidy/tests/probe_layout.loam -o /tmp/vp && /tmp/vp   # draw list
```
