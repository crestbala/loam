/**
 * codegen_c.h — emit gnu99 C for a typechecked Yuga program.
 *
 * Inlines `rt_path` (yuga_rt.h). Empty std fns are not emitted (intrinsics
 * in the runtime). Capturing closures become stack envs; Box uses malloc.
 */
#ifndef YUGA_CODEGEN_C_H
#define YUGA_CODEGEN_C_H

#include <stdio.h>
#include "module.h"

/**
 * Write a complete C translation unit to `out`.
 * `mods[0]` is the main module. `rt_path` is copied into the file.
 */
void codegen_emit_c(FILE *out, YugaModule *mods, int nmods, const char *rt_path);

/**
 * Phase 12: `yugac test` mode. When enabled, `codegen_emit_c` skips the
 * program's own `main` and emits a runner `main` that calls every `#[test]`
 * fn, printing each test before it runs so a trap names the failing one.
 */
void codegen_set_test_mode(int on);

#endif
