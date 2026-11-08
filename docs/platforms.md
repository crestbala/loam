# Platforms

Where Loam's command-line surface builds, which C compilers it expects, and
how that is checked. The GUI runtimes (Zeus on Cocoa / X11, Maya, raygui) are
out of scope here; this is the path `loamc`, `nob`, and any CLI program take.

## Support matrix

| Platform                  | Runtime (`loam_rt.h`) + `std:sys` | Seed compiler (`packages/loam/src`) | Verified by                               |
| ------------------------- | --------------------------------- | ----------------------------------- | ----------------------------------------- |
| macOS (arm64, x86_64)     | yes                               | yes                                 | `./nob.sh test` (built and run)           |
| Linux x86_64, aarch64     | yes, headless (no X11 needed)     | yes                                 | `tools/cross-check.sh` (built and linked) |
| Windows x86_64, aarch64   | yes (MinGW-w64 headers)           | no — see below                      | `tools/cross-check.sh` (built and linked) |
| WASM, iOS, Android        | as before (secondary)             | —                                   | their own suites                          |

`tools/cross-check.sh` cross-compiles with `zig cc` from a macOS machine: the
generated C of `nob.loam` and of every non-GUI `compile_pass` fixture, linked
for each target, plus the seed sources for Linux. It proves the code builds
and links; it does not run it. Running there needs the platform itself (a CI
runner or a VM), which is the remaining step for the multi-platform claim.

## C compilers

| Platform | Supported                                          | Notes                                                    |
| -------- | -------------------------------------------------- | -------------------------------------------------------- |
| macOS    | Apple clang (`cc` from the Command Line Tools)     | the default everywhere in the repo                       |
| Linux    | gcc or clang, glibc                                | generated C is `-std=gnu99`; the seed is `-std=c11 -D_GNU_SOURCE` |
| Windows  | clang or gcc targeting `*-windows-gnu` (MinGW-w64), e.g. `zig cc`, llvm-mingw, MSYS2 | MSVC `cl.exe` is not supported: the generated C uses GNU extensions (statement expressions, `__attribute__`) |

**How the compiler is found.** The seed `loamc` runs `cc` from `PATH` for
every build. The Loam `loamc` (self-hosting phase "driver") will honour `CC`
and otherwise use `cc`, then `clang`, then `gcc`, invoking it through
`sys.spawn` with an argument list, never through a shell.

## Windows notes

- **Paths are `/` on the Loam side.** Every Win32 file API accepts `/`, so
  the runtime passes paths through, and `sys.cwd()` returns `/`-separated
  paths. `sys.is_abs` knows `C:/x`, `C:\x`, and `\\host\x`; `sys.mkdir`
  skips the drive in `C:/a/b`.
- **No shell in process spawn.** `sys.spawn` uses `_spawnvp` and quotes each
  argument the way `CommandLineToArgvW` splits it back, so an argument
  arrives verbatim. `sys.exec` still goes through `cmd.exe` (`_popen`).
- `sys.rename` replaces an existing target, as POSIX `rename` does.
- `sys.read_stdin` / `sys.read_line` put stdin in binary mode, so `\r\n` is
  not rewritten under a JSON-RPC peer.

**Why the seed compiler is not ported to Windows.** `driver.c` builds shell
command lines (`sh -c`, `2>/dev/null`, `fork`/`execl`, `mkstemp`). The
self-hosting plan replaces it with a Loam driver that uses `sys.spawn`, and
a fresh Windows checkout bootstraps from the committed generated C of the
Loam compiler — so porting the C driver would be work thrown away
(docs/self-hosting-loam.md, essentialism rule).
