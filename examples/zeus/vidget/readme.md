# vidget — fast video downloader (Zeus)

Paste a link, see what it holds, download it. Built with the `zeli` CLI on
`zeus-components`; runs as a **native macOS window** and as **wasm in the
browser under Vite**. Both talk to one native engine.

Handles direct files (`.mp4 .webm .mov .mkv .ts …`), HLS (`.m3u8`, incl.
fMP4 and AES-128), DASH (`.mpd`), and web pages, including webpack apps whose
stream URL only appears inside a JS bundle. Output: Original, MP4, MKV,
WebM, MP3, or M4A.

## Tools

Only **aria2c**, **curl** and **ffmpeg** (`brew install aria2 ffmpeg`).

| Source | Fetch | Join / convert |
|---|---|---|
| Direct file | `aria2c -x N -s N` split connections (curl fallback) | ffmpeg remux, or encode for WebM/MP3/M4A |
| HLS VOD | Loam parses the playlist; all segments + keys + init in one `aria2c -i list -j N` (curl `xargs -P N` fallback) | ffmpeg joins the local playlist (stream copy, decrypts AES-128) |
| DASH / live / byte-range HLS | ffmpeg reads the URL | — |
| Page | Loam reads `og:` tags, `<video>`, escaped JSON, kt_player `video_url` / `video_alt_url` (best first), same-origin bundles. A host that resets curl is fetched with aria2c | then one of the above |

The preview image is `og:image`, or one frame grabbed by ffmpeg and sent as a
`data:` URI so it also paints on the web.

## Run

Everything goes through the `zeli` CLI. `zeli serve` typechecks the app,
builds and starts the engine (`server/main.loam`, :8080) when nothing is
listening there yet, then runs the target and ties its life to its own
(Ctrl-C stops all of it).

```sh
./bin/zeli serve examples/zeus/vidget --target macos              # native window
./bin/zeli serve examples/zeus/vidget --target web --port 5176    # wasm under Vite
```

Files land in `~/Downloads/Vidget` (`VIDGET_DIR` overrides it, and so does
`LOAM_RPC_ADDR` for the engine address).

For `web`, zeli hands off to the app's own Vite host because `web/vite.config.js`
exists (`npm install` runs on first use). The config compiles `web/app.loam`
to wasm, rebuilds and reloads on any `.loam` change, and proxies `/Vidget/*`
(gRPC-Web) to the engine. The engine has to be native: a browser can't run
aria2c/ffmpeg, and CORS blocks most video hosts.

Pasting a link (or pressing Download / Enter) opens a preview dialog:
thumbnail, title, length, and the format list. Nothing downloads until Save;
Cancel, Escape or the scrim drop it.

Logs come from `std:log`. Every RPC is logged on both sides
(`http.server` lines in the engine's terminal, `http.client` lines in the
browser console or the window's stdout), plus the engine's own actions
(`engine`: probe, job started / done / failed / cancelled). The header's
**Logs** button shows this window's records in the page. `LOAM_LOG=debug`
(or `warn`, `error`, `off`) sets the level on native.

## Layout

```
app.loam, web/app.loam   entries (macOS, wasm); both call ui.start()
components/store.loam    signals, RPC calls, the job-list stream
components/views.loam    header, link bar, preview dialog, downloads
server/api.loam          #[proto] contract (sizes in KiB: int is i32)
server/engine.loam       probe, plan, launch (detached process group), progress, cancel
server/hls.loam          m3u8 parsing + local rewrite + aria2c/curl lists
server/extract.loam      page / bundle extractor
web/                     Vite host (vite.config.js, index.html)
```

## Tests (offline, ~1 s each)

```sh
sh examples/zeus/vidget/tests/fixtures.sh &      # generated media on :8765
ZEUS_HEADLESS=1 ./bin/loamc test examples/zeus/vidget/app.loam           # parsers
./bin/loamc examples/zeus/vidget/tests/engine_cli.loam -o /tmp/vg
VIDGET_DIR=/tmp/vg-t /tmp/vg get http://127.0.0.1:8765/hls/master.m3u8 "" 1
VIDGET_NO_ARIA2=1 ...                             # exercise the curl path
# with the engine running: headless paste -> preview dialog -> save -> done
ZEUS_HEADLESS=1 ./bin/loamc examples/zeus/vidget/tests/ui_e2e.loam -o /tmp/vg-ui && /tmp/vg-ui
```

DRM-protected streams (Widevine/FairPlay) are not supported.
