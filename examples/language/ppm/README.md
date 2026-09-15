# PPM frames in Loam

Raster demos that write binary **PPM (P6)** frames from Loam and hand the
sequence to **ffmpeg** — no C, no image library, no video library. Both
programs are ports of [rexim's `checker.c` / `plasma.cpp`
gist](https://gist.github.com/rexim/ef86bf70918034a5a57881456c0a0ccf):

| File | What it is |
|---|---|
| `checker.loam` | Port of `checker.c`: 60 frames of a red/black checkerboard whose cells slide one cell per frame. |
| `plasma.loam` | Port of `plasma.cpp`: XorDev's twigl interference shader evaluated per pixel on the CPU. |
| `frames.loam` | The PPM/ffmpeg pipeline both demos share: frame naming, the P6 header, `sys.write_file`, `ffmpeg` via `sys.exec`. |
| `mathf.loam` | `sin` / `cos` / `exp` / `tanh` / `abs` / `sqrt` for `float` (f32), as polynomials in plain Loam. |
| `mathf_tests.loam`, `frames_tests.loam` | `#[test]` suites for the two modules. |

## Run

From the repository root, after `make`:

```
./bin/loam examples/language/ppm/checker.loam --run    # ~2s, 960x540 x 60
./bin/loam examples/language/ppm/plasma.loam  --run    # ~52s, 960x540 x 120
```

`./run.sh language/ppm/checker` does the same thing (it runs `loam check`
first and then `loam --run`).

Frames and the `.mp4` land in `./out` (`out/checker.mp4`, `out/plasma.mp4`).
Everything is re-scalable without editing a file:

| Variable | Default | Meaning |
|---|---|---|
| `LOAM_PPM_OUT` | `out` | output directory (created with `sys.mkdir`) |
| `LOAM_PPM_SCALE` | 60 | `w = 16 * scale`, `h = 9 * scale` — the gist's `16*60 x 9*60` = 960x540 |
| `LOAM_PPM_FRAMES` | checker 60, plasma 120 | frame count |
| `LOAM_PPM_FPS` | 60 | output frame rate |
| `LOAM_PPM_CRF` | 16 | x264 quality; `0` is lossless, `23` is x264's own default |
| `LOAM_PPM_PRESET` | `slow` | x264 preset (`fast`, `medium`, `slower`, `veryslow`, …) |
| `LOAM_PPM_KEEP` | unset | keep the `.ppm` files after encoding |
| `LOAM_PPM_FLIP` | unset | plasma only: flip the row the shader sees (see below) |

The plasma defaults are the gist's own 960x540 at 120 frames (it uses 240);
the render is CPU-bound, so `LOAM_PPM_SCALE` and `LOAM_PPM_FRAMES` are the two
levers on how long it takes and how much memory it holds.

```
# Cheapest useful preview: 1/16 the pixels and 1/8 the frames.
LOAM_PPM_SCALE=15 LOAM_PPM_FRAMES=15 ./bin/loam examples/language/ppm/plasma.loam --run

# The gist's exact framing, and lossless to boot (~8 MB for 2s).
LOAM_PPM_FRAMES=240 LOAM_PPM_CRF=0 ./bin/loam examples/language/ppm/plasma.loam --run
```

## How it works

One frame is one `[]int` byte buffer:

```yuga
let mut b = frames.image(w, h)          // "P6\n<w> <h>\n255\n"
frames.rgbf(&mut b, c.x, c.y, c.z)      // 3 bytes per pixel, clamped
frames.save(dir, "plasma", f, 3, b)     // sys.write_file(...)
```

The two language features doing the real work are both already in `std:sys`:

- `string_from_bytes([]int)` is the language's one bridge from a byte buffer to
  a `string`, which is why the pixel buffer is `[]int` and not `[]u8`. It masks
  each element to a byte, so NUL bytes are fine.
- `sys.exec(cmd)` runs the shell, which is where `ffmpeg` comes in:

  ```
  ffmpeg -y -loglevel error -framerate 60 -i out/plasma-%03d.ppm \
         -c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p out/plasma.mp4
  ```

  `-framerate` is on the *input* side: without it a still-image sequence is
  read at ffmpeg's default 25 fps and the video plays at the wrong speed. The
  exit status comes back through `sys.exec_status`, so a missing ffmpeg is
  reported instead of silently producing nothing.

## Encoding quality

A plasma is a field of very smooth gradients, which is the case x264 handles
worst: at its default `-crf 23` the ramps get quantized into visible bands and
the whole clip lands around 0.9 Mbps. Both the `-crf` and the `-preset` are
part of the command ffmpeg is handed, and the demos default to a quality
setting rather than ffmpeg's. Measured on the plasma, 960x540 x 120 frames, as
PSNR/SSIM against the source `.ppm` frames:

| Setting | Bitrate | Size | PSNR | SSIM |
|---|---|---|---|---|
| `-crf 23` (x264 default, not used) | 0.88 Mbps | 219 KB | 47.8 dB | 0.9924 |
| `-crf 16 -preset slow` (default) | 1.81 Mbps | 452 KB | 51.1 dB | 0.9955 |
| `-crf 14 -preset slow` | 2.32 Mbps | 581 KB | 51.9 dB | 0.9961 |
| `-crf 0` (lossless) | 31.9 Mbps | 8.0 MB | — | 1.0000 |

`yuv420p` is deliberate over `yuv444p` (which is smaller here and keeps full
chroma) because it is the format everything plays. If you want to judge
encoder settings yourself, `LOAM_PPM_KEEP=1` leaves the frames in place, and
any ffmpeg invocation can be pointed at `out/plasma-%03d.ppm`:

```
LOAM_PPM_KEEP=1 ./bin/loam examples/language/ppm/plasma.loam --run
ffmpeg -framerate 60 -i out/plasma-%03d.ppm -c:v libx264 -crf 12 \
       -preset veryslow -pix_fmt yuv420p out/plasma-hq.mp4
```

The `.ppm` files are the actual image; nothing is lost before ffmpeg sees them,
so the encoder is the only place quality is decided.

The demos also round the scale factor down to an even number, because H.264
with `yuv420p` requires even dimensions.

### Why `mathf.loam` exists

`plasma.loam` needs `sin`, `cos`, `exp`, `tanh` and `abs` on floats, and there
is no `std:math`. Rather than adding a C seam, `mathf.loam` writes them as
argument reduction plus a Horner polynomial — plain Loam arithmetic, accurate
to about `1e-6` relative, which is far below one 8-bit color step. That also
keeps the demos portable to `--target wasm`, since nothing crosses the C
boundary except `sys` and `fmt`.

## Fidelity to the originals

`checker.loam` produces **byte-identical** files to `checker.c`: all 60 frames
at 960x540 compare equal.

`plasma.loam` matches `plasma.cpp` except for float rounding at the truncation
boundary — libm's `sinf`/`expf`/`tanhf` versus the polynomials here differ in
the last bits, and a channel that lands on `x.9999999` rather than `(x+1).0`
truncates one lower. Measured against the C++ reference:

| Run | Bytes differing | Max delta |
|---|---|---|
| 3 frames @ 64x36 | 3 of 20,736 | 1 |
| 4 frames @ 480x270 | 52 of 1,555,200 | 1 |

Comment 6 on the gist notes that the C++ port flips the image relative to the
shader it came from (the blue field ends up at the top). `plasma.loam` keeps
the C++ behaviour by default and exposes the row flip as `LOAM_PPM_FLIP=1`
rather than hardcoding either one.

## Tests

```
./bin/loam test examples/language/ppm/mathf_tests.loam
./bin/loam test examples/language/ppm/frames_tests.loam
```

`mathf_tests.loam` checks the polynomials against reference libm values,
including the reduction seams (`sin²x + cos²x == 1` across the period) and the
saturation ends (`exp` clamps instead of overflowing, `tanh(±20) == ±1`).
`frames_tests.loam` covers frame naming, the float → byte clamp (NaN included),
and the environment reader.

These are not in `make test`, which globs `examples/language/*.loam` and would
otherwise spend a minute rendering every time the suite runs.

## Memory

Frame bytes are reclaimed as the render goes, so peak memory is the frame count
times one leaked *string* (see below) plus the allocation high-water of the
frame buffer:

| Run | Frames | Pixels/frame | Max RSS |
|---|---|---|---|
| Checker 960x540 | 15 | 518,400 | 135 MB |
| Checker 960x540 | 60 | 518,400 | 231 MB |
| Plasma 960x540 (default) | 120 | 518,400 | 252 MB |
| Checker 480x270 | 60 | 129,600 | 72 MB |
| Checker 96x54 | 60 | 5,184 | 20 MB |

That is about **1 byte per frame byte**, which is exactly the string
`string_from_bytes` returns: `loam_rt.h` frees no string ("the language has no
string ownership story to hook into yet"), so one frame's bytes stay resident
per frame. Everything else — the `[]int` staging buffer, which is four bytes
per frame byte — is allocated and released once per frame.

Getting there needed three ownership fixes in the compiler, all of which are
in `packages/loam/src`: the `[]int` that feeds `string_from_bytes` used to be
leaked twice over (once as a call argument whose caller drop was dropped, once
as a `let` inside a loop body that was only released on the way *out* of the
loop). Before them the same plasma held **985 MB** and the checker **512 MB**,
about 5.5 bytes per frame byte; a `250`-frame render at 960x540 was not
practical.

So the lever on peak memory is still the frame count and the size — and
`LOAM_PPM_SCALE=15 LOAM_PPM_FRAMES=15` keeps a preview under 20 MB.

## Limitations

- **ffmpeg must be on `PATH`.** `libx264` has to be in that build of ffmpeg
  (`-c:v libx264`); anything else that reads a PPM sequence will do if you
  change `frames.encode`.
- **H.264 wants even dimensions**, which is why both demos push the scale
  factor through `frames.even`. An odd scale would render fine and then fail
  at the encode.
