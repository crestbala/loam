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

/** Upper bound on language-std directories. `std:` does not search LOAM_PATH. */
#define LOAM_MAX_STD_DIRS 8

/** Directories searched for `std:` modules. Language standard library only.
 *  Pointers alias static storage; valid until the next call. Returns the count. */
int loam_std_dirs(const char **out, int max);

/** 1 if `path` lives under the language std or a framework package's `src/`
 *  (LOAM_PATH; a leftover `std/` still counts). Those files stay on the
 *  runtime-seam path even though `std:` no longer names them. */
int loam_is_std_path(const char *path);

/** Load package roots for `anchor` (the entry file). Vendor + lock first, then
 *  the nearest `loam.toml` workspace, then `LOAM_PATH` as an override for names
 *  still missing. Call once per compile, before resolving imports. */
void loam_packages_discover(const char *anchor);

/** 1 if a discovered package is named `name`. */
int loam_package_known(const char *name);

/** Entry file of package `name`, or 0. On failure a vendor checksum mismatch
 *  is reported by `loam_package_error`. */
int loam_package_entry(const char *name, char *out, size_t outsz);

/** Resolve `package:module` (`zeus:zeuscore/arena`) to a path, or 0. */
int loam_package_module(const char *pkg, const char *mod, char *out, size_t outsz);

/** Message from the last failed package lookup, or "". */
const char *loam_package_error(void);

/** Fill `buf` with why `name` did not resolve. */
void loam_package_missing(const char *name, char *buf, size_t sz);

/** 1 if `pkgdir` (a `vendor/<name>` directory) matches `loam.lock`. A missing
 *  checksum column passes. Mismatch returns 0 and sets `loam_package_error`. */
int loam_vendor_check(const char *pkgdir);

/** Package name owning `path` ("std" for the language library), or NULL.
 *  Pointer aliases static storage from the last discover. */
const char *loam_package_label(const char *path);

/** Discovered package names, in registration order. */
int loam_package_count(void);
const char *loam_package_name(int i);
/** Directory of discovered package `i`, or NULL. Aliases static storage. */
const char *loam_package_dir(int i);

/** Drop the last package-lookup message. */
void loam_package_clear_error(void);

/** When set, a full package import in the entry file warns. Release builds. */
void loam_set_release_imports(int on);

/** All modules of one compile, plus diagnostics from the last check. */
typedef struct {
    LoamModule mods[LOAM_MAX_MODULES];
    int nmods;
    LoamDiag *diags;
    int ndiag;
    /* Warnings from the same check; never make it fail. */
    LoamDiag *warns;
    int nwarn;
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
