/**
 * compile.c — load modules from disk and run lexer/parser/sema.
 *
 * Import `"std:name"` → `<std dir>/name.<ext>`, searched across LOAM_STD_DIR
 * and each LOAM_PATH root's `std/` subdir. Relative paths are from the
 * importing file. Cycles and missing files are errors. After all modules
 * parse, typecheck → borrowck → boundscheck.
 */
#include "compile.h"
#include "ext.h"
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

#ifndef LOAM_STD_DIR
#define LOAM_STD_DIR "std"
#endif

/* Colon-separated framework roots searched for `std:` modules after the
   language std. Build-time default; a non-empty LOAM_PATH env variable is
   prepended to it (see std_search_path). */
#ifndef LOAM_PATH
#define LOAM_PATH ""
#endif

static LoamSession *CUR;
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
    if (!buf) loam_fatal("out of memory");
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int file_exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int path_under(const char *path, const char *dir) {
    size_t d = dir ? strlen(dir) : 0;
    if (!path || !dir || d == 0) return 0;
    return strncmp(path, dir, d) == 0 && (path[d] == '/' || path[d] == '\0');
}

/* Env LOAM_PATH (if set) followed by the build-time default, so a custom path
   adds roots without dropping zeus/http/maya. */
static const char *std_search_path(char *buf, size_t sz) {
    const char *env = getenv("LOAM_PATH");
    if (env && env[0] && strlen(LOAM_PATH) > 0) snprintf(buf, sz, "%s:%s", env, LOAM_PATH);
    else if (env && env[0]) snprintf(buf, sz, "%s", env);
    else snprintf(buf, sz, "%s", LOAM_PATH);
    return buf;
}

int loam_std_dirs(const char **out, int max) {
    static char buf[1024];
    static char dirs[LOAM_MAX_STD_DIRS][1024];
    int n = 0;
    if (n < max) out[n++] = LOAM_STD_DIR;
    const char *roots = std_search_path(buf, sizeof buf);
    const char *p = roots;
    while (*p && n < max) {
        while (*p == ':') p++;
        if (!*p) break;
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > 0 && len + 5 < sizeof dirs[0]) {
            int k = snprintf(dirs[n], sizeof dirs[0], "%.*s/std", (int)len, p);
            if (k > 0 && k < (int)sizeof dirs[0]) { out[n] = dirs[n]; n++; }
        }
        if (!end) break;
        p = end + 1;
    }
    return n;
}

int loam_is_std_path(const char *path) {
    const char *dirs[LOAM_MAX_STD_DIRS];
    int n = loam_std_dirs(dirs, LOAM_MAX_STD_DIRS);
    for (int i = 0; i < n; i++)
        if (path_under(path, dirs[i])) return 1;
    return 0;
}

/* First existing `<std dir>/<name><ext>`, language std first. Inside a root the
   extensions are tried in ext.h order, so one std dir keeps beating the next. */
static int std_module_lookup(const char *name, char *out, size_t outsz) {
    const char *dirs[LOAM_MAX_STD_DIRS];
    int n = loam_std_dirs(dirs, LOAM_MAX_STD_DIRS);
    for (int i = 0; i < n; i++)
        for (size_t e = 0; e < LOAM_EXT_COUNT; e++) {
            snprintf(out, outsz, "%s/%s%s", dirs[i], name, loam_ext_name(e));
            if (file_exists(out)) return 1;
        }
    return 0;
}

/*
 * `import "pkg:name"` — a vendored package. Walk up from the entry file to the
 * nearest directory holding `vendor/name/`, then take `name.<ext>` (or
 * `main.<ext>`). `zeus pkg sync` materializes that tree from `yuga.deps`.
 */
static int pkg_module_lookup(const char *name, char *out, size_t outsz) {
    if (!name[0] || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) return 0;
    const char *anchor = (CUR && CUR->nmods > 0) ? CUR->mods[0].path : NULL;
    if (!anchor) return 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", anchor);
    for (int depth = 0; depth < 64; depth++) {
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        else dir[0] = '\0';
        /* The package's own file beats a generic `main`, whichever spelling. */
        for (int which = 0; which < 2; which++) {
            const char *file = which ? "main" : name;
            for (size_t e = 0; e < LOAM_EXT_COUNT; e++) {
                char cand[1200];
                if (dir[0])
                    snprintf(cand, sizeof cand, "%s/vendor/%s/%s%s", dir, name, file, loam_ext_name(e));
                else
                    snprintf(cand, sizeof cand, "vendor/%s/%s%s", name, file, loam_ext_name(e));
                if (file_exists(cand)) { snprintf(out, outsz, "%s", cand); return 1; }
            }
        }
        if (dir[0] == '\0') break;
    }
    return 0;
}

static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return loam_dup(".");
    if (slash == path) return loam_dup("/");
    return loam_dupn(path, (size_t)(slash - path));
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
        if (n >= 64 || len >= 255) return loam_dup(in);
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
            if (o + 1 >= sizeof out) return loam_dup(in);
            out[o++] = '/';
        }
        if (o + len >= sizeof out) return loam_dup(in);
        memcpy(out + o, parts[i], len);
        o += len;
    }
    if (o == 0) {
        out[o++] = abs ? '/' : '.';
    }
    out[o] = '\0';
    return loam_dup(out);
}

/* Module alias for a path: stem, without directory or source extension. */
static char *stem_of(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return loam_dupn(base, loam_stem_len(base, strlen(base)));
}

/** Map an import spec to a filesystem path, or NULL and an error. */
static char *resolve_import(const char *importer, AstNode *im) {
    char path[1024];
    const char *spec = im->as.import.path;
    if (!spec) {
        loam_error(im->loc, "import requires a quoted path, e.g. import \"std:zeus\"");
        return NULL;
    }
    if (strncmp(spec, "std:", 4) == 0) {
        if (!std_module_lookup(spec + 4, path, sizeof path)) {
            loam_error(im->loc, "cannot find std module '%s'", spec + 4);
            return NULL;
        }
        return normalize_path(path);
    }
    if (strncmp(spec, "pkg:", 4) == 0) {
        if (!pkg_module_lookup(spec + 4, path, sizeof path)) {
            loam_error(im->loc, "cannot find package '%s' (run `zeus pkg sync`)", spec + 4);
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
        loam_error(im->loc, "cannot open imported file '%s'", path);
        return NULL;
    }
    return normalize_path(path);
}

static int load_module(const char *path, const char *name);

static int load_imports(LoamModule *m) {
    AstNode *p = m->ast;
    if (!p) return 1;
    for (size_t i = 0; i < p->as.program.import_count; i++) {
        AstNode *im = p->as.program.imports[i];
        char *ip = resolve_import(m->path, im);
        if (!ip) return 1;
        im->as.import.resolved = loam_dup(ip);
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
                loam_error(loc, "import cycle involving '%s'", path);
                return 1;
            }
            return 0;
        }
    }
    if (CUR->nmods >= LOAM_MAX_MODULES) {
        SourceLoc loc = {path, 1, 1, 0, 0};
        loam_error(loc, "too many modules");
        return 1;
    }
    int idx = CUR->nmods++;
    CUR->mods[idx].name = loam_dup(name);
    CUR->mods[idx].path = loam_dup(path);
    CUR->mods[idx].loading = 1;
    CUR->mods[idx].ast = NULL;
    if (main_override_src && main_override_path && strcmp(path, main_override_path) == 0)
        CUR->mods[idx].src = loam_dup(main_override_src);
    else
        CUR->mods[idx].src = read_file(path);
    if (!CUR->mods[idx].src) {
        SourceLoc loc = {path, 1, 1, 0, 0};
        loam_error(loc, "could not read '%s'", path);
        return 1;
    }
    Lexer lex;
    lexer_init(&lex, CUR->mods[idx].src, CUR->mods[idx].path);
    Parser p;
    parser_init(&p, &lex);
    CUR->mods[idx].ast = parser_parse(&p);
    if (p.had_error || !CUR->mods[idx].ast) return 1;
    CUR->mods[idx].ast->as.program.mod_name = loam_dup(name);
    if (name && strcmp(name, "http") != 0) {
        AstNode *ast = CUR->mods[idx].ast;
        int has_proto = 0, has_http = 0;
        int has_json = 0, has_std_json = 0;
        for (size_t i = 0; i < ast->as.program.decl_count; i++) {
            AstNode *d = ast->as.program.decls[i];
            if (d->kind == AST_STRUCT_DECL && d->as.strct.is_proto) has_proto = 1;
            if (d->kind == AST_STRUCT_DECL && d->as.strct.is_json) has_json = 1;
        }
        for (size_t i = 0; i < ast->as.program.import_count; i++) {
            const char *spec = ast->as.program.imports[i]->as.import.path;
            if (spec && strcmp(spec, "std:http") == 0) has_http = 1;
            if (spec && strcmp(spec, "std:json") == 0) has_std_json = 1;
        }
        if (has_proto && !has_http) {
            SourceLoc loc = ast->loc;
            AstNode *im = ast_import(loam_dup("http"), loam_dup("std:http"), loc);
            size_t ni = ast->as.program.import_count;
            AstNode **imps = (AstNode **)realloc(ast->as.program.imports, (ni + 1) * sizeof(AstNode *));
            if (!imps) loam_fatal("out of memory");
            imps[ni] = im;
            ast->as.program.imports = imps;
            ast->as.program.import_count = ni + 1;
        }
        /* `#[json]` derives call the `json` module, so import it implicitly. */
        if (has_json && !has_std_json) {
            SourceLoc loc = ast->loc;
            AstNode *im = ast_import(loam_dup("json"), loam_dup("std:json"), loc);
            size_t ni = ast->as.program.import_count;
            AstNode **imps = (AstNode **)realloc(ast->as.program.imports, (ni + 1) * sizeof(AstNode *));
            if (!imps) loam_fatal("out of memory");
            imps[ni] = im;
            ast->as.program.imports = imps;
            ast->as.program.import_count = ni + 1;
        }
    }
    if (load_imports(&CUR->mods[idx])) return 1;
    CUR->mods[idx].loading = 0;
    return 0;
}

void loam_session_init(LoamSession *s) { memset(s, 0, sizeof(*s)); }

/** Free ASTs, source buffers, and typecheck state. */
void loam_session_free(LoamSession *s) {
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
    loam_diag_free(s->diags, s->ndiag);
    s->diags = NULL;
    s->ndiag = 0;
    typecheck_cleanup();
}

/** Parse, typecheck, borrowck, boundscheck. Returns 0 on success. */
int loam_session_check(LoamSession *s, const char *path, const char *src) {
    loam_session_free(s);
    loam_session_init(s);
    CUR = s;
    main_override_src = src;
    main_override_path = src ? path : NULL;
    loam_diag_capture(1);
    loam_diag_clear();

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

    loam_diag_take(&s->diags, &s->ndiag);
    loam_diag_capture(0);
    CUR = NULL;
    main_override_src = NULL;
    main_override_path = NULL;
    return s->ndiag > 0 || load_err;
}
