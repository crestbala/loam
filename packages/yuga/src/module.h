/**
 * module.h — one compiled Yuga file in a session.
 *
 * The session holds up to YUGA_MAX_MODULES. `loading` is set while this
 * module is being parsed so import cycles can be reported.
 */
#ifndef YUGA_MODULE_H
#define YUGA_MODULE_H

#include "ast.h"

#define YUGA_MAX_MODULES 256

typedef struct {
    char *name;   /* module alias: stem, or `foo` from import "std:foo" */
    char *path;   /* filesystem path used to load it */
    char *src;    /* owned source text */
    AstNode *ast; /* program root */
    int loading;  /* 1 while parsing (cycle detect) */
} YugaModule;

#endif
