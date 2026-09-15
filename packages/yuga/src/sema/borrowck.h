/**
 * borrowck.h — ownership, moves, and exclusive vs shared borrows.
 *
 * Statement-level temps: a borrow that is not stored in a `let` ends at
 * the next statement. Stored `let a = &mut x` lasts until `a` leaves
 * scope. Not NLL. Returns 1 if any error.
 */
#ifndef YUGA_BORROWCK_H
#define YUGA_BORROWCK_H

#include "../module.h"

int borrowck_modules(YugaModule *mods, int nmods);

#endif
