/**
 * module.h — one compiled Loam file in a session.
 *
 * The session holds up to LOAM_MAX_MODULES. `loading` is set while this
 * module is being parsed so import cycles can be reported.
 */
#ifndef LOAM_MODULE_H
#define LOAM_MODULE_H

#include "ast.h"

#define LOAM_MAX_MODULES 256

typedef struct {
    char *name;   /* module alias: stem, or `foo` from import "std:foo" */
    char *path;   /* filesystem path used to load it */
    char *src;    /* owned source text */
    AstNode *ast; /* program root */
    int loading;  /* 1 while parsing (cycle detect) */
} LoamModule;

#endif
