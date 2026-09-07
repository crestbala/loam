# GreenInfer

Ultra-lightweight, zero-dependency, local-first vector memory engine on
macOS Apple Silicon — written in Yuga. Phase 1 ships the **memory engine
core + a macOS desktop check UI** (Zeus/Cocoa); the watcher and frontier
agent phases land on top next.

```
examples/zeus/greeninfer/
  greeninfer.yuga            desktop UI (engine + watcher + search harness)
  engine.yuga                the engine: all policy in Yuga (capacity, count
                             mirror, top-k sweep, embeddings, cosine norm)
  watcher.yuga               folder scan, 500-token chunker, manifest diff,
                             per-chunk labels/snippets
  agent.yuga                 frontier pipeline: retrieval -> context payload
                             -> offline reply or optional TLS API call
  main.yuga                  headless CLI (greeninfer-macos)
  smoke.yuga                 headless self-test (no window)
  runtime/greeninfer_runtime.c   the C seam — POSIX mmap + ARM NEON kernel
                             + file data-movement trampolines, nothing else
```

## The seam rule

Everything that decides lives in Yuga, where the language's checks apply:
bounds traps on every row access, overflow traps on every counter, safe
ownership of every buffer. The C seam is only what Yuga cannot express:

- `gi_sys_mmap` / `gi_sys_munmap` — POSIX zero-copy mapping (`MAP_SHARED`;
  pages fault in, nothing is ever `read()` into a Yuga buffer)
- `gi_neon_dot_product` — Apple Silicon NEON kernel (`float32x4_t`,
  `vmlaq_f32`, `vaddvq_f32`)
- row trampolines — a map is an opaque `int` handle; each entry re-validates
  handle, header magic, and row bounds before touching the mapping. Yuga
  never sees a raw pointer.

File layout (engine-owned constants, sealed in the seam):

```
64 B header  { magic "GIM1", u32 dim, u64 cap, u64 count }
cap rows     { u32 id + 384 f32 }        stored vectors
1 scratch row                            query staging for the search sweep
```

`search_knn` (engine.yuga) stages the query once in the scratch row, then
sweeps every stored row with one in-place NEON dot each — no per-row
conversion, no copy — while Yuga keeps the top-k.

## Build & run (macOS desktop)

The compiler auto-links the seam: yugac compiles and links
`runtime/<app>_runtime.c` whenever it sits next to the entry program
(`driver.c`). From the repo root:

```
make                          # yugac (once)
./run.sh greeninfer           # compile + open the Cocoa window
```

Or step by step, as the toolchain intends:

```
# 1. the C runtime: NEON + mmap primitives
clang -O1 -I packages/compiler/runtime -c \
  examples/zeus/greeninfer/runtime/greeninfer_runtime.c \
  -o examples/zeus/greeninfer/build/greeninfer_runtime.o

# 2. the native arm64 binary (Yuga -> C99 -> cc; seam auto-linked)
./bin/yugac examples/zeus/greeninfer/greeninfer.yuga -o greeninfer-macos
./greeninfer-macos
```

## Headless self-test + CLI

```
# smoke: engine + chunker + cosine + folder scan assertions
YUGA_LINK_EXTRA="examples/zeus/greeninfer/runtime/greeninfer_runtime.c" \
  ./bin/yugac --run examples/zeus/greeninfer/smoke.yuga

# CLI demo: index ./examples/zeus/greeninfer, run 4 prompts, print hits + timing
YUGA_LINK_EXTRA="examples/zeus/greeninfer/runtime/greeninfer_runtime.c" \
  ./bin/yugac examples/zeus/greeninfer/main.yuga -o greeninfer-macos
./greeninfer-macos
```

`make test` also compiles and headless-runs the desktop app itself.

## Phase plan

1. **Engine core + desktop check** (done): mmap storage, NEON sweep,
   word-overlap embedding stub, timing in the UI.
2. **Watcher + chunker** (done): recursive folder scan with manifest diff,
   500-token chunk boundaries for source + markdown, cosine embeddings
   (L2-normalized on insert, so sim % is cosine), live auto-reindex in the
   desktop app.
3. **Frontier agent** (`agent.yuga`, done): KNN-retrieved context assembled
   into a minimal payload; offline context answers by default, optional TLS
   call to Anthropic / xAI style endpoints when a key is configured (all
   failures degrade to the offline answer).
4. **CLI + packaging** (`main.yuga`, done): `greeninfer-macos` build, RAM
   budget pass (< 15 MB RSS, measured).

Remaining for the presentation demo: final UI pass over the watcher +
search experience (kept untouched until the engine layers were complete).
