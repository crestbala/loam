# Route-based wasm splitting

Goal: a visitor downloads only the code for the page they asked for, sees real
content before any wasm arrives, and moves between pages with no visible wait.

This is a Zeus and `zeli` feature, not a `www` feature. Any web app with routes
gets it. `www` is the first user and the test bed.

Status: Phases 1 to 4 done. Painting before wasm and trimming the primary are next. Sizes under "What we measured" are from `feat/www` at `e981527`. Compression is deferred (see [Later](#later-compression)), so goals
are in raw bytes.

## What we measured

### Size

`www/build/web/app.wasm`, the one file every visitor downloads today:

| | Raw | Gzip | Brotli |
|---|---|---|---|
| `app.wasm` | 1,649 KB | 517 KB | 368 KB |
| `loader.js` | 76 KB | 22 KB | not measured |

- Code is 96% of the file (2,553 functions). Data is 3%.
- The showcase pages added 427 KB. The file grows with every page, on every page.
- `wasm-opt -Oz` saves nothing more, because clang already runs it after linking
  whenever `wasm-opt` is on `PATH`. Without it the code is 2.1 MB, not 1.6 MB, so
  the build's size silently depends on what is installed.

### What each route needs

Reachable code per route, from the linker map and the call graph of the emitted
C, in raw KB before compression. "Own" is everything the route reaches beyond
the core. "Unique" is what only that route uses.

| Route | Own | Unique |
|---|---|---|
| Core (engine, router, layout, drawer) | 415 | |
| `/playground` | 535 | 467 |
| `/components/forms` | 186 | 115 |
| `/components` | 150 | 54 |
| `/components/charts` | 142 | 90 |
| `/components/overlays` | 138 | 60 |
| `/components/data` | 128 | 69 |
| `/components/navigation` | 111 | 49 |
| `/` | 61 | 12 |
| Each docs page (18 routes) | 31 to 48 | 2 to 5 |

So a docs reader downloads the playground's 467 KB interpreter and editor and
never runs it.

### Does splitting work on this binary?

Yes. `wasm-split --multi-split` (binaryen 131) was run on the real build with a
manifest generated from the numbers above:

| Module | Raw | Brotli |
|---|---|---|
| `primary` | 720 KB | 184 KB |
| `r-playground` | 469 KB | 81 KB |
| `r-components_forms` | 118 KB | 33 KB |
| `r-components_charts` | 92 KB | 26 KB |
| `components-common` | 69 KB | 20 KB |
| `r-root` (`/`) | 13 KB | 4 KB |
| Each docs page | 0.5 to 6 KB | 0.4 to 2 KB |

Each chunk imports the primary's memory, table, stack pointer and functions. The
primary reaches chunk functions through a second table that `wasm-split` adds.

### What the real build produces

`zeli build www` in 25 seconds, all raw:

| Route | Downloads | Total | Today |
|---|---|---|---|
| Docs pages (`/language`, `/types`, ...) | primary | 681 KB | 1,650 KB |
| `/` | primary + 2 | 808 KB | 1,650 KB |
| `/components/*` | primary + 3 | 868 to 930 KB | 1,650 KB |
| `/playground` | primary + 3 | 1,269 KB | 1,650 KB |

In a real browser, all 27 routes load cold with no placeholder calls and no errors,
and navigation, back and forward all go through the router gate. That is 59% less
for a docs reader. The primary is still above the core's 415 KB, which is what
Phase 6 is for.

### Where the blank screen comes from

First draw after the wasm response starts arriving, on `/language`:

| | Headers | Compiled | First draw |
|---|---|---|---|
| Laptop | 15 ms | 22 ms | 40 ms |
| CPU throttled 4× | 16 ms | 27 ms | 65 ms |

These are from a local server on a fast laptop, so a real low-end phone will be
slower. The shape still holds: startup work is small, and **the blank screen is
mostly the network**. Three things make it worse:

1. **Downloads run in series.** The wasm request cannot start until `loader.js`
   has downloaded and run, which costs an extra round trip.
2. **Nothing is painted meanwhile.** The HTML holds an empty canvas.
3. **The first colour is wrong.** The HTML sets `background:#fafafa`, so the dark
   site flashes white.

At 200 KB/s (slow 4G), before round trips, an uncompressed first load takes 8.2 s
today and 3.7 s with the split above. Splitting halves the wait, and painting
before wasm arrives (Phase 5) removes the blank screen itself.

## Goals

| Budget, raw bytes | Today | Target |
|---|---|---|
| First load of `/` or a docs page | 1,650 KB | 550 KB or less (681 and 808 now) |
| First load of `/playground` | 1,650 KB | 1,050 KB or less (1,269 now) |
| First load of a `/components` page | 1,650 KB | 750 KB or less (930 now) |
| Time until real content shows, slow 4G | seconds | under 500 ms |
| Next page, chunk already prefetched | n/a | no visible wait |
| White flash on a dark theme | yes | none (done) |

Non-goals: splitting inside a page by default (see Phase 6), and any change for
native, iOS or Android hosts, where splitting is a no-op.

## Design

### Chunks

```
primary        engine, router, layout, shell, anything most routes need
docs           code shared by the docs pages, merged with their small uniques
ui-common      component code shared by the /components pages
r-<route>      code only that route needs
```

Chunks come from a **usage signature**: the set of routes that reach a function.

1. Compute each function's signature from the compiler's reachability data.
2. Functions reached by the layout, the C runtime, or most routes go in `primary`.
3. Functions with the same signature form a group.
4. Merge any group under a minimum size (8 KB raw to start) into its nearest
   larger group. A 1 KB chunk costs more in requests than it saves in bytes.
5. Cap the number of chunks a route needs at 3.

This mirrors what JS bundlers do, and needs no annotations in app code.

### Why wasm-split, with placeholders

We split after linking with `wasm-split --multi-split`. It works on the finished
binary, so the compiler keeps a single code path, and it leaves the primary's
memory, table and data in one place.

Calling a function whose chunk has not loaded is the classic failure. Two layers
prevent it:

- **The normal path.** The router loads a route's chunks *before* it builds the
  page, so nothing unloaded is ever called.
- **The safety net.** `wasm-split` leaves a placeholder in each empty table slot.
  If one is ever called, the loader fetches that chunk with a synchronous XHR,
  instantiates it, and retries the call. This is slow but correct. It logs a
  warning in dev and a build-graph gap fails a test, so it stays exceptional.

### Navigation

`router.navigate` already has a place for this: `start_loaders` runs before the
page builds, and `routes/loading.loam` exists for pending routes.

```
navigate(path)
  match route -> chunks needed
  all loaded?  -> build the page now, same frame
  else         -> fetch, keep the current page on screen
                  on load: build the page
                  on failure: retry twice, then reload the URL once
```

The URL and the active nav link update at once, so a click always responds. Back
and forward use the same path. If the user moves on while a chunk loads, the newer
route wins and the older answer is ignored.

Still to add: showing `loading.loam` after 150 ms, for a slow connection.

Before Phase 3 the web host started every app at `/` and sent the real URL after
`main`, so each deep link built the home page first and threw it away. The router
now reads the URL before its first build, which also helps unsplit builds.

### Loading ahead of need

| When | What | How |
|---|---|---|
| In the HTML | Primary and this route's chunks | `<link rel="preload" as="fetch" crossorigin>`, emitted per route |
| Pointer rests on a link for 80 ms | That route's chunks | `router.preload(path)` from the link's `on_hover`, skipped on `saveData` and 2G |
| Browser idle after first paint | Chunks shared by 2 or more routes, up to 150 KB, one at a time | The build lists them; skipped on `saveData` and 2G |
| Repeat visit | Everything fetched before | Content-hashed names with `immutable` caching |

Every route HTML names its own files, so a deep link to `/language/errors` starts
all of its downloads in parallel with `loader.js`.

### Painting before wasm

`zeli build` already compiles and runs a native helper to collect each route's
`meta()`. The same step will mount each route headlessly and write its text tree
into that route's HTML as plain semantic markup with a small inline stylesheet:
headings, paragraphs, code, and links. It uses the theme's real colours, and a
small inline script picks light or dark before first paint.

The canvas fades in over that markup on its first frame, and then the markup is
removed. Search engines and link previews get real content as a side effect.

The page cannot be used until wasm is running, except that the markup's links are
real links and work as normal navigation.

### Files and caching

```
index.html, language/index.html, ...         no-cache
_zeus/loader.<hash>.js                       immutable
_zeus/app.<hash>.wasm                        immutable: the primary
_zeus/c<n>.<hash>.wasm                       immutable: route chunks
```

Loam code refers to routes by index, and the loader turns an index into files through a table
inlined in each page, which costs no request.

An old tab can outlive a deploy. If a chunk cannot be fetched after retries, the
loader reloads the target URL once and picks up the new build. A second failure
stops there, so it cannot loop.

## Phases

Each phase ships alone and improves the site without the next one.

### Phase 1: quick wins, no splitting (done)

- Theme-correct background and `theme-color` from the first byte. The build reads
  the colour by mounting the app headlessly, so no app has to configure it.
- `rel="preload"` for the wasm. Verified in Chrome: one request, sent at the same
  moment as `loader.js`, with no console warnings.
- Content-hashed assets under `/_zeus/`, and `_headers` for caching.
- `zeli build` fails with a clear message if `wasm-opt` is missing.
- A per-file size report at the end of `zeli build`.

### Phase 2: the build pipeline (done)

- `loamc --wasm-split-input` keeps symbol names and writes the C beside the output.
  It passes `-Xclang -Oz`, because clang runs `wasm-opt` after any link it sees `-O`
  for, and that strips the names.
- `packages/zeus/cli/chunks.loam` plans the chunks. The graph comes from the emitted
  C, not the compiler's DCE pass: that pass works on the AST, where the closures
  and generic clones that make up most of the binary do not exist yet.
- `zeli build` runs `wasm-split`, then `wasm-opt -Oz` on each module, then hashes
  them. That order matters: optimising first merges identical functions, which ties
  routes to each other.
- `zeli serve` splits too, so dev shows what visitors get. Without binaryen it falls
  back to one module, and only `zeli build` insists.

### Phase 3: loading at runtime (done)

- The page's chunks download with the primary and link before `main`.
- `plat_chunks_ready` and `plat_chunks_load` on the web host, no-ops elsewhere.
- Router gate for navigate, replace, back and forward.
- Placeholder safety net, retries, and a single reload on failure.
- Chunks relink on a restarted instance, so `Boundary` restart still works.

### Phase 4: prefetching (done)

- `router.preload(path)`, called from `on_hover` on the site's links. The newest
  hover replaces a waiting one, so a sweep down a list costs nothing.
- `zeli` lists the chunks to fetch at idle, so apps need no code for it. A touch
  screen has no hover, so a tap still waits for its chunks.

### Phase 5: painting before wasm

- Headless text-tree extraction at build time.
- Pre-paint markup, inline styles, canvas swap.

Done when: on slow 4G a deep link shows its real heading and text in under 500 ms.

### Phase 6: trim the primary

Measure each, keep what pays:

- Module init code: `loam_unicode__init` (17 KB) and `loam_colors__init` (16 KB)
  build tables at startup that could be static data.
- `-DLOAM_ALLOC_TRACE` is on for every Zeus web build; move it to dev only.
- Wrappers: the prototype's primary has 310 exports and 1,192 placeholder imports.
- Optional `zui.Lazy(...)` for a heavy widget inside a page, such as the
  playground's editor, loaded when it is needed.

## Testing

- **Headless:** chunk assignment tests on a small route fixture, including merging,
  the chunk cap, and a route with no unique code.
- **Browser, added to the existing CDP checks:**
  - Every route on a cold load, with no placeholder warnings.
  - Every route reached by navigation, back and forward.
  - A chunk that cannot be fetched reloads once and does not loop.
- **Budgets:** `nob` fails if a route's first-load total passes its budget, so size
  cannot creep back up unnoticed.
- **Unsplit parity:** the full suite still runs on unsplit builds, and draw goldens
  do not change.

## Risks

| Risk | What we do about it |
|---|---|
| A placeholder fires in production | Safety net keeps it correct; a test fails on any placeholder hit |
| Synchronous XHR is slow, or blocked for a large chunk | Only the safety net uses it, and chunks stay small enough for synchronous compile |
| `wasm-split` output format changes | Pin a minimum binaryen version; the web build checks it |
| Module init touches chunk code at startup | All `__init` functions are roots, so they stay in the primary |
| Function pointers across modules | They are table indexes, shared by every module, so identity holds |
| Trap recovery across instances | Stack pointer is a single shared global; covered in Phase 3 tests |
| The planner misses a function | Safety net keeps it correct; this happened once in development (see below) |
| Pre-paint text differs from the canvas layout | The swap is a short fade, and the text is the same content |
| Too many small chunks | Minimum size and the 3-chunk cap |

## Lessons from the build

The first real split ran every page correctly, but the safety net fired on all 27
routes. The parser had missed `main`, which opens behind an `#if`, so the layout's
code looked like route code. Nothing broke, which is what the safety net is for,
and the browser check caught it. Keep that check.

## Later: compression

Deferred on purpose. Splitting comes first, and we will check this afterwards.

- Brotli takes `app.wasm` from 1,650 KB to 368 KB, and the prototype's primary to
  184 KB, so this is worth doing next.
- Needs a decision on the host. There is no deploy config in the repo, so we do not
  yet know whether it compresses `.wasm` itself or needs `.br` files from the build.

## Open questions

- **Where will this be hosted?** `_headers` covers Netlify and Cloudflare Pages;
  other hosts need their own config, and this decides the compression work.
- **Does the home page keep its live demos?** Each extra component it uses moves
  code into its first load.
