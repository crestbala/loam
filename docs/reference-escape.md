# Reference escape without lifetimes

Loam has Rust-shaped ownership but no lifetime parameters, and it keeps it
that way ([downsides.md](downsides.md)). What lifetimes guarantee, "a
reference never outlives what it points to", must still hold. Today it does
not: borrowck tracks moves and shared vs exclusive borrows, but the only
escape check is `return &ident` in `src/sema/typecheck.c` (`AST_RETURN`).

The fix is **second-class references**: a reference may be a parameter, a
local, or a return value tied to its arguments. It may not be stored
anywhere that outlives its source. There is no new syntax.

Architecture: [loam.md](loam.md). Borrow checker: `src/sema/borrowck.c`.

## Probes (all compile and run today)

| Case | Program | Today |
|------|---------|-------|
| a | `fn pick(a: &int, b: &int) -> &int`; result of inner-scope locals used after the scope | dangling stack read |
| b | `struct Holder { r: &int }` holding `&x` past `x`'s scope | dangling stack read |
| c | `p = first(&v)`; `v.push(…)` ×1000; `*p` | prints `2043`, not `1` (read after realloc) |
| d | `return &*b` with `b` a local `Box` | prints garbage (read after free) |
| e | `p = &x` in an inner block, `*p` after it | dangling stack read |
| f | `p = first(&v)`; `v = []int {}`; `*p` | read after free |

Codebase impact, measured: no `.loam` file in the repo returns a reference
(except `compile_fail/return_local_ref.loam`), and no struct has a field of
reference type. None of these rules should break existing code.

## Rules

1. A function cannot return a reference rooted in its own locals, through any
   path: `&x`, `&x.f`, `&x[i]`, `&*box_local`.
2. A struct field cannot have reference type (`&T` / `&mut T`), including
   through generic arguments (`Holder<&int>`).
3. A borrow cannot be stored in a binding declared in an outer scope than the
   place it borrows.
4. A returned reference borrows from its arguments: while the result is live,
   every reference argument stays borrowed (shared or exclusive, matching the
   parameter).

## Phases

Done = the exit line, green with `nob test <filter>` for the touched area.
One phase at a time. Each phase adds `compile_fail` tests for the rejected
cases and `compile_pass` tests for the closest legal code, so the rule does not
reject too much.

### Phase 1: Return of a local, through any path (case d) — done

- [x] The escape check moved from `typecheck.c` (`return &ident` only) to
      borrowck. Each binding records whether it is a local, a parameter, or a
      global (`BindKind`). `place_frame_root` / `ref_frame_root` walk
      `&` / `.f` / `[i]` / `*` to the root. A reference parameter
      (`xs: &[]int`) or a global is fine; a local, a by-value parameter, a
      local `Box`, or a temporary is rejected.
- [x] A local reference binding remembers the local it points at
      (`Binding.ref_root`), so `let p = &x; return p` is caught. Assignments
      update the record and never clear it, so `if c { p = &x }` counts
      (conservative).
- [x] A call returning a reference borrows from its reference arguments
      (rule 4, applied to `return` only): `return id(&x)` is rejected.
- [x] `compile_fail`: `return_box_deref_ref`, `return_field_ref`,
      `return_index_ref`, `return_local_ref_via_let`,
      `return_value_param_ref`, `return_ref_call_of_local`,
      `return_ref_reassigned_local`.
- [x] `compile_pass`: `return_param_ref` (index, field, `pick`, through a
      `let`, chained call, `&**b` through `&Box`).
- **Exit:** probe d is rejected; `nob test compile_` (705) and
  `nob test examples` green; `loamc check` on all 499 entry programs gives no
  new rejection. **Green.**
- Not followed yet: `return if c { &x } else { a }` and a reference read out
  of `*pp` (`&&T`). Both are treated as safe until a program needs them.

### Phase 2: No reference-typed struct fields (case b)

- [ ] In typecheck struct declaration, reject a field whose type is `TY_PTR`.
      Error: `struct field 'r' cannot be a reference — store an owned value,
      Box, or index`.
- [ ] Reject struct instantiation with a `TY_PTR` type argument when the
      parameter is used as a field type (`Holder<&int>`).
- [ ] Check the C seam: FFI structs that mirror C pointers must keep working.
      If any use `&T` fields, give them a separate path (raw pointer type or
      `#[c]` exemption) rather than weakening the rule.
- [ ] `compile_fail`: `struct_ref_field.loam`, `struct_ref_generic_arg.loam`.
- **Exit:** probe b is rejected; the full set of std / zeus / packages still
  typechecks (`loamc check` on each package entry).

### Phase 3: No borrow stored into an outer-scope binding (case e)

- [ ] In borrowck, when assigning a borrow of place `P` to an existing
      binding `h` (`h = &x`, or `h = <expr yielding a borrow of x>`), reject
      if `h->depth < depth_of(root(P))`. `let` at the same depth stays legal.
- [ ] Same check for a borrow written through a field or index of an
      outer binding, once phase 2 makes this reachable only through `[]&T`.
      Decide here: reject `[]&T` element types like struct fields (preferred,
      for consistency) or depth-check the push.
- [ ] `compile_fail`: `borrow_escapes_scope.loam` (probe e),
      `borrow_escapes_loop.loam` (assignment inside `for`).
- [ ] `compile_pass`: reassigning an outer reference to another outer place
      inside a block (`p = &z2` where `z2` is at `p`'s depth).
- **Exit:** probe e is rejected; NLL tests in `compile_pass` stay green.

### Phase 4: A returned reference borrows its arguments (cases a, c, f)

Open decision: **tie to arguments** (default, below) vs **ban `-> &T`
outright**. Banning is simpler but removes accessors like `first(xs)` /
`get(map, k)`. Settle before starting.

- [ ] For a call `f(args…)` whose return type is `TY_PTR`, collect the places
      of every argument passed to a `&T` / `&mut T` parameter (explicit or
      auto-borrowed). The result binding gets those as its borrows: extend
      `Binding.borrow_of` from one place to a list.
- [ ] While the binding is live (NLL last use), those places are borrowed:
      `v.push`, `v = …`, `&mut v`, and moves of `v` are rejected.
- [ ] A `&mut T` result borrows exclusively; a `&T` result from `&mut`
      arguments borrows them shared unless the function returns `&mut`.
- [ ] Combine with phase 3: `p = pick(&x, &y)` into an outer `p` is rejected
      if either `x` or `y` is deeper than `p`.
- [ ] Method calls / UFCS (`v.first()`) go through the same path.
- [ ] `compile_fail`: `ret_ref_then_push.loam` (probe c),
      `ret_ref_then_reassign.loam` (probe f), `ret_ref_escapes_scope.loam`
      (probe a).
- [ ] `compile_pass`: `p = first(&v); fmt.println(*p); v.push(2)`, which is
      legal because `p` is dead before the push.
- **Exit:** probes a, c, f are rejected; the compile_pass case runs.

### Phase 5: Docs and cleanup

- [ ] `docs/loam.md`: add the four rules beside the ownership state diagram,
      plus a short "what to use instead" (owned value, `Box`, index, handle).
- [ ] `docs/downsides.md`: the cost line "borrowck is not NLL" is stale
      (loam.md says NLL). Fix it and add "references are second-class".
- [ ] `borrowck.c` header comment: list the escape rules.
- [ ] Run the probe programs one last time; all six are rejected.
- **Exit:** full `make test` green (run once, at the end).

## Later, only if a program needs it

Borrowing structs (a parser holding `&string` slices, iterators over a
borrowed collection) are what rules 2 and 3 give up. Because `loamc`
compiles the whole program as one unit, signatures could be inferred across
calls, working out which arguments a result actually borrows, still with no
annotation syntax. Open that as a phase only when a real program needs it.
