/**
 * compile.h — load a program and its imports, then run all frontend passes.
 *
 * Pipeline: parse each module → typecheck → borrowck → boundscheck.
 * Does not invoke codegen or `cc`; that is driver.c / loam-lsp.
 */
#ifndef LOAM_COMPILE_H
#define LOAM_COMPILE_H

#include "module.h"
#include "diagnostics.h"

/** Upper bound on language-std + LOAM_PATH roots searched for `std:` modules. */
#define LOAM_MAX_STD_DIRS 8

/** Directories searched for `std:` modules, in order (language std first).
 *  Pointers alias static storage; valid until the next call. Returns the count. */
int loam_std_dirs(const char **out, int max);

/** 1 if `path` lives under any directory returned by loam_std_dirs(). */
int loam_is_std_path(const char *path);

/** Resolve the `package:module` spec form to a path, or 0. The package is the
 *  LOAM_PATH root whose directory is named `package`; the module is looked up in
 *  that root's `std` dir, and may itself be a path (`zeus:zeuscore/arena`).
 *  Writes `out` only on success, so the language server can offer exactly the
 *  package-qualified specs that resolve. */
int loam_package_module(const char *pkg, const char *mod, char *out, size_t outsz);

/** All modules of one compile, plus diagnostics from the last check. */
typedef struct {
    LoamModule mods[LOAM_MAX_MODULES];
    int nmods;
    LoamDiag *diags;
    int ndiag;
} LoamSession;

void loam_session_init(LoamSession *s);
void loam_session_free(LoamSession *s);

/**
 * Analyze `path` as the main module. If `src` is non-NULL, use it as that
 * file's contents (imports still load from disk). Returns 0 if no errors.
 * Fills s->diags (also printed unless capture is on).
 */
int loam_session_check(LoamSession *s, const char *path, const char *src);

#endif
