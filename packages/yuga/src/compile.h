/**
 * compile.h — load a program and its imports, then run all frontend passes.
 *
 * Pipeline: parse each module → typecheck → borrowck → boundscheck.
 * Does not invoke codegen or `cc`; that is driver.c / yuga-lsp.
 */
#ifndef YUGA_COMPILE_H
#define YUGA_COMPILE_H

#include "module.h"
#include "diagnostics.h"

/** Upper bound on language-std + YUGA_PATH roots searched for `std:` modules. */
#define YUGA_MAX_STD_DIRS 8

/** Directories searched for `std:` modules, in order (language std first).
 *  Pointers alias static storage; valid until the next call. Returns the count. */
int yuga_std_dirs(const char **out, int max);

/** 1 if `path` lives under any directory returned by yuga_std_dirs(). */
int yuga_is_std_path(const char *path);

/** All modules of one compile, plus diagnostics from the last check. */
typedef struct {
    YugaModule mods[YUGA_MAX_MODULES];
    int nmods;
    YugaDiag *diags;
    int ndiag;
} YugaSession;

void yuga_session_init(YugaSession *s);
void yuga_session_free(YugaSession *s);

/**
 * Analyze `path` as the main module. If `src` is non-NULL, use it as that
 * file's contents (imports still load from disk). Returns 0 if no errors.
 * Fills s->diags (also printed unless capture is on).
 */
int yuga_session_check(YugaSession *s, const char *path, const char *src);

#endif
