/**
 * threadcheck.h — the Send discipline for std:thread (language threads).
 *
 * Runs after typecheck (resolved calls, typed closure captures) and before
 * borrowck/codegen. Verifies every `thread.spawn` callback: closure captures
 * are Send, the callback and its callees touch no module state and no C-seam
 * symbol outside the pure allowlist, and fn values are statically traceable.
 * Errors are reported through loam_error (captured by the session).
 */
#ifndef LOAM_THREADCHECK_H
#define LOAM_THREADCHECK_H

#include "ast.h"
#include "module.h"

/** Check all spawn sites across `mods`. Returns the number of errors. */
int threadcheck_modules(LoamModule *mods, int nmods);

#endif
