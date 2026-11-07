# Loam Module System Improvements

**Goal**: Improve Loam’s import/module system with clean full and selective imports, properly isolate packages, and make them production-ready so they can be shipped and built identically on any machine without path/env surprises. Align with the language’s “futuristic, green, efficient” aims: smaller binaries via real tree-shaking, reproducible builds, zero runtime module overhead.

## Current State (as of Nov 2026)

### Import Syntax
- Full: `import "std:fmt"` → access as `fmt.println(...)`
- Selective (already partially supported):  
  `import { add, sub as minus } "math.loam"`  
  or `import { Card } "std:ui"`
- Rename module: `import "img.loam" as pic`
- Resolution forms:
  - `std:name` → `packages/loam/std/name.loam` then `LOAM_PATH` roots’ `std/name.loam`
  - relative `"path.loam"`
  - `pkg:name` → `vendor/name/name.loam` (searched upward)
- Parser (`packages/loam/src/parser.c`): supports both quoted and `{...} "path"` forms.
- Selective imports bind bare names; the module alias remains available.
- No `export` keyword. All top-level `fn`, `struct`, `const`, `enum`, and other declarations in a module are available to be imported. Visibility is controlled only by what the importer chooses to import.

### Packaging
- Language std lives in `packages/loam/std/`.
- Frameworks (`zeus`, `http`, `maya`, `raygui`) live outside and are discovered only via `LOAM_PATH`.
- Third-party: `loam.deps` + `zeli pkg sync` → materializes into `vendor/` + writes `loam.lock`.
- Whole-program compilation to one C file. No strong per-package isolation or independent shipping of prebuilt artifacts.
- No package registry. Builds can fail on different machines if `LOAM_PATH` is unset or vendor is incomplete.

### Pain Points
- `std:` prefix + `LOAM_PATH` search mixes core language std with frameworks; feels messy and non-portable.
- Selective imports exist but are under-used / under-documented; full module is still loaded.
- Packages are not first-class, isolatable, versioned units that can be published and consumed independently.
- Production shipping relies on the monorepo + env vars → “works on my machine” risk.
- No true tree-shaking from selective imports → larger binaries than necessary (hurts “green/efficient”).

## Target Design

### 1. Import Syntax (no `from` keyword)

```loam
// Full module import
import "std:zeus"                          // → zeus.App, zeus.Box, ...
import "std:zeus" as ui                    // optional rename of the module

// Selective / named import
import { App, Box, Text, Button, Direction } "std:zeus"
import { Direction as Dir, LOOK } "std:zeus"
import { add, mul as times } "std:math"
import { Card } "std:ui"
```

**Rules**:
- Full import: `import "path"` or `import "path" as name`.
- Selective import: `import { Name, Other as Alias } "path"`.
- No `from` keyword.
- Prefer selective imports for application code (encourages tree-shaking).
- Full import still available for convenience and for internal framework use.
- Support `std:`, `pkg:`, and relative paths in the string.
- Optional future: import maps or path aliases in package manifest.

**Resolving the `std:` prefix mess**:
The current `std:` prefix is overloaded. Language core (`fmt`, `math`, `thread`, etc.) and large frameworks (`zeus`, `http`, `maya`) all live under the same `std:` namespace and are resolved via a mixed `LOAM_PATH` search. This is messy and non-portable.

Target:
- Keep `std:` only for true language standard library modules that ship with the compiler.
- Frameworks and third-party packages use their own package names (resolved via lockfile + vendor), e.g. `import { App } "zeus"` or `import "zeus"`.
- No more magic `LOAM_PATH` lookup that makes `std:zeus` point at a completely different tree.

### 2. First-Class Packages

Every shippable unit (std modules, frameworks, third-party libs, user packages) becomes a **package**:

```
my-pkg/
  loam.toml          # or package.loam.toml
  src/
    lib.loam         # or main.loam / zeus.loam
    internal/
  tests/
  README.md
```

**`loam.toml` example**:
```toml
[package]
name = "zeus"
version = "0.4.0"
description = "Retained UI toolkit"
entry = "src/zeus.loam"

[dependencies]
http = { version = "0.2", source = "git+https://..." }
# or path, registry, etc.

[lib]
# optional: features, entry points
# No export keyword. All top-level declarations are available for import.
```

- Core language std remains special (always available, versioned with the compiler).
- Frameworks (`zeus`, `http`, `maya`) become ordinary packages, not magically found via `LOAM_PATH`.
- Resolution order (deterministic):
  1. Relative paths
  2. Package’s own dependencies (from lockfile / vendor)
  3. Language std (hard-coded search or embedded)
- Drop or deprecate free-form `LOAM_PATH` for normal use. Keep it only as an advanced override for local development.

### 3. Isolation & Production Shipping

- **Vendoring + lockfile is mandatory** for any non-std dependency:
  - `loam pkg sync` (or `zeli pkg sync`) reads `loam.toml` / `loam.deps` → writes `vendor/` + `loam.lock` with checksums.
  - Builds never hit the network; everything is local.
  - Identical source + lockfile → identical binary on any machine (no env-var dependency).
- Packages can be published independently once a site exists (git tags, a lightweight registry, or a URL). There is no package site yet.
- A shipped application is self-contained: entry file + `vendor/` + lockfile. Compiler only needs to be on the PATH.
- Optional: precompiled package caches (IR or C objects) for faster incremental builds across machines.

### 4. Efficiency / “Green” (Tree-Shaking & Minimal Binaries)

- Selective imports drive dead-code elimination:
  - Only the referenced declarations (and their transitive deps) are type-checked fully and emitted.
  - Unused functions, structs, and constants from a module are dropped.
- Whole-module import still works but generates a warning in release builds (or a lint).
- Existing `--cost` flag extended to show “bytes contributed by each imported package”.
- Goal: a minimal CLI using only `fmt` + a few math functions should produce a tiny binary; a Zeus app should only pull the widgets it actually uses.

### 5. Compiler & Tooling Changes

| Area              | Current                          | Target                                      |
|-------------------|----------------------------------|---------------------------------------------|
| Resolution        | LOAM_PATH search                 | Manifest + lockfile + vendor               |
| Selective import  | Supported, module still loaded   | Drives tree-shaking                        |
| Package isolation | Monorepo + env                   | Self-contained vendor + lock               |
| Shipping          | Source monorepo                  | Independent packages + reproducible builds |
| Binary size       | Whole modules                    | Used symbols only                          |
| Docs              | Mixed std/framework under std:   | Clear package model                        |

**Concrete work items**:
1. Update parser/docs to fully document and prefer the selective form (`import { Name } "path"`).
2. Introduce `loam.toml` (or extend existing deps format) and make resolution deterministic without `LOAM_PATH`.
3. Make frameworks (`zeus`, `http`, etc.) ordinary packages with their own manifests.
4. Strengthen tree-shaking in the IR / codegen stages based on which names were imported.
5. Ensure `loam pkg sync` + `loam.lock` is the only supported way to consume non-std packages.
6. Add tests: cross-machine reproducibility, selective-import binary size reduction, missing-package clear errors.
7. Update `docs/loam.md` to reflect the new module/package model. Leave the existing “do not add” decisions in `docs/downsides.md` (no `pub`, no re-exports, no `export` keyword) unchanged.
8. (Later, when a package site exists) a package registry or `loam pkg publish`, and precompiled package caches. Until then a dependency is a path in `loam.toml` or `loam.deps`, vendored by `zeli pkg sync`.

## Migration Path

1. **Phase 1 (syntax + docs)**: Document selective imports as `import { Name } "path"`. Full form remains `import "path"`. No `from` keyword. No `export` keyword. Begin separating `std:` (language core only) from frameworks. Update examples.
2. **Phase 2 (packaging)**: Add manifests for existing frameworks. Make them ordinary packages. Deprecate `LOAM_PATH` for normal resolution. Require lockfile for any external dep.
3. **Phase 3 (tree-shaking)**: Make selective imports actually drop unused code. Measure binary size wins.
4. **Phase 4 (polish)**: Better error messages, package caching, editor/LSP improvements (go-to-def across packages). No export system.

## Example After Changes

```loam
import { App, Box, Text, Button, Direction, LOOK } "zeus"
import { println } "std:fmt"

fn main() {
    let n = zeus.signal(0)
    App("Count", fn() {
        Box(align_direction = Direction.Column, padding = 16) {
            Text("Count: {{n.get()}}")
            Button("+", look = LOOK.Solid, on_click = fn() => n.set(n.get() + 1))
        }
    })
    println("started")
}
```

A production build of this should only include the used Zeus widgets + signal machinery + fmt, not the entire framework.

## Success Criteria

- `import { Direction } "zeus"` (or equivalent package path) works and is the recommended style.
- Full imports use `import "path"`.
- Selective imports use `import { Name } "path"`.
- No `from` keyword. No `export` keyword.
- The `std:` prefix is reserved for language core only; frameworks are ordinary packages.
- A package can be developed, versioned, and consumed by any Loam project on any machine with only the lockfile + vendor directory (no special env vars).
- Binary size of a selective-import program is measurably smaller than the equivalent full-import program.
- Builds are bit-for-bit reproducible across machines given the same sources + lockfile.
- Language remains simple while feeling modern and production-ready. All top-level items are importable; the importer decides what to pull in. Existing decisions in `docs/downsides.md` (no `pub`, no re-exports) stay.

This direction keeps Loam’s Odin-like simplicity and Rust-like safety while giving it a module system that is practical for real, shippable software.
