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
5. While a borrow of a global is live (held by a binding, or passed to the
   call itself), no call may change that global: a fn that writes it,
   `&mut`-borrows it or pushes to it, directly or through what it calls. A
   call through a fn value may change anything. Another module's global
   (`store.ITEMS`) is the same place as in its own module.
6. A function cannot return a reference into a global, nor store one through
   `*pp`: the caller could not see what it borrows. Return a copy or an index.

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
- Gap closed in phase 6: a generic body that builds `[]T` with `T` a
  reference.

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
  keeps its loans. (The explicit `&mut x` argument case was fixed in
  phase 6.)

### Phase 5: Docs and cleanup — done

- [x] `docs/loam.md`: a "References" section after the ownership state
      diagram (the four rules and what to store instead); the borrowck row in
      the pipeline table names the escape rules.
- [x] `docs/downsides.md`: "borrowck is not NLL" replaced; references are
      second-class in the keep column and in the costs line.
- [x] `borrowck.c` header comment lists the escape and loan rules.
- [x] The probe programs, one last time: all rejected.
- **Exit:** full `./nob.sh test` green (run once, at the end).

### Phase 6: Remaining gaps after the first pass — done

- [x] **Generic body boxing a reference.** A generic body is checked once,
      with `T` unknown, so `let v = []T {}` slipped through when a call bound
      `T = &int`. Typecheck now records, per generic fn, which type
      parameters it puts in a container (`[]T`, `[N]T`, `Box<T>`, a generic
      struct's container field), including in its signature. A generic call
      passing its own `T` on to a boxing parameter marks `T` too (fixed point
      after all bodies). Then any call binding a boxing parameter to a type
      holding a reference is an error: `'hold' stores its type parameter 'T'
      in a container, so it cannot be a reference (&int)`. This replaces
      phase 3's return-type-only check. Generics that never box `T` still
      take references (`id(&x)`).
- [x] **C hooks returning a reference.** An empty-bodied fn in a seam module
      is a C hook; nothing can check what a reference it returns points at.
      It is now an error (`C hook 'c_lookup' cannot return a reference`).
      `#[c]` fns were already limited to scalars. No hook in the repo
      returned one.
- [x] **`&mut x` arguments lasted to the end of the statement.** A call's own
      `&x` / `&mut x` argument borrows now end with the call (unless it
      returns a reference, whose binding takes them as loans), so
      `bump(&mut n) + bump(&mut n)` compiles. Clashes inside one call are
      still caught as each argument is borrowed.
- [x] `compile_fail`: `generic_boxes_ref_locally`,
      `generic_boxes_ref_transitive`, `bodyless_returns_ref`,
      `mut_addr_twice_one_call`.
- [x] `compile_pass`: `generic_ref_args`, `mut_arg_per_call` (the phase 5
      build rejected it).
- **Exit:** `nob test compile_` (750) and `nob test examples` green; all 505
  entry programs give the same result as the phase 3 build apart from the
  new pass tests. **Green.**

### Phase 7: Find holes automatically — done

`nob fuzz-borrow [N]` (default 200; `FUZZ_SEED`, `LOAM_FUZZ_LOAMC`), off the
default suite. It generates programs that aim two references at three lists,
two boxes, and locals, through `&v[0]`, `first(&v)`, `&*b`, copies, `pick`,
nested `if` expressions, `aim(&mut p, …)` out parameters, and helpers that
may point into their own frame; then grows, empties, or replaces what they
point at, inside branches and loops. Every program `loamc` accepts is built
with `-fsanitize=address -fsanitize-address-use-after-return=always -O0` and
run. An ASan report is a hole (copied to `tests/tmp/fuzz/holes/`); a program
rejected for a non-borrow reason is a generator bug (`other`).

- [x] **The oracle.** The C backend hoists every local to the function and
      drops values at function exit, so a reference outliving an inner block
      cannot fail at run time and ASan cannot see it. What it does see: a
      list buffer reallocated or freed under a reference, a replaced `Box`,
      and a reference into a returned frame. Inner-scope rules stay covered
      by the `compile_fail` tests.
- [x] **Validated against the old checker.** With `LOAM_FUZZ_LOAMC` set to a
      `loamc` built from before phase 1, 100 programs gave 17 holes. Against
      the current one: 0.
- [x] Found on the way and fixed:
      - `loamc` reused a stale binary when only `LOAM_LINK_EXTRA` changed
        (the freshness key ignored it), so sanitizer flags silently did
        nothing. It is part of the key now.
      - A nested `if` expression ending an arm
        (`if a { if b { 1 } else { 2 } } else { 3 }`) was typed `void`.
        `check_block_value` now treats it as the arm's value, the same shape
        `wrap_else_if` gives an `else if` (`if_expr_nested_arm`).
      - `aim(&mut q, q)` was rejected: the explicit `&mut q` was taken before
        the other argument copied `q`. Arguments are now read first and the
        `&mut place` ones taken after, so only a *borrow* still held by
        another argument conflicts (`put(&mut v, &v[0])` stays an error).
- [x] **A real hole, found by the fuzzer** (seed 4242, program 1739):
      `expr_uses` did not look inside `if` expressions, so in
      `q = &*b0; b0 = Box::new(20); q = if c { … } else { q }` the read of `q`
      was missed, `q`'s loan ended early, and replacing `b0` freed what `q`
      pointed at. The same blind spot hit the older last-use check
      (`let n = if c { *p } else { 0 }` after a push). Fixed in `expr_uses`;
      tests `if_expr_reads_holder`, `if_expr_last_use`.
- **Exit:** 0 holes in 4,500 programs on the current rules: seeds 4242 and
  99 (2,000 each, after the `expr_uses` fix, before outer liveness) and seed
  1 (500, with outer liveness). The 2,000-program reruns of seeds 4242 and
  99 with outer liveness were still running at this commit. **Green.**

### Phase 8: Real liveness — done

- [x] `end_dead_borrows` asks `seq_access`/`first_access` whether a holder is
      read again, along any path, before it is overwritten: an assignment
      `r = …` that does not read `r` kills it; `if` kills only when both arms
      do; a loop body never kills (it may not run); anything unclear (a
      closure, `aim(&mut r, …)`, a compound assignment) counts as a read. A
      killed or unread holder's loans end at once.
- [x] A binding of this block is judged by the rest of the block. An *outer*
      binding (declared in an enclosing block) is judged by the rest of this
      block and then by what follows it, walking out through a stack of the
      blocks being checked: a loop around the block may run its body again
      (`liveness_loop_read_first`, `liveness_outer_next_iter`), a `switch`
      may fall into the next case, an `if` is done, then the statements after
      it, up to the binding's own block. So `let r = &v[0]; if c { use(r);
      v.push(2) }` is legal (`liveness_outer_in_block`), and a read after the
      block keeps the loan (`liveness_outer_read_after`,
      `liveness_outer_two_levels`).
- [x] `compile_pass`: `liveness_overwrite` (overwrite then mutate the old
      target, overwrite in both arms, overwrite at the top of every
      iteration, `aim(&mut q, q)`, `grow(&mut v, v.len)`).
- [x] `compile_fail`: `liveness_one_path_kill`, `liveness_loop_read_first`,
      `mut_arg_with_shared_borrow`, `liveness_outer_read_after`,
      `liveness_outer_next_iter`, `liveness_outer_two_levels`.
- [x] Fuzzer acceptance on the same 500 programs went from 180 to 212, with
      0 holes before and after.
- **Exit:** `nob test compile_` (764) and `nob test examples` green; all 508
  entry programs give the same result as the phase 3 build apart from the
  new pass tests. **Green.**
- Still conservative: leaving an `if` expression arm or a closure body keeps
  outer holders live (the rest of the enclosing statement is not walked);
  a loop or `switch` that mentions a holder anywhere keeps it live through
  the whole statement. `aim(&mut r, …)` counts as a read of `r` by design,
  not caution: the callee may leave `r` as it was, so its old loans stay.

### Phase 9: Borrows of globals across calls (rules 5, 6) — done

Found while porting the checker to Loam (self-hosting phase 6). Borrowck saw
borrows within one fn only, and not at all for another module's globals, so
four programs read freed memory (each printed `0`, not `1`):

| Shape | Program |
|-------|---------|
| held across a call | `let first = &ITEMS[0]; grow_items(); first.value` |
| passed to a writer | `show_after_growth(&ITEMS[0])`, which grows ITEMS, then reads its argument |
| another module's global | `let first = &store.ITEMS[0]; push(store.ITEMS, …)` or `store.grow_items()` |
| returned | `fn first_item() -> &Item { return &ITEMS[0] }`, then `grow_items()` |

- [x] A borrow records the global its place roots in (`global_of_place`;
      `store.ITEMS` resolves to the same declaration). Each fully imported
      module is a binding under its alias, so `&store.ITEMS[i]` is tracked
      like `&ITEMS[i]` (borrows only: moves out of it are not checked, as
      before).
- [x] `global_writes`: per fn, the globals it may change, its callees'
      included, to a fixpoint. A call through a fn value, a signal store, an
      unresolved callee, or a bodyless hook handed a fn may change any.
- [x] At every call, a live borrow of a global the callee may change is an
      error. A borrow through an index is taken after the other arguments
      (borrowck's argument phases and the IR's lowering agree), so
      `push(NODES[n].kids, new_node())` stays legal.
- [x] `return` and `*pp = …` of a reference into a global are errors
      (`refers_into_global`: `&place`, a binding holding such a loan, a call
      result made from one, either arm of an `if`).
- [x] `compile_fail`: `global_borrow_across_call`,
      `global_borrow_passed_to_writer`, `global_borrow_cross_module`,
      `global_borrow_cross_module_push`, `return_ref_into_global`.
      `compile_pass`: `global_borrow_reader_call` (calls that only read the
      global or change another one, an argument that grows the vec beside an
      indexed borrow of it, another module's global).
- **Exit:** no new error on any of the repo's 780 non-`compile_fail` `.loam`
  files (std, Zeus, examples, `nob.loam`, the self-hosted compiler);
  `nob test compile_pass`, `compile_fail`, `golden` green.
- Conservative: the global is one place, so a call that changes `G.a` is
  refused while `G.b` is borrowed.

## Later, only if a program needs it

Borrowing structs (a parser holding `&string` slices, iterators over a
borrowed collection) are what rules 2 and 3 give up. Because `loamc`
compiles the whole program as one unit, signatures could be inferred across
calls, working out which arguments a result actually borrows, still with no
annotation syntax. Open that as a phase only when a real program needs it.
