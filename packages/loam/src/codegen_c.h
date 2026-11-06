/**
 * codegen_c.h — emit gnu99 C for a typechecked Loam program.
 *
 * Inlines `rt_path` (loam_rt.h). Empty std fns are not emitted (intrinsics
 * in the runtime). Capturing closures become stack envs; Box uses malloc.
 */
#ifndef LOAM_CODEGEN_C_H
#define LOAM_CODEGEN_C_H

#include <stdio.h>
#include "module.h"

/**
 * Write a complete C translation unit to `out`.
 * `mods[0]` is the main module. `rt_path` is copied into the file.
 */
void codegen_emit_c(FILE *out, LoamModule *mods, int nmods, const char *rt_path);

/**
 * Phase 12: `loam test` mode. When enabled, `codegen_emit_c` skips the
 * program's own `main` and emits a runner `main` that calls every `#[test]`
 * fn, printing each test before it runs so a trap names the failing one.
 */
void codegen_set_test_mode(int on);

/**
 * `#[server]` splitting: when on (client target), a `#[server]` fn's body is
 * replaced by a trap stub so it is absent from the emitted program.
 */
void codegen_set_server_split(int on);

/**
 * Wasm publish mode. Panic locations and `#line` names that sit under
 * `prefix` are emitted relative to it. Any other absolute path is reduced
 * to its basename, so a home directory or temp folder never lands in the
 * binary. `NULL` keeps the path the compiler loaded.
 */
void codegen_set_path_strip(const char *prefix);

/**
 * Shard mode, for `LOAM_SHARD` builds: the output stays one file, marked so the
 * driver can cut it into separately compiled units. Everything before the first
 * `@S name` comment is a header every unit includes, each of those comments starts
 * one definition, and an `@D` comment starts the data only `LOAM_SHARD_DEFS`
 * compiles. The file is still one valid translation unit with that macro defined.
 */
void codegen_set_shard(int on);

/**
 * Batch mode, for `LOAM_BATCH` builds: the program imports its fixtures as
 * `case0`, `case1`, ... and runs one per process, chosen by `LOAM_CASE`. Only the
 * modules that case can reach run their initializers, so it starts up as it would
 * alone. `nob test` uses this to build hundreds of fixtures as one binary.
 */
void codegen_set_batch(int on);

#endif
