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
- Follow-up: `if` expressions were not borrow-checked at all (arms
  unchecked, value not followed). Fixed after phase 5: arms are checked like
  a statement `if`, the value's root is recorded while the arms' locals are
  live, and both arms' borrows are kept. Tests: `if_expr_*`. A reference
  read out of `*pp` (`& &T`) is caught through the call rule
  (`inner(&p)` points into `p`).

### Phase 2: No reference-typed struct fields (case b) — done

- [x] `struct_type_of` rejects a field whose type holds a reference:
      `&T`, `[]&T`, `[N]&T`, `Box<&T>` (`type_holds_ref`). A `fn(&T)` field
      is fine because it only takes references as arguments. Error:
      `struct field 'Holder.r' cannot hold a reference (&int) — store an
      owned value, a Box, or an index`.
- [x] `make_struct_inst` rejects an instantiation that makes a generic field
      a reference, whether written (`Holder<&int>`) or inferred from a literal
      (`Holder { v: &x }`). It takes the use site's location for the error;
      instantiations reached through `subst_type` report at the decl.
- [x] C seam: no FFI or `#[c]` struct in the repo has a reference field, so
      no exemption was needed.
- [x] `compile_fail`: `struct_ref_field`, `struct_ref_vec_field`,
      `struct_ref_generic_arg`, `struct_ref_generic_inferred`.
- [x] `compile_pass`: `struct_no_ref_fields` (generic over a value, an index
      cursor, a `Box` field, a `fn(&int)` field).
- **Exit:** probe b is rejected; `nob test compile_` (711) and
  `nob test examples` green; `loamc check` on all 500 entry programs gives no
  new rejection. **Green.**

### Phase 3: No borrow outliving its source in the frame (cases a, e) — done

Checked at the **use**, NLL-style, not at the assignment: aiming an outer
reference into an inner scope is legal as long as it is not read after that
scope ends.

- [x] Each binding records the deepest frame binding it may point into
      (`ref_depth`, 0 = caller or global data) and its name. This replaces
      phase 1's single root name. Assignment replaces what a reference points
      at; `if` / `switch` (including the no-match path) / `for` / `while`
      (zero or more iterations) join by taking the deepest.
- [x] When a block ends (`pop_to_depth`), a surviving reference into it is
      marked dangling. Reading it, through `p`, `*p`, `p.f`, or `&p`, is an
      error: `'p' is used after 'x', which it borrows, went out of scope`.
      Assigning it again clears the mark.
- [x] A reference written to an untracked place (`*pp = &x`, or a global)
      must outlive that place right away: `'x' does not live long enough to
      be stored here`.
- [x] A call given `&mut p`, with `p` a reference, may aim `p` at any of its
      other reference arguments (`aim(&mut p, &v)`): the signature carries no
      lifetimes, so the caller assumes the worst.
- [x] `[]&T`, `[N]&T`, and `Box<&T>` are banned everywhere: written types,
      `Box::new(&x)`, array literals of references, and a generic call whose
      return type becomes a container of references (`wrap(&x)` with
      `-> []T`). Phase 2's struct check now reports only direct `&T` fields;
      containers are reported where the type is written.
- [x] `compile_fail`: `borrow_escapes_scope` (probe e), `borrow_escapes_loop`,
      `borrow_dangles_next_iter`, `borrow_escapes_via_call` (probe a),
      `borrow_stored_through_deref`, `borrow_stored_via_out_param`,
      `vec_of_refs`, `box_of_ref`, `generic_returns_ref_list`.
- [x] `compile_pass`: `borrow_scoped_refs` (inner-scope use, reassignment
      after the scope, per-iteration reassignment, `aim(&mut p, &v)` with
      `v` outliving `p`).
- **Exit:** probes a and e are rejected; `nob test compile_` (722) and
  `nob test examples` green; `loamc check` on all 501 entry programs gives no
  new rejection. **Green.**
- Gap: a generic body that builds `[]T` locally, with `T` a reference, and
  never returns it is not checked (only the instantiated return type is).

### Phase 4: A reference keeps what it borrows borrowed (cases c, f) — done

Decision: **tie to arguments**. `-> &T` stays legal (accessors like
`first(xs)`), and the result borrows what its reference arguments borrow.

- [x] Loans replace the single `Binding.borrow_of` path. Each stored
      `Borrow` records its `owner` (the binding holding it) and `target` (the
      binding the place roots in), so two references to `v` end
      independently, and a binding's loans end with it or at its last use.
- [x] `gather_loans` collects what a reference holds: the place of `&place`;
      for a copy `let b = a`, `a`'s loans plus a reborrow of `a`; for a call
      returning a reference, the same for every reference argument
      (recursively). A `&T` result keeps only shared loans, even from `&mut`
      arguments.
- [x] Used for `let`, for rebinding a reference (`p = …` replaces `p`'s
      loans, and an assignment now holds them like a `let`), for
      `aim(&mut p, &v)` (p also holds `v`), and for `*pp = &x` with `pp`
      aimed at a local reference.
- [x] Assigning or `++`-ing a place a live loan covers is an error:
      `cannot assign to 'v' while it is borrowed`.
- [x] A `&mut` reference passed to a `&mut` parameter is reborrowed mutably
      *at the call*, after all arguments are read (`grow(s, s.n)` is fine),
      and only for the call. `two(m, m)` and `first(xs)` followed by
      `xs.push()` while the result lives are rejected.
- [x] `if` / `switch` arms each start from the loans before the branch and
      join by union, so `if c { r = &x } else { x = 2 }` is legal.
- [x] `compile_fail`: `ret_ref_then_push` (probe c), `ret_ref_then_reassign`
      (probe f), `assign_while_borrowed`, `incdec_while_borrowed`,
      `reassigned_ref_keeps_loan`, `copied_ref_keeps_loan`,
      `shared_loans_independent`, `ret_ref_through_param`, `aim_keeps_loan`,
      `mut_ref_passed_twice`.
- [x] `compile_pass`: `ret_ref_loans` (NLL release after last use, rebinding
      releases, `if`/`switch` arms, copied reference, `grow(s, s.n)`). The
      previous build rejected it: a `let` loan outlived rebinding.
- **Exit:** all six original probes plus three found on the way (assign
  while borrowed, assignment-held loan, two shared borrows of one place) are
  rejected; `nob test compile_` (734) and `nob test examples` green; all 502
  entry programs give the same result as the phase 3 build. **Green.**
- Conservative, not unsound: liveness is per block statement, so a
  reference mentioned anywhere later (even overwritten before it is read)
  keeps its loans. An explicit `&mut x` argument is still a temp that lasts
  to the end of the statement, as before this work, so
  `f(&mut x) + g(&mut x)` is rejected.

### Phase 5: Docs and cleanup — done

- [x] `docs/loam.md`: a "References" section after the ownership state
      diagram (the four rules and what to store instead); the borrowck row in
      the pipeline table names the escape rules.
- [x] `docs/downsides.md`: "borrowck is not NLL" replaced; references are
      second-class in the keep column and in the costs line.
- [x] `borrowck.c` header comment lists the escape and loan rules.
- [x] The probe programs, one last time: all rejected.
- **Exit:** full `./nob.sh test` green (run once, at the end).

## Later, only if a program needs it

Borrowing structs (a parser holding `&string` slices, iterators over a
borrowed collection) are what rules 2 and 3 give up. Because `loamc`
compiles the whole program as one unit, signatures could be inferred across
calls, working out which arguments a result actually borrows, still with no
annotation syntax. Open that as a phase only when a real program needs it.
