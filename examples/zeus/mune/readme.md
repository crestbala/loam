# Mune

A piece made from your own recordings. The sound in the file is those recordings.

Mune cuts phrases on each recording's pulse, hears their chords, and sequences them with concatenative synthesis and a Markov chain along an energy arc: a quiet intro, a verse, a build, a full drop, a break, a second drop that brings the first one back, and an outro. The synths play the chords the recordings play, so the two agree, and the chain keeps a recording's own order and repeats its hook rather than jumping around. The energy strip under the waveform shows the arc. The header switches between light and dark.

Choose recordings opens Finder so you can select several audio files. You can also drop files on the window. Voices are removed from stereo recordings before the piece is made.

You need the rights to the recordings you import, and to the piece made from them. Mune does not ship any songs.

```
./bin/zeli serve examples/zeus/mune --target macos              # native window
./bin/zeli serve examples/zeus/mune --target web --port 5175    # wasm under Vite
```

Rendering needs ffmpeg (`brew install ffmpeg`), on the machine that is
rendering. The **web** build has no ffmpeg in it — a browser cannot spawn it —
so it talks to the same native engine the macOS window does
(`server/main.loam`, :8080), over gRPC-Web through the Vite proxy
(`web/vite.config.js`, the pattern `examples/zeus/vidget` uses). `zeli serve`
builds and starts that engine before the UI on either target.

```
sh examples/zeus/mune/tests/run.sh
```

How a piece is made is in `DESIGN.md`.
