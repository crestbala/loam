# Two ownership bugs in generated code: `Box` contents and module `__init`

Found 2026-11-12 while measuring `loam-lsp`'s memory over a long session
([self-hosting-loam.md](self-hosting-loam.md), Next changes 4, P5). Neither is
fixed yet. Both are in the code generator, so they affect every program, the
compiler included. One is a **memory-safety hole reachable from safe Loam**
(§5.2 of the self-hosting doc says there must be none), and the other is a
leak.

## Memory safety is not leak freedom

Loam's guarantee (§2.7, §5.2) is that safe code cannot corrupt memory:

- no use after free;
- no double free;
- no out-of-bounds access;
- no data race.

A leak breaks none of these. Memory that is never freed is still valid
memory. Rust makes the same promise and the same exception (`Box::leak`,
`Rc` cycles), and Loam already lists its known leak, capturing closures, as a
resource problem rather than a safety one (§2.4, §5.2 S8).

So of the two bugs below, bug 1 breaks the guarantee and is a blocker, and
bug 2 is a resource leak, the same class as the closure leak. Bug 1 is also
why "Loam is memory safe" has to be checked rather than assumed: the
ownership rules are proven by the checker, but the C the code generator
writes must implement them. Here it did not.

## Bug 1 (memory safety): `Box::new` of a value with owned fields

### Reproduction

```loam
import "std:fmt"
struct Node { v: int, name: string }
fn mk(n: int) -> Box<Node> {
    return Box::new(Node { v: n, name: "n{{n}}" })
}
fn main() {
    let b = mk(2)
    fmt.println(b.name)   // prints a blank line, not "n2": a freed string
}
```

It compiles cleanly, then reads a string that was already freed. Inside a
single fn it does not show: a local such as `s` in `Box::new(Node { name: s })`
keeps the string alive until the fn returns, so the box only dangles once it
outlives the fn that built it.

### Cause

`mk`'s generated C:

```c
_l5 = loam_str_concat(_l6, _l7);            // "n2", refcount 1
_l8 = (Node){ .v = n, .name = _l5 };
loam_str_retain(_l8.name);                  // the literal's reference: 2
_l4 = (Node *)loam_new(sizeof(Node), ...);
*_l4 = _l8;                                 // into the box: no reference taken
loam_str_release(&_l8.name);                // the literal drops its own: 1
...
loam_str_release(&_l5);                     // the temporary drops: 0, freed
return _l4;                                 // the box points at freed memory
```

There are two halves, and they have to be fixed together:

1. `Op.Alloc` (`Box::new`, in `codegen.loam`) copies the value into the box
   with a plain `*dst = src`. A move (`move_inst`) gives the destination its
   own references for a Copy value (`nested_keeps`), or steals them from the
   source for a non-Copy one (`steal`). `Alloc` does neither, so the box
   shares references the source then releases.
2. Dropping a box is shallow: `drop_place` emits `loam_drop(&b)` for
   `TK.Box`, which frees the box's memory but releases nothing the value
   holds. The two halves hid each other. With no references of its own, the
   box also had nothing to release, so no leak showed, only the dangling
   pointer.

Both came over as they were from the C compiler (`codegen_c.c`), so the bug
is older than the port. The differential gates compared the Loam compiler's
C with C's output, so they could not see it.

### Fix (written, then reverted untested)

- **`Op.Alloc`:** after `*dst = src`, if the element type needs a drop, call
  `nested_keeps("(*dst)", et)` when it is Copy and `steal(src, et)` when it
  is not, as `move_inst` does.
- **`drop_place` for `TK.Box`:**
  `if (b) loam_elem_release_<h>(b); loam_drop(&b);`, with
  `h = elem_hook(elem)`. The release goes through the element hook, out of
  line, because a type that holds itself through a box
  (`struct List { next: Option<Box<List>> }`) would otherwise be expanded
  forever at code-generation time. The hook bodies are emitted in a loop
  that already handles hooks registered while other hooks are emitted, and
  ids are deduplicated by type, so recursion ends.
- The first half alone turns the dangling pointer into a leak (2 leaks,
  64 bytes, in the probe below), which is safe but wrong. Only both together
  are correct.
- A temporary could be moved into the box instead of retained and released,
  as `push` does for temporaries (`loam_vec_push_take`). That is a later
  speed step, not part of the fix.

### What must check it

- **Fixtures:** a `compile_pass` fixture for each shape, run under `leaks`
  (macOS) or ASan (Linux), with output compared:
  - a boxed struct with a string, returned from the fn that built it;
  - a boxed `[]string`;
  - `Box::new(named_local)` with the local used afterwards;
  - `Option<Box<List>>` holding strings, three levels deep;
  - a `Box` field reassigned.
- **The compiler itself under ASan:** it keeps a `Box` in a few fields
  (§2.7 audit). It must build itself under `-fsanitize=address`, with the
  seed = stage 1 = stage 2 check still passing.
- **Speed:** a `Box`-heavy program (the recursive-types fixtures,
  dracula), timed before and after.
- **Everything else:** the full `./nob.sh test`, `nob selfhost`, then a new
  seed if the compiler's own C changes.
- **The soundness generator:** §5.2 S1 (soundness fuzzing) should generate
  `Box` construction and drops, so the next such hole is found by a test,
  not by accident.

## Bug 2 (leak): temporaries of a module `let` initializer

### Reproduction

```loam
fn make() -> []int { let mut v = []int {}; push(v, 1); return v }
let mut A: []int = make()
let mut B: []int = []int { 1, 2, 3 }
fn main() {
    A = []int {}
    B = []int {}
}
```

`leaks --atExit`: 2 leaks, 96 bytes. In `loam-lsp` this is the constant 5 KB
`leaks` reports (P5): every global the server's `reset()` reassigns leaks
its first value once (`ast.EXTRA`, `loader.REALPATHS`, `typecheck.CNAMES`,
`borrowck.GLOBAL_NAMES`, …). It never grows with the number of rechecks.

### Cause

`lower_init` (`ir.loam`) lowers each initializer and moves it into the
global, but it never calls `emit_drops_at_exits()`, which every other fn
body runs (`lower_body`). A `[]T` or `string` is a Copy type, so the move
into the global takes its own reference (`loam_A = loam_vec_retain(&_l2)`).
The temporary then never drops, and the first value keeps a refcount of 2.
Reassigning the global releases one of them; the other is never released.

### Fix (written, then reverted untested)

`emit_drops_at_exits()` in `lower_init`, before `scope_pop_to(-1)`, as in
`lower_body`. Module globals are `L_GLOBAL` locals and never carry `L_DROP`,
so only the temporaries are dropped. With the fix, the probe and `loam-lsp`
reported 0 leaks. It changes the C of every program with a non-constant
module `let`, so it lands with the same gates as bug 1.

## Not a leak: the language server's RSS

`loam-lsp`'s peak RSS grows slowly over a session (121 MB after 5 rechecks,
178 MB after 160). The heap does not: after 12 and after 80 rechecks the
live heap at exit is the same 151,845 blocks (76 MB), and the physical
footprint stays about 31 MB. The growth is freed pages the allocator keeps.
