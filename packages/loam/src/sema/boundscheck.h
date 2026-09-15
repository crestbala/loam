/**
 * boundscheck.h — prove constant indexes in range.
 *
 * For `a[i]` where `i` is a number literal and `a` is `[N]T`, sets
 * ASTF_INDEX_SAFE so codegen skips loam_idx. All other indexes trap
 * at run time. Always returns 0 (does not fail the compile).
 */
#ifndef LOAM_BOUNDSCHECK_H
#define LOAM_BOUNDSCHECK_H

#include "../module.h"

int boundscheck_modules(LoamModule *mods, int nmods);

#endif
