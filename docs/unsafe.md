# `unsafe`

`unsafe` is one word, not a keyword, the same way `async` is. It marks a
top-level fn or a block. Safe code has no pointer type, no unchecked index,
and no inline C. The only raw operations are the three below, and only where
this file allows them.

A module may use `unsafe` only when it declares a `#[c]` binding. That is
the C ABI: sockets, `memcpy`, `malloc`, a column pointer. A module that
never names C cannot open an unsafe region.

## What an unsafe region may do

`raw_addr`, `raw_read`, and `raw_write` exist only when no fn of that name
is in scope, and only inside an `unsafe` fn or block.

- `raw_addr(buf) -> i64` is the address of a string's bytes, or of a `[]T`
  or `&[]T` of numbers or bools.
- `raw_read(addr, &mut place)` copies one number, bool, or plain-data struct
  from `addr` into `place`.
- `raw_write(addr, &place)` copies one such value the other way.

Each one is a single `memcpy` or pointer cast in the generated C, so an
unaligned address is defined. An `i64` offset is ordinary arithmetic
(`raw_addr(buf) + i64(off)`).

An `unsafe fn` that is not a `#[c]` binding, and an `unsafe` block, must do
a raw operation or call an unsafe fn. A region that does neither is an
error. Regions do not nest: an `unsafe` block inside an `unsafe` fn is an
error, and so is a second block.

## What stays safe

An `unsafe fn` is private to its module, as a `#[c]` fn is. Another module
cannot call it, import it, or take it as a value. The module exports a safe
wrapper instead. An unsafe fn is not a value even inside its own module.

A `#[c]` binding is not unsafe unless it is written `unsafe fn`. A binding
that takes and returns ordinary Loam values (`strlen` of a string) stays
callable from safe code in that module. A binding that takes an address, or
allocates, is `unsafe fn`, and its callers are unsafe too.

The compiler, `nob.loam`, and the tools under `packages/loam/compiler/`,
`packages/tooling/`, and `packages/zeus/cli/` are safe Loam. `nob trusted`
rejects `unsafe` there.

## Where it is listed

`loamc check --ffi <file>` prints, per module, every `#[c]` binding (and
whether callers must be unsafe), every unsafe fn, and every unsafe block.
Paths are relative to the working directory. `nob trusted` counts the lines
of those regions as trusted Loam, beside the trusted C.
