# Platforms

Where Loam's command-line surface builds, which C compilers it expects, and
how that is checked. The GUI runtimes (Zeus on Cocoa / X11, Maya, raygui) are
out of scope here; this is the path `loamc`, `nob`, and any CLI program take.

## Support matrix

| Platform                  | Runtime (`loam_rt.h`) + `std:sys` | Compiler (`bootstrap/loamc.c` → `bin/loamc`) | Verified by                                         |
| ------------------------- | --------------------------------- | ----------------------------------- | --------------------------------------------------- |
| macOS (arm64, x86_64)     | yes                               | yes                                 | `./nob.sh test` (built and run)                     |
| Linux x86_64, aarch64     | yes, headless (no X11 needed)     | yes                                 | CI `selfhost` (full suite, run); `cross-check.sh`   |
| Windows x86_64            | yes (MinGW-w64), except `std:net` | yes (`nob` itself does not run)     | CI `selfhost` (fresh checkout: seed, fixed point, fixtures run natively); `cross-check.sh` |
| Windows aarch64           | same as x86_64                    | not run                             | `cross-check.sh` (built and linked only)            |
| WASM, iOS, Android        | as before (secondary)             | —                                   | their own suites                                    |

**CI** (`.github/workflows/selfhost.yml`) runs on pull requests that
target `master` and by hand, never on push. Every OS builds from the checkout alone, the
same way: `./nob.sh` has the C compiler build the committed seed,
`bootstrap/loamc.c`, into `bin/loamc0`, and that builds `bin/loamc` from
`packages/loam/compiler`. Linux then runs `./nob.sh test` on x86_64 and
aarch64. On Windows (MinGW-w64 gcc) `nob.sh` stops after `bin/loamc`: `nob`
needs a POSIX shell. That compiler emits each non-GUI fixture's C
(`tools/win-bundle.sh`); MinGW-w64 gcc builds and runs it, comparing stdout
with the fixture's `.expected` byte for byte (`tools/win-run.sh`); and it
builds itself again, which reaches the fixed point
(`tools/win-selfhost.sh`). The `same-c` job checks that Windows
and Linux emitted the same C for the compiler and every fixture.

**Cross-check** (`tools/cross-check.sh`, needs `zig`) builds and links the
same set for all four targets from a macOS machine — the quick local check,
no runner needed. Each fixture links what the driver adds: `runtime/net.c`
for std:net and every `#![c_source]` / `#![c_link]` of it and its packages.

**Known gap: `std:net` on Windows.** `runtime/net.c` is BSD sockets plus the
platform TLS, with no Winsock/SChannel port, so the `http` / `net` fixtures
are listed and skipped there. The compiler needs no networking, so this is
outside the self-hosting work.

## C compilers

| Platform | Supported                                          | Notes                                                    |
| -------- | -------------------------------------------------- | -------------------------------------------------------- |
| macOS    | Apple clang (`cc` from the Command Line Tools)     | the default everywhere in the repo                       |
| Linux    | gcc or clang, glibc                                | generated C (the seed and `loam-lsp` included) is `-std=gnu99`; `loam-fmt` is `-std=c11 -D_GNU_SOURCE` |
| Windows  | clang or gcc targeting `*-windows-gnu` (MinGW-w64), e.g. `zig cc`, llvm-mingw, MSYS2 | MSVC `cl.exe` is not supported: the generated C uses GNU extensions (statement expressions, `__attribute__`) |

**How the compiler is found.** `nob.sh` builds the seed with `$CC`, else
`cc`, else `gcc`. `loamc` (`packages/loam/compiler/loamc.loam`) honours
`CC` (split on spaces, so `CC="zig cc"` works) and otherwise uses the first of
`cc`, `clang`, `gcc` on `PATH`, invoking it through `sys.spawn` with an
argument list, never through a shell.

## Windows notes

- **Paths are `/` on the Loam side.** Every Win32 file API accepts `/`, so
  the runtime passes paths through, and `sys.cwd()` returns `/`-separated
  paths. `sys.is_abs` knows `C:/x`, `C:\x`, and `\\host\x`; `sys.mkdir`
  skips the drive in `C:/a/b`.
- **No shell in process spawn.** `sys.spawn` uses `_spawnvp` and quotes each
  argument the way `CommandLineToArgvW` splits it back, so an argument
  arrives verbatim. `sys.exec` still goes through `cmd.exe` (`_popen`).
- `sys.rename` replaces an existing target, as POSIX `rename` does.
- stdout and stderr are binary from startup, and `sys.read_stdin` /
  `sys.read_line` put stdin in binary mode: bytes out are the bytes written
  (no `\n` → `\r\n`), which byte-identical generated C depends on.
- `crypto.random` uses `rand_s` (the OS CSPRNG) where POSIX has `getentropy`.

**Why the C compiler was never ported to Windows.** `driver.c` built shell
command lines (`sh -c`, `2>/dev/null`, `fork`/`execl`, `mkstemp`). The Loam
driver that replaced it uses `sys.spawn`, and every checkout bootstraps
from the committed generated C of the Loam compiler (`bootstrap/loamc.c`),
so porting the C driver would have been work thrown away
(docs/self-hosting-loam.md, essentialism rule).

**The seed is current.** `bootstrap/loamc.c` is the C the current compiler
emits for itself, byte for byte, so a commit that changes the compiler runs
`nob seed` and commits the new seed with it. `nob selfhost selfhost_seed`
(CI runs it on every pull request into `master`) checks that `cc` builds
the seed, that the seed builds the current compiler, that the result
reaches the fixed point, and that the C it emits is the committed seed. Any
byte difference fails and prints the first differing lines.
