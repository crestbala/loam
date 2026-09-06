/**
 * compile.c — load modules from disk and run lexer/parser/sema.
 *
 * Import `"std:name"` → YUGA_STD_DIR/name.yuga. Relative paths are from the
 * importing file. Cycles and missing files are errors. After all modules
 * parse, typecheck → borrowck → boundscheck.
 */
#include "compile.h"
#include "lexer.h"
#include "parser.h"
#include "sema/typecheck.h"
#include "sema/borrowck.h"
#include "sema/boundscheck.h"
#include "threadcheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef YUGA_STD_DIR
#define YUGA_STD_DIR "std"
#endif

static YugaSession *CUR;
static const char *main_override_src;
static const char *main_override_path;

/** Read the whole file into a malloc'd NUL-terminated buffer. NULL on fail. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) sz = 0;
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) yuga_fatal("out of memory");
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int file_exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return yuga_dup(".");
    if (slash == path) return yuga_dup("/");
    return yuga_dupn(path, (size_t)(slash - path));
}

/* Collapse "." / ".." so the same file is one module regardless of import spelling. */
static char *normalize_path(const char *in) {
    char parts[64][256];
    int n = 0;
    int abs = in[0] == '/';
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n > 0 && strcmp(parts[n - 1], "..") != 0) n--;
            else if (!abs && n < 64) {
                memcpy(parts[n], "..", 3);
                n++;
            }
            continue;
        }
        if (n >= 64 || len >= 255) return yuga_dup(in);
        memcpy(parts[n], start, len);
        parts[n][len] = '\0';
        n++;
    }
    char out[1024];
    size_t o = 0;
    if (abs) out[o++] = '/';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(parts[i]);
        if (o && out[o - 1] != '/') {
            if (o + 1 >= sizeof out) return yuga_dup(in);
            out[o++] = '/';
        }
        if (o + len >= sizeof out) return yuga_dup(in);
        memcpy(out + o, parts[i], len);
        o += len;
    }
    if (o == 0) {
        out[o++] = abs ? '/' : '.';
    }
    out[o] = '\0';
    return yuga_dup(out);
}

static char *stem_of(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n > 5 && strcmp(base + n - 5, ".yuga") == 0) n -= 5;
    else {
        const char *dot = strrchr(base, '.');
        if (dot) n = (size_t)(dot - base);
    }
    return yuga_dupn(base, n);
}

/** Map an import spec to a filesystem path, or NULL and an error. */
static char *resolve_import(const char *importer, AstNode *im) {
    char path[1024];
    const char *spec = im->as.import.path;
    if (!spec) {
        yuga_error(im->loc, "import requires a quoted path, e.g. import \"std:zeus\"");
        return NULL;
    }
    if (strncmp(spec, "std:", 4) == 0) {
        snprintf(path, sizeof path, "%s/%s.yuga", YUGA_STD_DIR, spec + 4);
        if (!file_exists(path)) {
            yuga_error(im->loc, "cannot find std module '%s'", spec + 4);
            return NULL;
        }
        return normalize_path(path);
    }
    if (spec[0] == '/')
        snprintf(path, sizeof path, "%s", spec);
    else {
        char *dir = dir_of(importer);
        snprintf(path, sizeof path, "%s/%s", dir, spec);
        free(dir);
    }
    if (!file_exists(path)) {
        yuga_error(im->loc, "cannot open imported file '%s'", path);
        return NULL;
    }
    return normalize_path(path);
}

static int load_module(const char *path, const char *name);

static int load_imports(YugaModule *m) {
    AstNode *p = m->ast;
    if (!p) return 1;
    for (size_t i = 0; i < p->as.program.import_count; i++) {
        AstNode *im = p->as.program.imports[i];
        char *ip = resolve_import(m->path, im);
        if (!ip) return 1;
        int rc = load_module(ip, im->as.import.alias);
        free(ip);
        if (rc) return 1;
    }
    return 0;
}

/** Recursively load `path` as module `name`. 0 = ok. Detects cycles. */
static int load_module(const char *path, const char *name) {
    for (int i = 0; i < CUR->nmods; i++) {
        if (CUR->mods[i].path && strcmp(CUR->mods[i].path, path) == 0) {
            if (CUR->mods[i].loading) {
                SourceLoc loc = {path, 1, 1, 0, 0};
                yuga_error(loc, "import cycle involving '%s'", path);
                return 1;
            }
            return 0;
        }
    }
    if (CUR->nmods >= YUGA_MAX_MODULES) {
        SourceLoc loc = {path, 1, 1, 0, 0};
        yuga_error(loc, "too many modules");
        return 1;
    }
    int idx = CUR->nmods++;
    CUR->mods[idx].name = yuga_dup(name);
    CUR->mods[idx].path = yuga_dup(path);
    CUR->mods[idx].loading = 1;
    CUR->mods[idx].ast = NULL;
    if (main_override_src && main_override_path && strcmp(path, main_override_path) == 0)
        CUR->mods[idx].src = yuga_dup(main_override_src);
    else
        CUR->mods[idx].src = read_file(path);
    if (!CUR->mods[idx].src) {
        SourceLoc loc = {path, 1, 1, 0, 0};
        yuga_error(loc, "could not read '%s'", path);
        return 1;
    }
    Lexer lex;
    lexer_init(&lex, CUR->mods[idx].src, CUR->mods[idx].path);
    Parser p;
    parser_init(&p, &lex);
    CUR->mods[idx].ast = parser_parse(&p);
    if (p.had_error || !CUR->mods[idx].ast) return 1;
    CUR->mods[idx].ast->as.program.mod_name = yuga_dup(name);
    if (name && strcmp(name, "http") != 0) {
        AstNode *ast = CUR->mods[idx].ast;
        int has_proto = 0, has_http = 0;
        for (size_t i = 0; i < ast->as.program.decl_count; i++) {
            AstNode *d = ast->as.program.decls[i];
            if (d->kind == AST_STRUCT_DECL && d->as.strct.is_proto) has_proto = 1;
        }
        for (size_t i = 0; i < ast->as.program.import_count; i++) {
            const char *spec = ast->as.program.imports[i]->as.import.path;
            if (spec && strcmp(spec, "std:http") == 0) has_http = 1;
        }
        if (has_proto && !has_http) {
            SourceLoc loc = ast->loc;
            AstNode *im = ast_import(yuga_dup("http"), yuga_dup("std:http"), loc);
            size_t ni = ast->as.program.import_count;
            AstNode **imps = (AstNode **)realloc(ast->as.program.imports, (ni + 1) * sizeof(AstNode *));
            if (!imps) yuga_fatal("out of memory");
            imps[ni] = im;
            ast->as.program.imports = imps;
            ast->as.program.import_count = ni + 1;
        }
    }
    if (load_imports(&CUR->mods[idx])) return 1;
    CUR->mods[idx].loading = 0;
    return 0;
}

void yuga_session_init(YugaSession *s) { memset(s, 0, sizeof(*s)); }

/** Free ASTs, source buffers, and typecheck state. */
void yuga_session_free(YugaSession *s) {
    if (!s) return;
    for (int i = 0; i < s->nmods; i++) {
        ast_free(s->mods[i].ast);
        free(s->mods[i].name);
        free(s->mods[i].path);
        free(s->mods[i].src);
        s->mods[i].ast = NULL;
        s->mods[i].name = NULL;
        s->mods[i].path = NULL;
        s->mods[i].src = NULL;
    }
    s->nmods = 0;
    yuga_diag_free(s->diags, s->ndiag);
    s->diags = NULL;
    s->ndiag = 0;
    typecheck_cleanup();
}

/** Parse, typecheck, borrowck, boundscheck. Returns 0 on success. */
int yuga_session_check(YugaSession *s, const char *path, const char *src) {
    yuga_session_free(s);
    yuga_session_init(s);
    CUR = s;
    main_override_src = src;
    main_override_path = src ? path : NULL;
    yuga_diag_capture(1);
    yuga_diag_clear();

    char *stem = stem_of(path);
    int load_err = load_module(path, stem);
    free(stem);

    if (!load_err && s->nmods > 0) {
        if (typecheck_modules(s->mods, s->nmods) != 0) { /* errors captured */ }
        else {
            /* Send discipline: validate every thread.spawn callback. */
            threadcheck_modules(s->mods, s->nmods);
            borrowck_modules(s->mods, s->nmods);
            boundscheck_modules(s->mods, s->nmods);
        }
    }

    yuga_diag_take(&s->diags, &s->ndiag);
    yuga_diag_capture(0);
    CUR = NULL;
    main_override_src = NULL;
    main_override_path = NULL;
    return s->ndiag > 0 || load_err;
}
