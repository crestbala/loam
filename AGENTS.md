# Loam Language Rules (Root Prompt)

You are working on the Loam programming language.  
These rules are non-negotiable. Apply them on every design decision, code change, and suggestion.

## 5 Core Goals
1. Essentialism + Minimalism
2. Greenest language: equal or better than C (zero-overhead, minimal runtime)
3. Memory safe
4. Near-zero memory-related security vulnerabilities
5. Multi-platform, including embedded (Arduino, Raspberry Pi, bare-metal)

## Rules

### 1. Essentialism + Minimalism
- Language must fit in one afternoon document (syntax + semantics).
- No feature unless required for one of the other goals and it passes the efficiency test.
- Prefer fewer, sharper constructs. Convenience belongs in libraries, not the language.
- Compiler and runtime stay small. Prefer implementing functionality in Loam over C.

### 2. Greenest: Equal or Better Than C (Zero-Overhead)
- Generated C (or direct emission) must be as good as or better than skilled handwritten C in size, speed, and predictability.
- Zero hidden cost: no hidden allocations, no mandatory runtime, no GC, no refcounting.
- Drive runtime C toward zero. Move logic into Loam unless there is a measured, justified reason it must stay in C.
- Compile times stay in the C range.
- Embedded binary and RAM footprint must be competitive with or better than equivalent C.
- Dead-code elimination must remove unused runtime pieces.

### 3. Memory Safety (Simple Model)
- Compile-time ownership + linear types for all resources.
- Use-after-free, double-free, and use-after-move are hard compile errors.
- No full borrow checker.
- Bounds and null: compile-time proofs where possible (place identity, no mutation, capacity clamping); otherwise cheap runtime traps.
- Explicit `unsafe` + `#[c]` only when required for FFI or raw access; it must be obvious, rare, and reviewed.
- Prefer proofs and sanitizers (ASan, UBSan, TSan) over trusting C.

### 4. Near-Zero Memory Vulnerabilities
- The safety model must eliminate classic memory corruption vectors (UAF, double-free, most overflows).
- Generated code and remaining runtime must not re-introduce those vulnerabilities.
- Capacity clamping and range proofs for integer/index operations are required.
- Data races remain the programmer’s responsibility (optional `Send` checks + TSan).
- Goal: memory-related attack surface as close to nil as practical.

### 5. Multi-Platform Including Embedded
- Same source targets: native(macOS, linux, windows), web (wasm), mobile, Raspberry Pi, Arduino-class, and bare-metal.
- Use `#[os]` and `#[arch]` attributes for platform/CPU variants. No hidden platform logic.
- Platform differences handled by thin host layers or compile-time flags only.
- Embedded must support no OS / minimal or no libc, static allocation preferred, deterministic behavior.
- No feature that makes bare-metal or small-MCU targets impractical.
- Remaining runtime must be optional and eliminable via DCE for embedded builds.

## Decision Filter
Every change must satisfy all of these:
1. Keeps the language, compiler, and runtime smaller or the same (prefer Loam over C)
2. Generated code ≥ C in efficiency and predictability (zero-overhead)
3. Preserves or strengthens the ownership + proof-based safety model
4. Reduces (not increases) memory vulnerability surface
5. Works cleanly on embedded targets and can eliminate unused runtime via DCE

If any answer is no → reject or simplify.

These rules override all other preferences. Stay minimal, stay zero-overhead, stay safe, stay portable.