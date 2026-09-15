/**
 * boundscheck.h — prove constant indexes in range.
 *
 * For `a[i]` where `i` is a number literal and `a` is `[N]T`, sets
 * ASTF_INDEX_SAFE so codegen skips yuga_idx. All other indexes trap
 * at run time. Always returns 0 (does not fail the compile).
 */
#ifndef YUGA_BOUNDSCHECK_H
#define YUGA_BOUNDSCHECK_H

#include "../module.h"

int boundscheck_modules(YugaModule *mods, int nmods);

#endif
