# Loam + Zeus v2 — drawbacks and the plan to fix them

Scope of this document:

1. Honest drawbacks of the Loam language as it stands today.
2. Honest drawbacks of Zeus as a UI library.
3. The numeric type system rebuild (`f32` / `i32` default, sized + unsigned types).
4. Zeus taking React's *structures* while rejecting React's *implementation*.
5. Zeus taking Next.js's *structures* the same way — file routing, layouts,
   server functions, metadata — with no DOM and no JS for UI.
6. Repository layout with Zeus lifted **out of `packages/`**, plus the app-side
   folder structure the framework imposes.
7. Phases, in order, with exit criteria.

This supersedes the framing in `docs/downsides.md`. That file closed eight phases
of "make the same app run on every host." This one is about making Loam a language
people can write large programs in, and Zeus a framework people can ship products
with.

---

## Part 0 — The one-line summary

Zeus already has the hard part that React, Next.js and Flutter had to retrofit:
a retained tree, fine-grained signals, one draw list, and the same source on five
hosts. What it lacks is *everything around* that core — routing, data loading,
error recovery, text, a11y, dev tooling, and a type system that doesn't waste
half of every cache line.

The design rule throughout: **take React's and Next.js's structures, take none of
their implementations.** The conventions were right; the machinery underneath them
was a response to constraints (a DOM, a JS runtime blind to state changes, a
bundler) that Zeus does not have. Anyone who knows Next.js should recognize the
file layout on sight and find that half the concepts they learned no longer exist.

The correct order is: **types → text → boundaries → router → server → tooling.**
Text and types are load-bearing for everything after them.

---

# Part 1 — Loam: real drawbacks

Ordered by how much they will hurt once other people write Loam code.

### 1.1 No error-handling discipline (biggest gap)

There are no algebraic enums, no `Option`, no `Result`, and no way to force a
caller to look at a failure. Every fallible API therefore invents its own
convention (an `ok` field, a sentinel `-1`, a zero struct), and the compiler
cannot tell you that you ignored it. In a 200-line program that's fine. In a
framework where `http.call`, file reads, image decodes, and route loaders can
all fail, it is the thing that will produce silent wrong behaviour in production.

**Fix that does not require a Rust-shaped type system:** a compiler-known
`#[must_check]` attribute on a struct. If a value of a `#[must_check]` struct is
dropped without at least one field read on every path, that's a compile error.
Add one std struct, `Res<T>` (`ok: bool`, `val: T`, `err: string`), marked
`#[must_check]`, plus an `or_trap()` / `or(default)` helper pair. This is ~200
lines in `borrowck.c` (you already track drops) and gets 90% of `Result`'s value
with zero new type-system machinery, no pattern matching, and no `?` operator.

### 1.2 Borrowck is not NLL

A borrow lives until the binding leaves scope. That produces false rejections
that force people to wrap things in extra blocks or reorder code for no semantic
reason — the single most common complaint against early Rust, and the reason NLL
happened. You already build a CFG in `ir.c`; end-of-borrow can be computed as
last-use liveness on that CFG. This is a contained change (~300–400 lines) and it
removes an entire category of "why won't it compile" from the new-user experience.

### 1.3 Closures are Copy-capture, memcpy'd, and interned for the process

Three separate problems wearing one coat:

- **Copy-only capture** means you cannot capture a `[]T` or a `Box<T>`. All real
  state has to be smuggled through global arena handles. That's workable because
  `Signal` is a handle — but it means the escape hatch *is* the design, and a user
  who doesn't know that writes broken code first.
- **`env` is memcpy'd at `plat_intern_fn`** — captured values are a *snapshot*.
  This is exactly React's stale-closure bug, reintroduced in a language that was
  supposed to be immune to it. If someone captures an `int` instead of a
  `Signal<i32>`, the handler sees a frozen value forever, with no diagnostic.
- **Interned for process lifetime** — every rebuilt component leaks its handler
  envs. A long-running app that rebuilds a list on every data refresh leaks
  monotonically. This is a real bug, not a design tradeoff.

**Fix:** closure envs get a refcount and an owner. When a `Node` is dropped, its
interned handlers drop with it. Separately: warn (then error) when a closure
captures a non-handle value by copy and that value is also mutated in the
enclosing scope — that is always a stale-capture bug.

### 1.4 Text is not solved, and it blocks four other things

`string` is bytes with a length, `s[i]` is a byte, and there is no grapheme or
cluster layer. Consequences:

- Caret movement, selection, and backspace in `Input` are wrong for anything
  outside ASCII. For Tamil and other Indic scripts they are badly wrong — a single
  visual character is several code points and must move as one unit.
- Text metrics come from the host (`measureText` on Canvas2D, Core Text on macOS,
  Android's `Paint`). Those three disagree. So "the same source on every host"
  is true for layout boxes and false for text — the one thing users actually look at.
- Any server-side or build-time rendering is impossible while metrics live in the
  host, because the server has no Canvas2D.
- Rich text spans are already blocked on this (`downsides.md` Phase 8, "Won't").

**One fix unlocks all four: in-tree font metrics.** Ship a font, parse its tables
in Loam, do shaping and line breaking in Loam, and make `measure_text` a pure
function of (font bytes, size, string). Hosts then only draw glyph runs at
positions Loam computed — `plat_glyphs(run)` instead of `plat_text(str)`.

**The honest conflict:** real shaping for Indic scripts is HarfBuzz-class work.
HarfBuzz is C *library logic*, not a platform/vendor API, so linking it violates
your own C boundary rule. Three options, pick one deliberately:

| Option | Cost | Result |
|---|---|---|
| Port a minimal shaper to Loam (Latin + the scripts you target) | Months, but it's a real Loam workload | Rule intact, pixel-identical everywhere |
| Link HarfBuzz, document it as a named exception | Days | Rule has one hole, but a defensible one |
| Keep platform shaping | Zero | Cross-host text never matches; no SSR; caret stays wrong |

Recommendation: option 2 now, option 1 as a long-horizon project. Option 3 quietly
kills the framework's main claim.

**Decision (recorded; implementation deferred — "look at later").** The
direction this tree takes is **option 1**: port a minimal shaper into Loam for
the scripts it targets, so shaping, metrics, and glyph runs stay Loam values and
`docs/boundary.md` holds. Until the port lands, metrics are unshaped — one glyph
per grapheme cluster — and `std:font.glyph_run` returns that unshaped run.
**Option 2 (HarfBuzz) is not adopted as the architecture:** it is C *library
logic*, which the boundary rule rejects; it computes advances only (drawing still
needs a host rasterizer); and it grows the wasm artifact. Reopen it only if
correct Indic advances are needed before the port, and then only as a temporary,
named exception behind `platform.loam` and marked for removal. Option 3 is
rejected.

The shaper boundary to design when this is picked up: one `shape` step turning a
grapheme cluster plus its script into glyph ids and advances, consumed by
`std:font.glyph_run`; the host ABI stays `plat_glyphs(run)`, so shaping itself
needs no host change.

### 1.5 Debug experience is generated C

Debuggers, profilers, and crash reports show `a.c`, not `app.loam`. **Cheap, large
fix:** emit `#line N "app.loam"` directives in `codegen_c.c`. lldb, gdb, perf,
Instruments and every sanitizer will then report Loam line numbers with no other
work. This is a day of work and it changes how the language *feels*.

### 1.6 No test story inside the language

Tests are compiler fixtures (`compile_pass` / `compile_fail` / golden). There is no
way for an application author to write a test. Add `#[test] fn` and `yugac test` —
collect, run, report. Without it nobody will write a serious app in Loam.

### 1.7 No formatter

A new language's biggest cheap win is that formatting is not a conversation.
`yugafmt`, driven by the existing parser (or the tree-sitter grammar), with one
style and no options.

### 1.8 No dependency story at all

`import "path.loam"` is relative-path only. There is no way to use someone else's
code, pin a version, or vendor. You don't need a registry. You need:
`import "pkg:name"` resolving to `vendor/name/`, plus a lockfile of git URLs and
SHAs, plus `yugac vendor sync`. Ten percent of the work, ninety percent of the value.

### 1.9 Async and threading exist — the gaps are at the edges

Correction to an earlier framing of this document: Loam **has** async and
multithreading. `std:thread` gives detached OS threads with a Send discipline and
`Chan<T>` back to the UI loop; `http.call_async` is non-blocking including the TLS
handshake. This is not a missing feature.

What remains are three specific edges, each worth writing down as a scope decision
rather than discovering later:

- **wasm `spawn` is a no-op.** The browser has Web Workers and, with COOP/COEP
  headers, `SharedArrayBuffer`. So a real wasm thread pool is possible, but it
  needs the cross-origin isolation headers on whatever serves the app, and that
  constraint has to be documented with the framework, not left to the deployer.
- **The HTTP server is one thread.** Fine for a dev backend, not for anything with
  load. Accept-loop plus a worker pool is the obvious next step and it is
  independent of everything else in this document.
- **Callback composition.** `call_async` is callback-shaped, so two dependent
  requests are a hand-written state machine at the call site. The fix is not new
  language syntax — it is the resource-signal layer in §5.5, which turns a chain
  of callbacks into a chain of derived signals.

### 1.10 Platform reach

Native GUI is macOS plus an X11 Linux host. No Windows. iOS device signing is out
of scope. That's fine for now; it should be written down as a scope decision, not
discovered by a contributor.

---

# Part 2 — Zeus: real drawbacks

### 2.1 Manual subtree rebuilds are React's problem, reintroduced

Components that call `drop_children(n.id)` and rebuild everything in an
`on_restyle` handler are doing coarse-grained diffing by hand. That throws away
the entire advantage of a retained tree + signals: it loses focus, loses scroll
position, loses animation state, and allocates a full subtree on every update.

The fix is the control-flow primitive set every fine-grained framework has, and
Zeus must have all four before any list-heavy app is written:

- `For(items, key, build)` — keyed, diffs by key, **moves** nodes rather than
  rebuilding them. Key is required, not optional.
- `Index(items, build)` — positional; for when items are stable and keys are noise.
- `If(cond, then, else)` / `Show` — toggles a subtree without touching siblings.
- `Switch` / `Match(signal, arms)` — one arm mounted at a time.

Once these exist, `drop_children` becomes internal-only and every manual rebuild
site in the widget kit is deleted.

### 2.2 No error boundary — a trap kills the app

Loam traps on overflow and out-of-bounds. In a CLI that's correct. In a framework,
a bad index in one card must not take down the whole application. There is no
recovery mechanism today, and with trapping semantics and no unwinding there
cannot be one without runtime support.

**Design:** `zeus.Boundary(fallback, build)`.
- On entry, record an arena mark and `setjmp` in `loam_rt`.
- A trap inside longjmps back, releases the node arena to the mark (bump allocator
  makes this trivial), drops any closures interned since the mark, and builds
  `fallback` with the error.
- Documented limits: it recovers the UI to a known-good state; it does **not**
  undo C-seam side effects, and it is not general exception handling. Boundaries
  are route-level and opt-in.

Without this, `error.loam` in §5 is undeliverable.

### 2.3 Accessibility is the strongest legitimate attack on canvas-only UI

Roles and a focus ring exist, but on web there is nothing for a screen reader to
read — a canvas is one opaque element. Flutter Web hit exactly this wall. In many
markets this is a legal blocker, not a nice-to-have.

This is solvable **without breaking "no DOM for UI"**: maintain an offscreen,
`aria-hidden="false"`, visually-hidden DOM mirror of the semantic tree — roles,
labels, values, focus order. It is never painted, never laid out, never styled,
and never the thing the user sees. The UI stays canvas. Say this in the docs
explicitly, because it looks like a contradiction and isn't. Native hosts wire the
same semantic tree to `NSAccessibility` / UIAccessibility / `AccessibilityNodeInfo`.

### 2.4 No devtools

React's adoption was partly DevTools. Zeus can do better cheaply, because the
whole UI state is one arena: ship a Zeus app that reads the arena of the app under
inspection and shows the node tree, computed layout boxes, the signal graph with
current values, and a paint overlay that flashes repainted regions. This is a
weekend project with the leverage of a marketing department.

### 2.5 No hot reload

Edit-compile-run through C99 + `cc` is seconds. For UI work that's the difference
between enjoyable and not. Component-level reload is feasible: keep the signal
arena across reload, keyed by a stable component path, rebuild the tree, restore
signal values. Not free, but the payoff is daily.

### 2.6 Everything is `int` (see Part 3)

Node fields, geometry, colors, and signals all sit in 64-bit slots. Layout is a
pointer-chasing tree walk over that arena, so its speed is almost entirely a
function of how much of the arena fits in L2. This is the single biggest
mechanical performance item in the engine.

### 2.7 wasm binary size and splitting

The JS analogue of bundle size is the wasm binary, and it's worse: JS chunking is
mature, wasm splitting is not. DCE is on the roadmap; per-route splitting needs a
real decision (§5.9). Do not pretend this is solved.

### 2.8 No animation system

No timeline, no spring, no transition primitive, no `transition` on mount/unmount.
Every serious UI framework has this and it interacts with the control-flow
primitives in 2.1 (an exiting node must stay alive through its exit animation).
Design them together or retrofit painfully later.

### 2.9 Layout engine limits

Rows, columns, gap, padding, grow. No grid, no wrap, no baseline alignment, no
intrinsic sizing (`fit-content` / `min-content`). Fine for dashboards; not fine for
the first person who wants a responsive marketing page.

---

# Part 3 — The numeric type system

## 3.1 Why the change is right

Today `int` is `int64_t` and `float` is 64-bit. For this stack specifically:

- **Zeus arena.** Every `Node` field is 8 bytes. Layout is a tree walk whose cost
  is dominated by cache misses. Halving the node size roughly halves the working
  set of the hot loop. Nothing else in the engine offers that ratio for that
  little work.
- **Coordinates don't need 64 bits.** `f32` is exact for integers up to 2^24
  (16.7 million). Every screen coordinate, dp value, and layout box is orders of
  magnitude inside that. `f64` for a padding value is pure waste.
- **SIMD.** NEON and wasm `simd128` are 128-bit. That's 4 `f32` lanes versus 2
  `f64`. For the GreenInfer row sweep and anything seed-x does on CPU, `f32` is a
  straight 2× on both throughput *and* memory bandwidth. Since the entire premise
  of that work is energy per inference, and energy is dominated by memory traffic,
  this is the highest-leverage single change available.
- **wasm32.** `i64` values cost more to move and operate on in a 32-bit address
  space than `i32`.
- **Handles.** `Node { id: int }` and `Signal { id: int }` are arena indices. They
  will never exceed 2^31. They are 8 bytes each today for no reason.

## 3.2 What this contradicts

`docs/downsides.md` lists "width integers" under **do not add**. This document
deliberately reverses that. Be clear-eyed about what it costs:

- Integer promotion and conversion rules must be specified and implemented in
  `typecheck.c`.
- `codegen_c.c` must emit explicit casts everywhere, because C's own integer
  promotions will otherwise silently change semantics on sub-`int` widths.
- The overflow-trap machinery must be replicated per width.
- Every `compile_fail` fixture about arithmetic needs a sized sibling.
- LSP hover, the tree-sitter grammar, and every doc example change.

That is a real multi-week cost. It is worth it because the alternative is baking a
wasteful representation into the arena, the draw list, the wire format, and every
app anyone writes — and that becomes unfixable once other people depend on it.
**Do it now or never.**

## 3.3 The type set

| Type | C | Notes |
|---|---|---|
| `i8` `i16` `i32` `i64` | `int8_t` … `int64_t` | signed, trap on overflow |
| `u8` `u16` `u32` `u64` | `uint8_t` … `uint64_t` | unsigned, trap on overflow **and underflow** |
| `f32` `f64` | `float` `double` | |
| `int` | alias for `i32` | the default integer |
| `float` | alias for `f32` | the default float |
| `bool` `string` `[]T` `[N]T` `Box<T>` `&T` `&mut T` `fn` | unchanged | |

Two aliases, not two extra types. `int` and `float` stay as the names you write
99% of the time; they simply mean 32 bits now. Existing source that says
`count: int` keeps compiling and gets faster.

## 3.4 Rules

**Untyped constants.** An integer or float literal is *untyped* until context
gives it a type (Odin/Go-shaped). `zeus.Box(padding = 16)` works whether `padding`
is `f32`, `i32` or `u8`. An unconstrained literal materializes as `i32` / `f32`.
A literal that doesn't fit its target is a compile error **at the literal**, with
the target type named.

**No implicit mixed-width arithmetic.** `a: i32 + b: i64` is an error, not a
promotion. C's promotion rules are the source of a large fraction of real-world
integer bugs; do not inherit them. The error message must name both types and
suggest the conversion.

**Conversions are call-shaped.** `i64(x)`, `f32(x)`, `u8(x)`. No `as` keyword —
you've ruled out Rust idioms, and a call form composes better with UFCS anyway.

**Narrowing traps.** `u8(300)` traps at runtime, consistent with the rest of the
language. Opt out explicitly: `wrapping_u8(x)`, `saturating_u8(x)`.

**Unsigned underflow traps.** `u32(5) - u32(10)` traps. It does not wrap to four
billion. This is a correctness feature and it is the reason unsigned types are
safe to hand to users at all.

## 3.5 Codegen rules (these are where it goes wrong quietly)

1. **Sub-`int` arithmetic** on `i8`/`i16`/`u8`/`u16` must be evaluated in
   `int32_t`/`uint32_t` and range-checked before the store. C promotes these to
   `int` anyway; be explicit so the check matches the declared type.
2. **Overflow checks** use `__builtin_add_overflow` and friends where available,
   with a portable pre-check fallback. One check shape per width.
3. **Shifts** lower through the unsigned type (signed shift of a negative value is
   UB in C). A constant shift ≥ width is a compile error; a dynamic one traps.
4. **Float determinism.** Compile generated C with `-ffp-contract=off` and never
   evaluate `f32` arithmetic in `double`. FMA contraction changes results between
   hosts and compiler versions, which would make DRAW goldens non-reproducible and
   make any cross-host pixel comparison meaningless.
5. **`f32` literals** emit with an `f` suffix so the C compiler doesn't widen them.

## 3.6 What must stay 64-bit

Write these down before the migration, because each one is a real bug if missed:

| Thing | Type | Why |
|---|---|---|
| Wall-clock time, epoch millis | `i64` | i32 millis overflows in 24 days |
| Monotonic clock / frame timestamps | `i64` nanos | same |
| Animation accumulators | `f64` or `i64` micros | `t += dt` in `f32` at 60fps drifts visibly within hours |
| File sizes, mmap offsets | `i64` | 2 GB is not a limit you want |
| Hashes, IDs, checksums | `u64` | |
| Money / decimal | integer minor units, never float | |
| Byte counts over a network | `i64` | |

Everything else — coordinates, sizes, indices, counts, colors, handles — is 32-bit.

## 3.7 Zeus-side representation after the change

```
struct Rect  { x: f32, y: f32, w: f32, h: f32 }     // 16 bytes, was 32
struct Color { rgba: u32 }                           // 4 bytes, was 8
struct Node   { id: u32 }                            // 4 bytes, was 8
struct Signal { id: u32 }                            // 4 bytes, was 8
```

`f64` survives in exactly one place: the C seam. Cocoa's `CGFloat` and Canvas2D's
JS numbers are doubles, so `plat_fill` / `plat_glyphs` convert at the boundary and
nowhere else. State this as a rule — "f32 inside, f64 only at the seam" — so it
doesn't leak back in.

Expected effects, to be **measured, not assumed**: node arena roughly halved,
draw-list bytes per op roughly halved, wasm binary smaller, NEON kernels 2× lanes.
Land a benchmark in `make test` in the same phase so the claim is checkable.

## 3.8 Below 32 bits: which `Node` fields, and when

`i32`/`f32` is the default, not the floor. Some `Node` fields genuinely fit in 16
or 8 bits. But the decision is made **per field by what the field is**, never by
"its values look small today," because under trap-on-overflow a too-narrow field
is a crash rather than a wraparound.

### Keep `f32` — do not narrow

Computed geometry: `x`, `y`, `w`, `h`, scroll offsets, measured text advances.

- Subpixel positions need fractions. Integer geometry means no half-pixel borders
  and no smooth scrolling.
- These get multiplied by the DPI scale at paint time, so the stored value is not
  the painted value.
- Mixed widths inside the layout arrays kill any future SIMD pass over them.

This is the bulk of the hot data, and it stays 32-bit float.

### `u32` — do not narrow to `u16`

Arena handles (`id`, `parent`, `first_child`, `next_sibling`, `signal_id`) and
packed colors (`rgba`).

`u16` caps the arena at 65,535 nodes. A large gallery, an unvirtualized table, or
a deeply nested document can plausibly approach that, and a hard ceiling in an
arena index is exactly the kind of limit you cannot remove later without touching
every file. Colors are already exactly 32 bits packed; splitting them is a loss.

### `u16` — yes, with a stated maximum

`padding`, `gap`, `radius`, `border`, `font_size`, `min_w`/`min_h`, `flex_basis`.

**Store these unscaled, in dp, and apply the DPI scale at paint.** If you store
*scaled* values, `padding = 100` on a 3× display traps at 300 in a `u8`. Every
narrowed field needs a documented max in its `///` comment and a compile error at
the literal — `padding = 400: does not fit u16 field 'padding' (max 65535)` — not
a runtime trap in someone's app.

### `u8` — clearly right

`direction`, `align`, `justify`, `LOOK`, `SIZE`, `overflow`, state bits, and the
flag byte. Two to eight legal values each, and none of them will grow.

### Field ordering is what actually makes this pay

Narrowing saves nothing if narrow fields are scattered between `f32`s — the
compiler just inserts padding and the struct is the same size. Group by width,
widest first:

```yuga
struct NodeRec {
    // f32 block — geometry, hot in layout
    x: f32, y: f32, w: f32, h: f32,
    min_w: f32, min_h: f32, scroll: f32, grow: f32,

    // u32 block — handles and color
    parent: u32, first_child: u32, next_sibling: u32,
    text: u32, bg: u32, fg: u32, handler: u32,

    // u16 block — dp-space style values
    padding: u16, gap: u16, radius: u16, border: u16, font_size: u16,

    // u8 block — enums and flags
    direction: u8, align: u8, justify: u8, look: u8, size: u8, flags: u8,
}
```

### The honest arithmetic

For a ~26-field node:

| Representation | Approx. bytes/node |
|---|---|
| Today, everything 64-bit | ~208 |
| After the 32-bit flip (Phase 11) | ~94 |
| Plus `u16`/`u8` narrowing, grouped | ~80 |

So narrowing is a further **10–15% on top of a ~55% win**. Real, but second-order.
Do not spend a week on it before the 32-bit flip and its benchmark land.

### The bigger remaining win is struct-of-arrays, not narrower fields

Split the arena into parallel arrays — `xs[]`, `ys[]`, `ws[]`, `hs[]`, `parent[]`,
`flags[]` — instead of one array of records. A layout pass that touches only
geometry then never pulls colors, text handles, or handler ids into cache. This is
where Servo and Blink both ended up, and narrow types become free inside it: each
array is homogeneous, so there is zero padding and the `u8` arrays are genuinely
one byte per node.

SoA costs you readable field access (`n.x` becomes `arena.xs[n.id]`), which UFCS
can hide behind accessor fns. It is a bigger change than narrowing and a bigger
payoff.

### Order of work

1. Land the 32-bit flip and the benchmark (Phases 10–11).
2. Reorder fields by width. Free, no semantic change, measure it.
3. Try SoA for the geometry arrays only. Measure.
4. Narrow to `u16`/`u8` last, inside whichever layout won.

Narrowing before you can measure is how you spend a week to find out it was 3%.

## 3.9 Migration order

1. Add the sized types alongside the current ones; `int` still means `i64`.
   Everything keeps compiling. Land conversions, literals, trap machinery, tests.
2. Flip `int` → `i32` and `float` → `f32` behind `--int64-compat`, fix the std
   library and examples, keep the flag for one release.
3. Convert Zeus geometry, colors, and handles; regenerate DRAW goldens once, as a
   single reviewable commit.
4. Convert the `#[proto]` wire mapping (`int` → `int32`/`sint32` in proto terms;
   explicit `i64` fields → `int64`).
5. Delete `--int64-compat`.

---

# Part 4 — Take React's structure, reject React's implementation

**The governing principle for Parts 4 and 5:** borrow the *shape* — the file
layout, the naming, the mental model, the API surface a developer touches — and
build none of the machinery underneath it. A React developer should open a Zeus
app and immediately know where things are. They should also find that every
concept they learned to work *around* in React is simply absent.

The distinction matters because React's structures were mostly right and its
implementation was mostly a series of forced moves. Components as functions,
props, composition, context, error boundaries, a loading state per subtree — all
good ideas. The VDOM, fiber, hooks-as-positional-slots, dependency arrays,
`memo`, hydration, RSC — all consequences of rendering to a DOM from a JS runtime
that had no way to observe state changes. You have signals and a retained arena.
None of those consequences follow.

## 4.1 The split, concept by concept

| Structure borrowed | React's implementation — **rejected** | Zeus implementation | Status |
|---|---|---|---|
| Component = function returning UI | VDOM element tree, diffed each render | Builds nodes into the arena once | **have it** |
| Props | Reconciler compares prop objects | Plain fn params with defaults | **have it** |
| State in a component | `useState` — positional hook slot | `signal(v)` — a value you can create anywhere, conditionally, in a loop | **have it** |
| Derived state | `useMemo` + dependency array | Derived signal; the graph already knows its inputs | **have it** |
| Stable callbacks | `useCallback` + dependency array | Nothing. Fns aren't identity-compared because nothing re-renders | **have it** |
| Skipping needless work | `React.memo`, reconciliation bailouts | Nothing. Only the changed node updates | **have it** (finish §2.1) |
| Side effects | `useEffect` + deps + cleanup + timing rules | `effect(fn)` after commit; cycles reported by name | **to build** |
| Lists | `key` prop, runtime warning if missing | `For` requires a key **at the type level** — no key, no compile | **to build** |
| Context | Provider re-renders the whole subtree | Lookup walks the arena parent chain; reading subscribes to one signal | **to build** |
| Error boundaries | Class component + `componentDidCatch` | `Boundary` with arena mark/release (§2.2) | **to build** |
| Suspense / loading UI | Thrown promises, concurrent scheduler | `resource` signal with `LOADING/READY/ERROR` (§5.5) | **to build** |
| Refs / imperative escape | `useRef`, forwardRef, ref merging | `Node` is already a handle. There is nothing to escape to | **free** |
| Portals | Separate DOM subtree | Overlay layer in the scene — no tree surgery | **have it** |
| Fragments | `<></>` because JSX needs one root | Nothing. A fn can add several children | **free** |
| Dev inspection | React DevTools browser extension | Devtools app reading the arena (§2.4) | **to build** |

## 4.2 Machinery that does not exist and never will

Not "we'll add it later" — structurally absent:

- **No virtual DOM, no diff, no reconciler.** There is one tree and it is the real
  one.
- **No fiber, no scheduler, no lanes, no concurrent mode, no `startTransition`.**
  One UI thread, synchronous commit.
- **No rules of hooks.** Signals are values, not positions in a call-order array.
  Nothing breaks if you create one inside an `if`.
- **No dependency arrays anywhere in the API.** Not one, in any function.
- **No re-render concept.** Since there is no re-render, there is no optimization
  API for avoiding it, so there is no way to use the optimization API wrongly.
- **No StrictMode double-invoke**, because nothing needs to be pure-checked.
- **No JSX, no Babel, no bundler, no transform step.** Components are fns with
  trailing blocks, compiled by `yugac`.

Those last two bullets are the actual pitch, and they're worth putting in the
README verbatim.

## 4.3 The one place Zeus currently reintroduces a React bug

Handlers capture by copy and the env is memcpy'd at intern time, so a captured
non-handle value is frozen — this is the stale-closure bug, recreated in a
language that should be immune to it (§1.3). It must be fixed before any of the
above is worth claiming. A framework that says "no dependency arrays" and then has
stale captures has just moved the bug somewhere with no lint rule to catch it.

---

# Part 5 — Take Next.js's structure, reject Next.js's implementation

Same principle as Part 4. Next.js is, underneath the marketing, **a convention for
organizing an app around a routing tree with per-route data, per-route UI states,
and per-route metadata.** That convention is good and has nothing to do with HTML.
Everything Next.js built to make it work in a browser is something you don't need.

## 5.0 The split, feature by feature

| Structure borrowed | Next.js implementation — **rejected** | Zeus implementation |
|---|---|---|
| `routes/` directory is the route table | Webpack/Turbopack route manifest, JS chunks per route | `yugac` scans the dir and generates a route table at compile time |
| `[slug]`, `[...rest]`, `(group)` naming | String-parsed at runtime | Same names; params are **typed** and validated before mount |
| `layout.loam` nesting, persistent across navigation | RSC payload diffing to preserve layout state | Retained tree — shared layout nodes are simply never touched |
| `loading.loam` | Suspense + thrown promises + streaming HTML | `resource` signal in `LOADING` state (§5.5) |
| `error.loam` | Error boundary class component | `Boundary` with arena mark/release (§2.2) |
| `not-found.loam` | Same | Route match failure or param validation failure |
| Server functions | `"use server"` directive, RSC serialization, action IDs in the bundle | `#[server]` fn split into a gRPC method + typed client stub (§5.3) |
| Data loading per route | `fetch` with a patched global cache, React cache, `revalidate` tags | gRPC call + cache keyed on method + message (§5.5, §5.6) |
| Third-party API calls | `fetch` anywhere, including client components | Server-side only, re-exposed as a `#[server]` fn with a proto return (§5.6) |
| Metadata API | `generateMetadata`, injected into streamed HTML | `meta()` fn per route → per-route `<head>` at build time (§5.7) |
| Middleware | Edge runtime, request interception | gRPC server interceptors (§5.9) |
| Static assets | `next/image`, runtime optimization, config | Build-time asset pipeline, typed handles (§5.8) |
| `next build` | One web target | One command, five targets (§5.11) |

And the things with no counterpart at all, because they only exist to serve a DOM:

- **No RSC.** Stream data, not serialized UI.
- **No `"use client"` / `"use server"` boundary.** Client code is client code;
  `#[server]` bodies are excluded at codegen, which is enforcement rather than a
  directive.
- **No hydration**, no SSR, no streaming HTML, no partial prerendering (§5.7).
- **No edge runtime**, no two JS runtimes to keep compatible.
- **No bundler, no chunk graph, no tree-shaking config.** One compiler.

## 5.1 File-system routing

The compiler scans `routes/` and generates a route table. Because there's no URL
bar on native, the router is an abstraction over four different navigation models —
and that unification is a genuine advantage, not a workaround.

```
routes/page.loam                  →  /
routes/blog/page.loam             →  /blog
routes/blog/[slug]/page.loam      →  /blog/:slug
routes/docs/[...path]/page.loam   →  /docs/*
routes/(marketing)/about/page.loam →  /about     (group, not in the path)
```

| Host | Back | URL | Deep link |
|---|---|---|---|
| wasm | browser back → History API in `loader.js` | real | real |
| macOS | ⌘[ / nav gesture | in-memory stack | custom URL scheme |
| iOS | edge swipe / nav bar | in-memory stack | Universal Links |
| Android | system back button | in-memory stack | App Links |

One API: `router.push(path)`, `router.back()`, `router.params()` returns a signal.
Route params are typed — `[slug]` becomes a `string` param, `[id:i32]` an `i32`
param validated before the route mounts, so a bad param renders `not-found`
instead of trapping.

## 5.2 Nested layouts

`layout.loam` in a directory wraps every route beneath it. On navigation, shared
layouts **are not rebuilt** — the retained tree does this natively and better than
React, where preserving layout state needed an entire RSC redesign. Sidebar scroll
position, an open accordion, a playing audio element: all survive navigation for
free.

Special files per directory, matching Next.js conventions so the knowledge transfers:

| File | Role |
|---|---|
| `page.loam` | the route's UI |
| `layout.loam` | persistent wrapper for this segment and below |
| `loading.loam` | shown while this segment's loader is pending |
| `error.loam` | `Boundary` fallback for this segment (§2.2) |
| `not-found.loam` | unmatched child path or failed param validation |
| `loader.loam` | data this segment needs, started in parallel with its parents |

## 5.3 Server functions — where Zeus beats Next.js

Next.js's server actions exist to cross a language/serialization boundary. You
don't have that boundary: it's Loam on both sides.

```yuga
// routes/blog/[slug]/loader.loam
#[server]
fn get_post(slug: string) -> Post {
    return db.query_post(slug)     // native-only code, never in the wasm binary
}
```

The compiler splits this into a gRPC method on the server binary plus a typed
client stub in the client binary. One definition, no `.proto` file to keep in
sync, no tRPC, no codegen step, and a type error if the shapes drift — because
they can't drift, there's one shape.

Rules that make it safe:

- A `#[server]` fn body is **excluded from every client target**. Secrets and DB
  handles cannot leak into the wasm binary; this is enforced at codegen, not by
  convention.
- Arguments and returns must be `#[proto]`-representable. That's checked at
  compile time with a message naming the offending field.
- Calling a `#[server]` fn from client code compiles to the RPC. Calling it from
  server code is a direct call.

## 5.4 What replaces RSC

Do not build React Server Components. RSC exists to stream *serialized UI* to a
JS runtime that will reconcile it with the DOM. You have neither a DOM nor a
reason to serialize UI.

**Stream data, render on the client.** Components stay in the wasm/native binary.
Server functions return typed data. This is simpler, has no "use client" /
"use server" boundary to reason about, and has no serialization boundary bugs.

## 5.5 Data loading: resource signals

This is a thin layer over the async that already exists (`call_async`,
`std:thread`, `Chan<T>`). It adds no language syntax — it moves callback
composition out of call sites and into the signal graph, where dependency tracking
is already solved:

```yuga
let post = zeus.resource(fn() => get_post(router.param("slug")))

zeus.Match(post.state, {
    LOADING: fn() => Skeleton(),
    ERROR:   fn() => ErrorCard(post.err()),
    READY:   fn() => Article(post.val()),
})
```

- `resource` owns a `Signal` holding `LOADING | READY | ERROR`. It re-fires when
  the signals its fetcher reads change. No dependency array — the signal graph
  already knows.
- Route loaders are collected and **started in parallel** when navigation begins,
  before the route paints. Fetch waterfalls are structurally impossible, which is
  the thing Remix got right and Next.js spent years fixing.
- Cache keyed by route + params, with `revalidate = <seconds>` and an explicit
  `invalidate()`. Backed by the existing KV layer: file on native, memory on wasm.
- **Offline works on native for free** — a file-backed cache means a Zeus app has
  an offline story Next.js structurally cannot have.

## 5.6 Wire protocol: gRPC by rule, JSON/REST as an interop escape hatch

Two different things, and the distinction is the whole rule:

**First-party traffic — Zeus client ↔ your Loam server — is gRPC, always.**
`#[proto]` messages over gRPC-Web in the browser, h2c natively. There is no
JSON option for this path, no "just this one endpoint," no config flag that turns
it on. One calling convention for your own backend.

**Third-party traffic gets JSON and REST, because you don't control it.** Stripe,
an LLM provider, a maps API, a webhook from GitHub — none of them will speak your
proto. Refusing JSON here doesn't buy purity, it just means you can't integrate.
So `std:json` and a REST client belong in the tree as a first-class, documented
capability.

### Where the boundary sits

```
Zeus client  ──gRPC──►  Your Loam server  ──REST/JSON──►  Third-party API
                                          ◄──REST/JSON──  Inbound webhook
```

- The client never sees JSON. A third-party call happens on the server and is
  re-exposed as a `#[server]` fn with a `#[proto]` return type, so the Zeus side
  is unchanged whether the data came from your database or from Stripe.
- **Exception, stated honestly:** a wasm client sometimes must call a public API
  directly (no server deployed, or CORS actually permits it). Allowed, but then any
  credential is in the binary and visible. Default to proxying through your server;
  make the direct path something you choose, not something you fall into.
- **Inbound webhooks are the case you cannot design away.** Stripe and GitHub will
  POST JSON to a URL. `http.serve_json(path, handler)` has to exist, or the
  framework can't receive a payment confirmation.

### `std:json` — codec, separate from transport

Keep the codec in its own module. `std:json` parses and emits; `std:http` moves
bytes. They compose, and the codec is testable without a socket.

Deserializing into a struct is the real design problem, because Loam has no
traits, no macros, no comptime, and no reflection. **The answer already exists in
the tree: do exactly what `#[proto]` does.** The compiler generates encode/decode
for an attributed struct. Same mechanism, second format.

```yuga
#[json]
struct Charge {
    #[json(name = "id")]              id: string,
    #[json(name = "amount_cents")]    amount: i64,
    #[json(string)]                   account_id: i64,   // API sends it quoted
    #[json(default = 0)]              retries: i32,
    #[json(skip)]                     computed: f32,
}

let r: Res<Charge> = json.decode<Charge>(body)
```

Decisions that have to be made explicitly, each of which is a bug if left implicit:

| Case | Rule |
|---|---|
| Malformed input | Returns `Res<T>` (§1.1). **Never traps** — a bad response from someone else's server must not kill your process |
| Missing field | `#[json(default = …)]`, else decode error naming the field and its path |
| Unknown field | Ignored by default; `#[json(strict)]` on the struct makes it an error |
| `null` | Same path as missing |
| Field naming | `#[json(name = "…")]` per field; APIs use camelCase, snake_case and worse, and guessing is wrong |
| Big integers sent as strings | `#[json(string)]` — very common (Twitter/Stripe-style IDs) |
| Duplicate keys | Last one wins, matching every other parser |

### Numbers — the part that interacts with Part 3

JSON numbers are IEEE doubles with no declared width. With `i32` now the default
integer (§3.3), this is a live hazard:

- A JSON value that doesn't fit the target field is a **decode error**, not a
  silent truncation and not a trap. `3000000000` into an `i32` field fails with the
  field path in the message.
- A JSON number with a fractional part decoding into an integer field is an error.
- Integers beyond 2^53 lose precision *in the JSON itself*, before you ever see
  them. That's why `#[json(string)]` exists; document it next to the `i64` rule.
- Decoding into `f32` is allowed and rounds; decoding into `f64` is exact.

### Untrusted input

This is a parser on bytes from someone else's server, so treat it that way:

- Hard nesting-depth limit (default ~64). Deeply nested arrays are the standard
  way to blow a recursive parser's stack.
- Maximum document size, caller-settable.
- No trapping paths anywhere in decode — every failure is a `Res`.
- Written in Loam, not linked from a C library. JSON is library logic, so the
  C-boundary rule applies without an exception here.

### Dynamic JSON

Sometimes the schema isn't knowable. Provide a `JsonValue` tree — a struct with a
`kind` tag plus accessors returning `Res<T>` (`v.get("a").get("b").as_i32()`).
Without algebraic enums this is the honest shape. Keep it as the fallback, not the
default: typed `#[json]` structs are what people should reach for.

### Streaming

`std:http` is unary today. Live first-party data — progress, notifications, a
tailing log — should use **server-streaming gRPC**, not WebSockets or SSE carrying
JSON. A stream maps cleanly onto a `Signal` that updates per message, which is the
shape the resource layer already wants. Third-party streaming formats (SSE from an
LLM API, for instance) are consumed on the server and re-published as a gRPC
stream to the client.

## 5.7 No SSR. Metadata for SEO instead.

**Decision: Zeus does not do SSR, in any form.** SSR exists to send a server-built
DOM to a client that will adopt it. There is no DOM, so there is nothing to send
and nothing to adopt. Every route renders on the client, on every host, always.

What that buys you, permanently:

- No hydration mismatch — the entire bug class does not exist.
- No `"use client"` / `"use server"` boundary to reason about.
- No two rendering paths to keep in agreement.
- One mental model: the tree is built by the client, data comes over gRPC.

What it costs is exactly one thing, and it is the only thing to solve: **a crawler
that fetches your URL gets an empty canvas.**

### What metadata can and cannot buy you

Be precise about this, because it determines how much effort is worth spending.

| Works with metadata alone | Does not, ever |
|---|---|
| Search result title and description | Indexing of body content painted on canvas |
| Link previews on WhatsApp, Slack, X, LinkedIn (OG tags) | Ranking on text the crawler can't read |
| Canonical URLs, no duplicate-content penalty | Rich snippets derived from page text |
| Correct per-route entries in the index | |
| `sitemap.xml` / `robots.txt` discovery | |

For an application — dashboards, tools, internal software — metadata is 100% of
what you need, because nobody searches for the contents of a logged-in dashboard.
For a content site whose value *is* its indexable text, canvas-only is the wrong
tool and you should know that before starting, not after.

### Per-route metadata

```yuga
// routes/blog/[slug]/page.loam

fn meta(p: Params) -> Meta {
    return Meta {
        title:       "Post — " + p.get("slug"),
        description: "…",
        canonical:   "/blog/" + p.get("slug"),
        og_image:    assets.og_default,
        robots:      ROBOTS.Index,
    }
}
```

- `meta()` is a plain fn on the route, same as `page` and `loader`.
- On web it fills the HTML shell's `<head>`. On macOS/iOS/Android the same struct
  fills the window title and the app's share sheet. One definition, five hosts.
- Static routes get their metadata evaluated at build time.
- Dynamic routes (`[slug]`) need values a build can't know. Two honest options:
  either enumerate them at build time from a `paths()` fn (the SSG list approach),
  or have the server fill the head for crawler user-agents only. Prefer the first;
  it needs no server rendering path at all.

### The shell is a head, not a page

The critical mechanic: **emit one HTML file per route at build time**, not a single
`index.html` for everything. If every URL serves the same file, every URL has the
same title and the index collapses to one entry.

```
build/web/
  index.html            <head> for /            + canvas + wasm loader
  blog/index.html       <head> for /blog        + the same canvas + loader
  blog/hello/index.html <head> for /blog/hello  + the same canvas + loader
  sitemap.xml
  robots.txt
  app.wasm
```

Each file is `<head>` plus an empty `<body>` holding the canvas element and the
loader script. Zero markup for UI. HTML is being used here as a *document header
format*, which is what it was for originally — it is not a UI runtime, and no Zeus
component ever produces an element.

### Two things to rule on explicitly

- **JSON-LD.** Structured data (`application/ld+json`) is the standard way to get
  rich results. It is JSON, which collides with §5.6. The rule that resolves it:
  §5.6 governs **application data transport**; JSON-LD is a crawler annotation in a
  document head and never carries data into or out of your app. Allow it, scoped to
  the shell generator only. If you want zero JSON in the tree at all, microdata
  attributes are the alternative and are strictly worse-supported — that's the
  trade.
- **Prerendered first paint.** Serializing a build-time draw list and replaying it
  on the canvas before wasm boots would cut time-to-first-paint. It is *not* SSR —
  it ships pixels, not a DOM — but it is adjacent enough to be scope creep, and it
  depends on in-tree font metrics (§1.4) which don't exist yet. **Deferred, not
  planned.** Ship a painted skeleton from the loader instead; it costs a day.

## 5.8 Assets

`assets/` is compiled, not copied. Images are decoded and resized at build time
into the format each host wants; the build emits a typed handle per asset:

```yuga
zeus.Image(assets.hero, width = 320)
```

No runtime path strings, so no 404s, no `next/image` config, and a missing file is
a compile error. Fonts are assets too, which fits §1.4 exactly.

## 5.9 Middleware, metadata, API surface

- **Middleware** = gRPC server interceptors (auth, logging, rate limit). Not edge —
  you don't have an edge runtime and shouldn't pretend to.
- **Metadata** = a `meta()` fn per route (§5.7) feeding the HTML `<head>` on web and
  the window/app title on native.
- **API surface** = `#[proto]` methods on `std:http`. There are no "API routes" in
  the Next.js sense, because there are no URL-shaped JSON endpoints to define —
  a service method *is* the API (§5.6).

## 5.10 Code splitting — decide deliberately

JS chunking is mature; wasm splitting isn't. Options in increasing order of effort:

1. **One binary + DCE.** Already on the roadmap. For an app-shaped product a
   single well-DCE'd wasm binary is likely fine. Measure it before doing anything
   harder.
2. **Split by route group.** Each top-level `routes/(group)/` becomes its own wasm
   module sharing the core; the router instantiates on demand. Needs a stable
   ABI between modules.
3. **Lazy data, eager code.** Ship all code, lazy-load only assets and loader data.
   The pragmatic 90% answer.

Recommendation: measure (1), ship (3), only build (2) if a real app demands it.

## 5.11 One build, five artifacts

```
zeus build
  → build/web/      app.wasm + loader.js + prerendered scenes + HTML shell
  → build/macos/    App.app
  → build/ios/      App.app
  → build/android/  app.apk
  → build/server/   server binary (all #[server] fns + RPC surface)
```

`next build` gives you a web app. `zeus build` gives you five targets from one
source tree. That sentence is the entire pitch — make sure it's literally true
before saying it.

---

# Part 6 — Repository layout

## 6.1 Root: Zeus out of `packages/`

Zeus is a framework, not a package inside the compiler repo. Lifting it out makes
the boundary between language and framework enforceable rather than aspirational.

```
yuga/
  yuga/                     the language
    src/                    yugac (C11): lexer, parser, sema, ir, codegen_c
    std/                    core std only: fmt, net, sys, thread, math, str, time
    runtime/                loam_rt — the ONE C runtime for the ecosystem
    tests/                  compile_pass / compile_fail / golden

  yuga-lsp/                 diagnostics, hover, go-to-def, completion, tokens
  yugafmt/                  formatter (§1.7)

  zeus/                     the framework
    core/                   arena, geometry, layout, scene, input, signals
    text/                   font tables, shaping, line break, metrics (§1.4)
    ui/                     widget kit + design system
    router/                 route table, history, params, navigation
    server/                 #[server] splitting, loaders, cache, revalidation
    cli/                    zeus new / dev / build / test / inspect
    devtools/               the inspector, itself a Zeus app (§2.4)
    hosts/
      desktop/              mac.m, linux.c
      ios/                  ios.m
      android/              android.c (JNI)
      web/                  loader.js, wasm.c, Canvas2D, a11y mirror
    docs/

  http/                     std:http — gRPC-Web / h2c, #[proto]; REST client and
                            #[json] codec for third-party interop (§5.6)
  maya/                     3D/2D engine

  tooling/
    tree-sitter-yuga/
    editors/                Zed, VS Code / Cursor

  examples/
    language/
    zeus/                   gallery, dashboard, counter, greeninfer

  docs/                     yuga.md, boundary.md, this file
  bin/                      yugac, yuga-lsp, yugafmt, zeus
  install.sh  run.sh  Makefile
```

Consequences worth noting:

- `import "std:zeus"` now resolves outside the compiler tree. Module resolution
  needs a real search-path notion (`LOAM_PATH`, or a workspace manifest at the
  root) instead of "`std/` next to the compiler." Do this properly once; it's the
  same machinery `import "pkg:name"` (§1.8) needs.
- The compiler's `make test` no longer depends on Zeus. Zeus gets its own test
  suite. That separation is the point.
- `zeus/text/` sits inside Zeus for now. If a non-UI consumer needs it later,
  promote it to a top-level `text/`.

## 6.2 What a Zeus app looks like

This is the Next.js-shaped part — the structure the framework imposes on users:

```
myapp/
  zeus.toml               name, targets, routes dir, theme, revalidate defaults

  routes/
    layout.loam           root shell: nav, theme provider
    page.loam             /
    loading.loam
    error.loam
    not-found.loam

    blog/
      layout.loam
      page.loam           /blog
      loader.loam         list query
      [slug]/
        page.loam         /blog/:slug
        loader.loam       #[server] fn get_post(slug)
        error.loam

    (marketing)/          group — not part of the URL
      about/page.loam     /about
      pricing/page.loam   /pricing

  components/             shared components, no routing knowledge
  server/                 #[server] fns and #[proto] contracts
    db.loam
    api.loam
  assets/                 images, fonts, icons — compiled, typed
  public/                 web shell only: favicon, robots.txt
  theme.loam              tokens: palette, spacing, type scale
  tests/                  #[test] fns

  build/
    web/  macos/  ios/  android/  server/
```

`zeus new myapp` scaffolds exactly this. `zeus dev` runs the server and the
current host with hot reload. No four copies of `app.loam` per host — that was
Phase 1 of the old roadmap and this structure makes the regression impossible.

---

# Part 7 — Phases

Same convention as before: tick a phase only when its exit line runs green.
One phase at a time. The ordering is a dependency chain, not a preference.

**Position: Phases 9, 10, and 12 are green. Phase 11 is green on the two layout
steps that landed (grouping, narrowing); `f32` geometry and struct-of-arrays
are deferred and recorded in `docs/numeric-widths.md`. Phase 13 is green on
its engine and editing steps — grapheme clusters, the in-tree font
parser/metrics, and grapheme-aware caret/backspace/delete; shipping a font and
`plat_glyphs`/shaping are open, with the reason below. Next: Phase 14.**

### Phase 9 — Sized numeric types (additive)

**Status: done.** `int`/`float` were still 64-bit at the end of this phase.

Add `i8`…`i64`, `u8`…`u64`, `f32`, `f64` alongside today's types. Untyped
constants, explicit conversions, no implicit mixed-width arithmetic, trap on
narrow/overflow/unsigned-underflow, per-width `wrapping_*` / `saturating_*`.
Codegen rules from §3.5, including `-ffp-contract=off`.
**Exit:** `compile_pass` covers every width; `compile_fail` covers mixed-width
arithmetic, out-of-range literals, narrowing without a conversion, and constant
shift-overflow. `int`/`float` still mean 64-bit. Existing goldens byte-identical.
**Green.**

### Phase 10 — Flip the defaults

**Status: done.** The §3.6 64-bit list is audited in `docs/numeric-widths.md`,
with the before/after benchmark.

`int` = `i32`, `float` = `f32`, behind `--int64-compat`. Fix std, examples, and
the §3.6 list of things that must stay 64-bit.
**Exit:** whole tree builds with the flag off; the 64-bit list is documented and
audited; a published benchmark records arena size, layout time, and wasm binary
size before and after. **Green.**

### Phase 11 — Zeus representation and field layout

**Status: done for representation and field layout; two sub-steps deferred.**
`Rect`/`Color`/`Node`/`Signal` are 32-bit (`int` = `i32`); handles and packed
colors stay at the 32-bit default. Draw-list ops re-typed. Fields then grouped
by width and narrowed (`u16`/`u8`), each measured — `UiNode` 544 → 480 B,
`Draw` 64 → 56 B (per-step table in `docs/numeric-widths.md`).

Deferred, with the reason written down in `docs/numeric-widths.md`:

- **geometry as `f32`** — layout in this tree is integer-pixel throughout, so
  this is a behavior change (fractional geometry) with new DRAW goldens, not a
  width change;
- **struct-of-arrays geometry** — a phase-sized refactor of every `n.x` access
  site behind an accessor.

**Exit:** DRAW goldens regenerated in one reviewable commit (done, together with
the `border_w` `-1` sentinel fix); the Phase 10 benchmark shows the improvement;
each landed layout step has its own before/after number. **Green on grouping +
narrowing; open on `f32` geometry and SoA.**

### Phase 12 — `#line` and `#[test]`

**Status: done.** Generated C carries `#line` directives mapped to the `.loam`
source, so the C compiler's diagnostics name Loam lines and, with `-g`
(`LOAM_DEBUG=1`), so do debuggers, profilers, and sanitizers. `#[test]` fns are
collected and run by `yugac test`, which calls `std:test`'s `begin`/`ok`/
`summary`; the assertions (`test.assert`, `test.assert_eq_*`) are ordinary Loam
in `std/test.loam` built on the `panic(msg)` primitive. `make test` runs
`packages/yuga/tests/inlang/*.loam` and expects an all-pass exit.

`#line` directives in generated C; `#[test]` fns and `yugac test`.
**Exit:** a trap in `app.loam` reports a `.loam` line in lldb (the panic message
names it directly, and the `#line`-mapped debug line table references
`app.loam` under `-g`); `make test` runs in-language tests. **Green.**

### Phase 13 — Text in-tree

**Status: green on grapheme clusters, the font parser/metrics and glyph runs,
grapheme-aware editing, and external-font layout measurement on all four hosts
(the web path via an async fetch bridge); open on shipping a font, the shaping
decision, and `plat_glyphs`.**

`std:unicode` implements UTF-8 and extended grapheme clusters (UAX #29) with the
GCB property tables in Loam: `next_grapheme` / `prev_grapheme` /
`grapheme_count`, including combining marks, Hangul L/V/T, regional-indicator
pairs, and emoji ZWJ sequences. `Input` caret movement, backspace, and delete
step by cluster — a Tamil akshara moves as one unit per press, which is the
second exit line below, now green.

`std:font` parses `head` / `maxp` / `hhea` / `hmtx` and `cmap` formats 4 and 12,
and exposes pure `measure` / `measure_wrap`. Text width and line breaking are
functions of (font bytes, size, string), so once the hosts call them,
`measure_text` is host-independent by construction. Tested against a generated
fixture (`packages/yuga/tests/fonts/tiny.ttf`, remade by
`make_tiny_font.py`).

`std/zeuscore/metrics.loam` binds an external font (`zeus.use_font(src)` reads a
file; `zeus.use_font_bytes` takes bytes the app holds) and routes layout
measurement through `std:font`, falling back to the host seam when unbound.
`zeus_plat.c` reads the file for desktop/iOS/Android; on wasm the loader fetches
the URL, the host copies the bytes back (`zeus_font_reserve` / `zeus_font_set`),
and the loader binds them — async, so layout keeps `measureText` until they land.
`std:font.glyph_run` produces the positioned glyph run a `plat_glyphs` draw will
consume; it is deliberately the same shape shaping will fill. The goldens lock
the load path: `draw_golden/golden_font_metrics` binds `tiny.ttf` and shows the
in-tree advance and wrap differing from the headless fallback.

The web bridge is compile-verified (wasm links, exports present) but not
browser-tested, and `plat_glyphs` has no host draw implementation yet.

Still open, and why — this is a staged landing, not a claim the phase is done:

- **`plat_glyphs` host draw** — `glyph_run` exists, but no host draws a run:
  Cocoa/iOS/Android can (`CTFontDrawGlyphs` / `Canvas.drawGlyphs`), Canvas2D
  cannot without a glyph atlas parsed from `glyf`. Naming `plat_text` →
  `plat_glyphs` is the interface step still to land.
- **shaping** — metrics and runs are unshaped (one glyph per cluster), so Indic
  reordering and ligatures wait on the port. §1.4 records the direction
  (**option 1**: port a minimal shaper; HarfBuzz only as a temporary exception);
  the port itself is deferred.
- **shipping a font** — an app still supplies its own font; there is no in-tree
  asset or compiler `@embed`, so `measure_text` is identical across hosts only
  for an app that binds the same font everywhere.

**Deferred — pick up later** (recorded here so it is not lost):

1. Design the shaper boundary and port a minimal shaper (§1.4, option 1).
2. `plat_glyphs(run)` on hosts — Cocoa first (`CTFontDrawGlyphs`), then the
   `plat_text` → `plat_glyphs` ABI. Canvas2D needs a `glyf`-derived glyph atlas.
3. A shipped or `@embed`-ed font so `measure_text` is identical without the app
   binding one. The web fetch bridge is compile-verified, not browser-tested.

Font table parsing, metrics, line breaking, grapheme clusters, shaping (with the
§1.4 option chosen and written down). `plat_glyphs` replaces `plat_text`.
**Exit:** `measure_text` is identical on macOS, wasm, iOS, and Android for the
same input (**open** — needs a shipped font and the §1.4 shaper port above);
caret movement through a Tamil string moves one grapheme per press (**green** —
`std:unicode` boundaries in `Input`).

### Phase 14 — Closures, boundaries, control flow
Refcounted closure envs owned by their Node. Stale-capture diagnostic.
`Boundary` with arena mark/release. `For` (keyed, required), `Index`, `If`,
`Switch`.
**Exit:** no `drop_children` outside `zeus/core`; a trap inside a `Boundary`
paints the fallback and the app keeps running; a keyed list reorder preserves
focus and scroll; no leak after 10k rebuild cycles.

### Phase 15 — Router
Route table generation, `layout`/`loading`/`error`/`not-found`, typed params,
history on all four hosts, deep links.
**Exit:** one app, one `routes/` tree, navigating on all four hosts with back
working natively on each; layout state survives navigation.

### Phase 16 — Server functions, loaders, and third-party interop
`#[server]` splitting, client-target exclusion enforced at codegen, resource
signals, parallel route loaders, cache with `revalidate`. Plus `std:json`:
`#[json]` structs generated by the same machinery as `#[proto]`, a REST client,
`http.serve_json` for inbound webhooks, and a `JsonValue` fallback.
**Exit:** a `#[server]` body is provably absent from the wasm binary; two loaders
in one route issue their requests concurrently; `zeus build` emits all five
artifacts. For JSON: a malformed document, a 70-deep nesting bomb, a missing
field, and an out-of-range integer each return a `Res` error naming the field path
— none of them trap; a third-party REST call is reachable from a `#[server]` fn
and unreachable from client code.

### Phase 17 — Error handling and NLL
`#[must_check]` + `Res<T>` across fallible std APIs. Liveness-based borrow ends.
**Exit:** ignoring an `http.call` result is a compile error; the NLL fixture set
passes; no existing program regresses.

### Phase 18 — Accessibility and web metadata
Semantic tree → a11y mirror on web, platform a11y on native. `meta()` per route,
one HTML file per route at build time, `sitemap.xml`, `robots.txt`, canonical
URLs, OG tags, optional JSON-LD scoped to the shell generator. No SSR, no
prerendered scenes.
**Exit:** VoiceOver reads the gallery on web and macOS; every route serves its own
`<head>` with its own title and canonical; a link preview on a chat app shows the
right title and image; `<body>` contains the canvas and nothing else.

### Phase 19 — Developer experience
`yugafmt`. Hot reload preserving the signal arena. Devtools inspector.
`import "pkg:name"` + vendor lockfile. Animation and transition primitives.
**Exit:** `zeus dev` reflects a component edit without losing state; the inspector
shows tree, layout boxes, and signal values live.

---

## What still will not change

- Not HTML/DOM, not UIKit/AppKit/Material widgets, not WebGPU. HTML appears only
  as a per-route `<head>` for crawlers and as an a11y mirror — never as UI, and no
  component ever emits an element.
- Not SSR, not SSG of markup, not prerendering. Every route renders on the client
  on every host. SEO is handled by metadata, not by rendering (§5.7).
- Not a `View` trait. A component is a `fn`. `Node` is a handle.
- **First-party traffic is gRPC, no exceptions.** Every Zeus client ↔ Loam server
  path is `#[proto]` over gRPC-Web / h2c. JSON and REST are supported for
  third-party APIs and inbound webhooks only, and they stop at the server binary
  (§5.6).
- Not a multi-threaded UI. `std:thread` and `call_async` exist and should be used;
  completion still returns to the one UI thread.
- Not RSC. Stream data, not serialized UI.
- Not traits, macros, comptime, or lifetime parameters. Sized numeric types are
  the one reversal in this document, and §3.2 states its price.


Native

Two ways:

```bash
# build then run
./bin/zeus build examples/zeus/myapp --targets macos
./examples/zeus/myapp/build/macos/app

# or one step (what the other examples' run.sh use)
./bin/yugac --target=native --run examples/zeus/myapp/app.loam
```

One gotcha: `zeus_plat.c:322` gates on `ZEUS_HEADLESS` —

```c
if (getenv("ZEUS_HEADLESS") || !plat_run) return 1;
```

so if that's exported (e.g. left over from `make test`) the binary starts, does nothing, and exits 0. `unset ZEUS_HEADLESS LOAM_HEADLESS MAYA_HEADLESS` first. `zeus build` already strips them at build time, but the check is at *runtime*.

## Wasm

```bash
./bin/zeus dev examples/zeus/myapp --port 5173
```

It's running now at **http://127.0.0.1:5173** — verified serving:

deno run --allow-read --allow-write packages/zeus/cli/zeus.ts routes examples/zeus/myapp
