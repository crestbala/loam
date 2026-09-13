/**
 * dce.h — reachability-based dead decl elimination for codegen.
 *
 * Only std-module declarations that the program can actually reach are
 * emitted as C: the entry module and everything it calls, everything the
 * C runtime/hosts call by name, and module-level initializers. Widgets,
 * codecs, and helpers a program never touches stop inflating generated C,
 * `cc` wall time, and wasm bundle size (per-widget opt-in, Phase 7).
 *
 * Runs after typecheck (bindings are resolved) and before codegen emission.
 * Typecheck, borrowck, boundscheck, IR lowering, and verification still see
 * every declaration, so errors in unused code keep failing loudly.
 */
#ifndef YUGA_DCE_H
#define YUGA_DCE_H

#include "ast.h"
#include "module.h"

/** Compute the reachable set for `mods`. Safe to call once per emit. */
void yuga_dce_run(YugaModule *mods, int nmods);

/** `yugac test`: keep every fn in the `test` std module, since the generated
 *  runner calls `test.begin` / `test.ok` / `test.summary` by name and nothing
 *  in the Yuga call graph reaches them. Call before yuga_dce_run. */
void yuga_dce_set_test_mode(int on);

/** 1 = emit this fn declaration's prototype/body/tramp (and its clones). */
int yuga_dce_keep(const AstNode *fn_decl);

#endif
