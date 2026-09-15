/**
 * borrowck.h — ownership, moves, and exclusive vs shared borrows.
 *
 * Statement-level temps: a borrow that is not stored in a `let` ends at
 * the next statement. Stored `let a = &mut x` lasts until `a` leaves
 * scope. Not NLL. Returns 1 if any error.
 */
#ifndef LOAM_BORROWCK_H
#define LOAM_BORROWCK_H

#include "../module.h"

int borrowck_modules(LoamModule *mods, int nmods);

#endif
