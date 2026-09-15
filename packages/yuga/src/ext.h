/**
 * ext.h — source-file extensions accepted by the compiler.
 *
 * `.loam` is canonical and `.loa` is an accepted short alias. There is no
 * legacy spelling: the pre-rename `.yuga` extension is not recognized.
 *
 * All lookups try the extensions in that order, so when a module exists under
 * both spellings the canonical one wins.
 */
#ifndef YUGA_EXT_H
#define YUGA_EXT_H

#include <stddef.h>
#include <string.h>

#define YUGA_EXT ".loam"
#define YUGA_EXT_SHORT ".loa"

/** Number of extensions accepted by yuga_ext_name / yuga_ext_size. */
#define YUGA_EXT_COUNT 2

/** The `i`-th accepted extension, or NULL past the end. */
static inline const char *yuga_ext_name(size_t i) {
    switch (i) {
    case 0: return YUGA_EXT;
    case 1: return YUGA_EXT_SHORT;
    default: return NULL;
    }
}

/** Length of the `i`-th accepted extension (0 past the end). */
static inline size_t yuga_ext_size(size_t i) {
    const char *e = yuga_ext_name(i);
    return e ? strlen(e) : 0;
}

/**
 * Length of the accepted extension at the end of `base[0..n)`, or 0 if the
 * name has none. `base` must be a file name, not a path. A name that is only
 * an extension has an empty stem and counts as having none.
 */
static inline size_t yuga_ext_at(const char *base, size_t n) {
    for (size_t i = 0; i < YUGA_EXT_COUNT; i++) {
        size_t e = yuga_ext_size(i);
        if (n > e && memcmp(base + n - e, yuga_ext_name(i), e) == 0) return e;
    }
    return 0;
}

/**
 * Stem length of a file name: strips a recognized extension, otherwise
 * everything from the last `.`, so `a.b.c` → 1. A name with no dot is taken
 * whole. Use this when the stem names an output artifact or a module alias
 * that was derived from a path.
 */
static inline size_t yuga_stem_len(const char *base, size_t n) {
    size_t e = yuga_ext_at(base, n);
    if (e) return n - e;
    for (size_t i = n; i > 0; i--)
        if (base[i - 1] == '.') return i - 1;
    return n;
}

/**
 * Stem length stripping only a recognized extension, with no dot fallback, so
 * `a.b.c` stays `a.b.c`. Use this when the name is an import spec, where any
 * dotted prefix is part of the module name.
 */
static inline size_t yuga_stem_len_ext(const char *base, size_t n) {
    return n - yuga_ext_at(base, n);
}

#endif
