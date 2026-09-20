/**
 * lsp.c — loam-lsp: Language Server Protocol over stdin/stdout.
 *
 * Full document sync, plus incremental contentChanges when a client sends them.
 * didOpen/didChange recompile the editor buffer (never a stale on-disk file)
 * and publish diagnostics (correct file URI, token span). Hover,
 * go-to-definition, completion (modules, methods, locals, keywords), and
 * semantic tokens (syntax highlighting) read the last session's typed AST.
 */
#include "compile.h"
#include "sema/type.h"
#include "ast.h"
#include "ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <dirent.h>

static LoamSession Gsess;
static char *Guri;
static char *Gpath;
static char *Gtext;
static int Ghave;

static void load_std_names(void);
static int in_import_str(const char *src, int off);
static void import_str_prefix(const char *src, int off, char *pre, size_t cap);

/** Read exactly `n` bytes from stdin. */
static char *read_exact(size_t n) {
    char *buf = (char *)malloc(n + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < n) {
        size_t r = fread(buf + got, 1, n - got, stdin);
        if (r == 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[n] = '\0';
    return buf;
}

/** One LSP message: headers then Content-Length body. */
static char *read_message(void) {
    char line[256];
    int len = -1;
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) return NULL;
        if (line[0] == '\n' || (line[0] == '\r' && line[1] == '\n')) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            len = atoi(line + 15);
    }
    if (len < 0) return NULL;
    return read_exact((size_t)len);
}

static void send_raw(const char *json) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
}

/** Find `"key"` in a JSON object; returns pointer at the value. */
static const char *find_key(const char *json, const char *key) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat))) {
        const char *q = p + strlen(pat);
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == ':') {
            q++;
            while (*q && isspace((unsigned char)*q)) q++;
            return q;
        }
        p++;
    }
    return NULL;
}

static char *parse_json_string(const char *p) {
    if (!p || *p != '"') return NULL;
    p++;
    size_t cap = 256, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else if (e == '"') c = '"';
            else if (e == '\\') c = '\\';
            else if (e == 'u' && p[0] && p[1] && p[2] && p[3]) p += 4, c = '?';
            else c = e;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(out, cap);
            if (!nb) {
                free(out);
                return NULL;
            }
            out = nb;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return out;
}

static int parse_json_int(const char *p) {
    if (!p) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    return atoi(p);
}

static char *json_escape(const char *s) {
    if (!s) s = "";
    size_t cap = strlen(s) * 2 + 8, n = 0;
    char *o = (char *)malloc(cap);
    if (!o) return NULL;
    for (; *s; s++) {
        const char *rep = NULL;
        char tmp[8];
        if (*s == '"') rep = "\\\"";
        else if (*s == '\\') rep = "\\\\";
        else if (*s == '\n') rep = "\\n";
        else if (*s == '\r') rep = "\\r";
        else if (*s == '\t') rep = "\\t";
        else if ((unsigned char)*s < 0x20) {
            snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned char)*s);
            rep = tmp;
        } else {
            tmp[0] = *s;
            tmp[1] = 0;
            rep = tmp;
        }
        size_t rl = strlen(rep);
        if (n + rl + 1 >= cap) {
            cap *= 2;
            o = (char *)realloc(o, cap);
            if (!o) return NULL;
        }
        memcpy(o + n, rep, rl);
        n += rl;
    }
    o[n] = '\0';
    return o;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** file:// URI to a filesystem path. */
static char *uri_to_path(const char *uri) {
    if (!uri) return NULL;
    if (strncmp(uri, "file://", 7) == 0) uri += 7;
    if (strncmp(uri, "localhost", 9) == 0) uri += 9;
    size_t n = strlen(uri);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (uri[i] == '%' && i + 2 < n) {
            int hi = hexval(uri[i + 1]), lo = hexval(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[j++] = uri[i];
    }
    out[j] = '\0';
    return out;
}

static char *path_to_uri(const char *path) {
    if (!path || !path[0]) return loam_dup("file://");
    if (strncmp(path, "file://", 7) == 0) return loam_dup(path);
    size_t n = strlen(path);
    char *u = (char *)malloc(n + 8);
    if (!u) return NULL;
    snprintf(u, n + 8, "file://%s", path);
    return u;
}

static char *extract_id(const char *json) {
    const char *p = find_key(json, "id");
    if (!p) return NULL;
    if (*p == '"') {
        const char *end = p + 1;
        while (*end && *end != '"') {
            if (*end == '\\' && end[1]) end += 2;
            else end++;
        }
        if (*end == '"') end++;
        return loam_dupn(p, (size_t)(end - p));
    }
    const char *e = p;
    if (*e == '-') e++;
    while (isdigit((unsigned char)*e)) e++;
    if (e == p) return NULL;
    return loam_dupn(p, (size_t)(e - p));
}

static char *extract_method(const char *json) {
    const char *p = find_key(json, "method");
    return parse_json_string(p);
}

static void loc_lsp(SourceLoc loc, int *sl, int *sc, int *el, int *ec) {
    int line = loc.line > 0 ? loc.line : 1;
    int col = loc.col > 0 ? loc.col : 1;
    int end_line = loc.end_line > 0 ? loc.end_line : line;
    int end_col = loc.end_col > 0 ? loc.end_col : col + 1;
    *sl = line - 1;
    *sc = col - 1;
    *el = end_line - 1;
    *ec = end_col - 1;
    if (*sl < 0) *sl = 0;
    if (*sc < 0) *sc = 0;
    if (*el < 0) *el = 0;
    if (*ec < 0) *ec = 0;
}

static int cmp_pos(int l0, int c0, int l1, int c1) {
    if (l0 != l1) return l0 - l1;
    return c0 - c1;
}

static int loc_covers(SourceLoc loc, int line0, int char0) {
    int sl, sc, el, ec;
    if (loc.line <= 0) return 0;
    loc_lsp(loc, &sl, &sc, &el, &ec);
    if (sl == el && sc == ec) return line0 == sl && char0 == sc;
    return cmp_pos(sl, sc, line0, char0) <= 0 && cmp_pos(line0, char0, el, ec) < 0;
}

static Type *peel_type(Type *t);

typedef struct {
    AstNode *node;
    AstNode *fn;
    int param_i;
} Pick;

static int pick_hit(const Pick *p) { return p && (p->node || p->param_i >= 0); }

static void pick_node(AstNode *n, int line, int col, Pick *best, AstNode *enclosing);

static void pick_maybe(AstNode *n, int line, int col, Pick *inner, AstNode *enclosing) {
    pick_node(n, line, col, inner, enclosing);
}

static int is_fn_like(AstNode *n) {
    return n && (n->kind == AST_FN_DECL || n->kind == AST_CLOSURE);
}

static void pick_node(AstNode *n, int line, int col, Pick *best, AstNode *enclosing) {
    if (!n) return;
    Pick inner;
    inner.node = NULL;
    inner.fn = is_fn_like(n) ? n : enclosing;
    inner.param_i = -1;
    AstNode *here = inner.fn;
    switch (n->kind) {
        case AST_PROGRAM:
            for (size_t i = 0; i < n->as.program.import_count; i++)
                pick_maybe(n->as.program.imports[i], line, col, &inner, here);
            for (size_t i = 0; i < n->as.program.decl_count; i++)
                pick_maybe(n->as.program.decls[i], line, col, &inner, here);
            break;
        case AST_IMPORT:
            break;
        case AST_FN_DECL:
        case AST_CLOSURE:
            here = n;
            for (size_t i = 0; i < n->as.fn.param_count; i++)
                pick_maybe(n->as.fn.params[i].type, line, col, &inner, here);
            pick_maybe(n->as.fn.ret_type, line, col, &inner, here);
            pick_maybe(n->as.fn.body, line, col, &inner, here);
            if (!pick_hit(&inner)) {
                for (size_t i = 0; i < n->as.fn.param_count; i++) {
                    if (loc_covers(n->as.fn.params[i].loc, line, col)) {
                        inner.node = NULL;
                        inner.fn = n;
                        inner.param_i = (int)i;
                        break;
                    }
                }
            }
            break;
        case AST_STRUCT_DECL:
            for (size_t i = 0; i < n->as.strct.field_count; i++)
                pick_maybe(n->as.strct.fields[i].type, line, col, &inner, here);
            break;
        case AST_VAR_DECL:
            pick_maybe(n->as.var.type, line, col, &inner, here);
            pick_maybe(n->as.var.init, line, col, &inner, here);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                pick_maybe(n->as.block.stmts[i], line, col, &inner, here);
            break;
        case AST_IF:
            pick_maybe(n->as.if_stmt.cond, line, col, &inner, here);
            pick_maybe(n->as.if_stmt.then_block, line, col, &inner, here);
            pick_maybe(n->as.if_stmt.else_block, line, col, &inner, here);
            break;
        case AST_FOR:
            pick_maybe(n->as.for_stmt.iter, line, col, &inner, here);
            pick_maybe(n->as.for_stmt.body, line, col, &inner, here);
            break;
        case AST_RETURN:
            pick_maybe(n->as.ret.expr, line, col, &inner, here);
            break;
        case AST_EXPR_STMT:
            pick_maybe(n->as.expr_stmt.expr, line, col, &inner, here);
            break;
        case AST_ASSIGN:
            pick_maybe(n->as.assign.left, line, col, &inner, here);
            pick_maybe(n->as.assign.right, line, col, &inner, here);
            break;
        case AST_BINARY:
            pick_maybe(n->as.binary.left, line, col, &inner, here);
            pick_maybe(n->as.binary.right, line, col, &inner, here);
            break;
        case AST_UNARY:
            pick_maybe(n->as.unary.operand, line, col, &inner, here);
            break;
        case AST_CALL:
            pick_maybe(n->as.call.callee, line, col, &inner, here);
            for (size_t i = 0; i < n->as.call.arg_count; i++)
                pick_maybe(n->as.call.args[i], line, col, &inner, here);
            break;
        case AST_INDEX:
        case AST_FIELD:
        case AST_DEREF:
        case AST_ADDR:
            pick_maybe(n->as.access.target, line, col, &inner, here);
            pick_maybe(n->as.access.index, line, col, &inner, here);
            break;
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++)
                pick_maybe(n->as.struct_lit.fields[i].init, line, col, &inner, here);
            break;
        case AST_ARRAY_LIT:
            pick_maybe(n->as.array_lit.elem_type, line, col, &inner, here);
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                pick_maybe(n->as.array_lit.elems[i], line, col, &inner, here);
            break;
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                pick_maybe(n->as.array_lit.elems[i], line, col, &inner, here);
            break;
        case AST_TYPE:
            pick_maybe(n->as.type.elem, line, col, &inner, here);
            for (size_t i = 0; i < n->as.type.fn_param_count; i++)
                pick_maybe(n->as.type.fn_params[i], line, col, &inner, here);
            for (size_t i = 0; i < n->as.type.targ_count; i++)
                pick_maybe(n->as.type.targs[i], line, col, &inner, here);
            break;
        default:
            break;
    }
    if (pick_hit(&inner)) {
        *best = inner;
        return;
    }
    if (loc_covers(n->loc, line, col)) {
        best->node = n;
        best->fn = here;
        best->param_i = -1;
    }
}

static char *extract_uri(const char *msg) {
    const char *td = find_key(msg, "textDocument");
    char *uri = NULL;
    if (td) uri = parse_json_string(find_key(td, "uri"));
    if (!uri) uri = parse_json_string(find_key(msg, "uri"));
    return uri;
}

static LoamModule *mod_by_path(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < Gsess.nmods; i++) {
        if (Gsess.mods[i].path && strcmp(Gsess.mods[i].path, path) == 0)
            return &Gsess.mods[i];
    }
    return NULL;
}

static LoamModule *mod_by_alias(const char *alias) {
    if (!alias) return NULL;
    for (int i = 0; i < Gsess.nmods; i++) {
        if (Gsess.mods[i].name && strcmp(Gsess.mods[i].name, alias) == 0)
            return &Gsess.mods[i];
    }
    return NULL;
}

static LoamModule *mod_for_uri(const char *uri) {
    char *path = uri ? uri_to_path(uri) : NULL;
    LoamModule *m = path ? mod_by_path(path) : NULL;
    free(path);
    if (m) return m;
    if (Gpath) m = mod_by_path(Gpath);
    if (!m && Gsess.nmods > 0) m = &Gsess.mods[0];
    return m;
}

static Pick pick_at(const char *uri, int line, int col) {
    Pick best;
    best.node = NULL;
    best.fn = NULL;
    best.param_i = -1;
    if (!Ghave) return best;
    LoamModule *m = mod_for_uri(uri);
    if (!m || !m->ast) return best;
    pick_node(m->ast, line, col, &best, NULL);
    return best;
}

static int is_module_ident(AstNode *n) {
    return n && n->kind == AST_IDENT && n->as.ident.resolved &&
           n->as.ident.resolved->kind == AST_PROGRAM;
}

static void hover_text(Pick *pk, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!pk) return;
    if (pk->param_i >= 0 && pk->fn && (size_t)pk->param_i < pk->fn->as.fn.param_count) {
        Param *p = &pk->fn->as.fn.params[pk->param_i];
        Type *ty = NULL;
        if (pk->fn->ty && pk->fn->ty->kind == TY_PROC &&
            (size_t)pk->param_i < pk->fn->ty->param_count)
            ty = pk->fn->ty->params[pk->param_i];
        snprintf(buf, cap, "%s: %s", p->name ? p->name : "?", ty ? type_name(ty) : "?");
        return;
    }
    AstNode *n = pk->node;
    if (!n) return;
    const char *ty = n->ty ? type_name(n->ty) : NULL;
    if (n->kind == AST_IDENT && n->as.ident.name) {
        if (is_module_ident(n))
            snprintf(buf, cap, "module %s", n->as.ident.name);
        else
            snprintf(buf, cap, "%s: %s", n->as.ident.name, ty ? ty : "?");
    } else if (n->kind == AST_IMPORT) {
        const char *alias = n->as.import.alias ? n->as.import.alias : "?";
        const char *path = n->as.import.path ? n->as.import.path : "";
        snprintf(buf, cap, "module %s\n%s", alias, path);
    } else if (n->kind == AST_FIELD && n->as.access.field)
        snprintf(buf, cap, "%s: %s", n->as.access.field, ty ? ty : "?");
    else if (n->kind == AST_FN_DECL && n->as.fn.name)
        snprintf(buf, cap, "%s: %s", n->as.fn.name, ty ? ty : "fn");
    else if (n->kind == AST_CLOSURE)
        snprintf(buf, cap, "closure %s", ty ? ty : "fn()");
    else if (n->kind == AST_VAR_DECL && n->as.var.name)
        snprintf(buf, cap, "%s: %s", n->as.var.name, ty ? ty : "?");
    else if (n->kind == AST_STRUCT_DECL && n->as.strct.name)
        snprintf(buf, cap, "struct %s", n->as.strct.name);
    else if (n->kind == AST_TYPE && n->as.type.name)
        snprintf(buf, cap, "%s", n->as.type.name);
    else if (ty)
        snprintf(buf, cap, "%s", ty);
}

static const char *struct_field_doc(const char *stname, const char *field) {
    if (!stname || !field) return NULL;
    for (int mi = 0; mi < Gsess.nmods; mi++) {
        AstNode *p = Gsess.mods[mi].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_STRUCT_DECL || !d->as.strct.name) continue;
            if (strcmp(d->as.strct.name, stname) != 0) continue;
            for (size_t f = 0; f < d->as.strct.field_count; f++) {
                if (d->as.strct.fields[f].name &&
                    strcmp(d->as.strct.fields[f].name, field) == 0)
                    return d->as.strct.fields[f].doc;
            }
        }
    }
    return NULL;
}

static const char *lookup_doc(Pick *pk) {
    if (!pk) return NULL;
    AstNode *n = pk->node;
    if (!n) {
        if (pk->fn && pk->fn->doc) return NULL;
        return NULL;
    }
    if (n->doc) return n->doc;
    if (n->kind == AST_IDENT) {
        if (is_module_ident(n) && n->as.ident.resolved)
            return n->as.ident.resolved->as.program.mod_doc;
        if (n->as.ident.resolved && n->as.ident.resolved->doc)
            return n->as.ident.resolved->doc;
        if (n->as.ident.resolved && n->as.ident.resolved->kind == AST_PROGRAM)
            return n->as.ident.resolved->as.program.mod_doc;
    }
    if (n->kind == AST_FIELD) {
        if (n->as.access.resolved && n->as.access.resolved->doc)
            return n->as.access.resolved->doc;
        Type *base = n->as.access.target ? peel_type(n->as.access.target->ty) : NULL;
        if (base && base->kind == TY_STRUCT)
            return struct_field_doc(base->name, n->as.access.field);
    }
    if (n->kind == AST_IMPORT) {
        LoamModule *m = mod_by_alias(n->as.import.alias);
        if (m && m->ast && m->ast->as.program.mod_doc)
            return m->ast->as.program.mod_doc;
    }
    return NULL;
}

static char *hover_alloc(Pick *pk) {
    char head[512];
    hover_text(pk, head, sizeof head);
    const char *doc = lookup_doc(pk);
    if (!head[0] && (!doc || !doc[0])) return NULL;
    if (!doc || !doc[0]) return loam_dup(head);
    size_t n = strlen(head) + strlen(doc) + 8;
    char *s = (char *)malloc(n);
    if (!s) return loam_dup(head);
    if (head[0])
        snprintf(s, n, "%s\n\n%s", head, doc);
    else
        snprintf(s, n, "%s", doc);
    return s;
}

static int module_file_loc(const char *alias, const char *path_hint, SourceLoc *out) {
    SourceLoc z = {0};
    *out = z;
    LoamModule *m = mod_by_alias(alias);
    if (!m && path_hint) m = mod_by_path(path_hint);
    if (!m || !m->path) return 0;
    out->file = m->path;
    out->line = 1;
    out->col = 1;
    out->end_line = 1;
    out->end_col = 2;
    return 1;
}

static int def_loc_of(Pick *pk, SourceLoc *out) {
    SourceLoc z = {0};
    *out = z;
    if (!pk) return 0;
    if (pk->param_i >= 0 && pk->fn && (size_t)pk->param_i < pk->fn->as.fn.param_count) {
        *out = pk->fn->as.fn.params[pk->param_i].loc;
        return out->line > 0;
    }
    AstNode *n = pk->node;
    if (!n) return 0;
    if (n->kind == AST_IMPORT)
        return module_file_loc(n->as.import.alias, NULL, out);
    if (n->kind == AST_IDENT) {
        if (n->as.ident.def_loc.line > 0) {
            *out = n->as.ident.def_loc;
            return 1;
        }
        if (n->as.ident.resolved) {
            *out = n->as.ident.resolved->loc;
            return 1;
        }
        return module_file_loc(n->as.ident.name, NULL, out);
    }
    if (n->kind == AST_FIELD && n->as.access.resolved) {
        *out = n->as.access.resolved->loc;
        return 1;
    }
    if (n->kind == AST_FN_DECL || n->kind == AST_VAR_DECL || n->kind == AST_STRUCT_DECL ||
        n->kind == AST_CLOSURE) {
        *out = n->loc;
        return 1;
    }
    return 0;
}

static int diag_for_file(LoamDiag *d, const char *path, int opened) {
    if (!d->file || !d->file[0]) return opened;
    if (path && strcmp(d->file, path) == 0) return 1;
    return 0;
}

static void send_diagnostics(const char *uri, const char *path, int opened) {
    size_t cap = 4096, n = 0;
    char *body = (char *)malloc(cap);
    if (!body) return;
    n += (size_t)snprintf(body + n, cap - n,
                          "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                          "\"params\":{\"uri\":");
    char *eu = json_escape(uri);
    n += (size_t)snprintf(body + n, cap - n, "\"%s\",\"diagnostics\":[", eu ? eu : "");
    free(eu);

    int first = 1;
    for (int i = 0; i < Gsess.ndiag; i++) {
        LoamDiag *d = &Gsess.diags[i];
        if (!diag_for_file(d, path, opened)) continue;
        SourceLoc loc;
        loc.file = d->file;
        loc.line = d->line;
        loc.col = d->col;
        loc.end_line = d->end_line;
        loc.end_col = d->end_col;
        int sl, sc, el, ec;
        loc_lsp(loc, &sl, &sc, &el, &ec);
        char *em = json_escape(d->msg ? d->msg : "");
        char item[2048];
        int m = snprintf(item, sizeof item,
                         "%s{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                         "\"end\":{\"line\":%d,\"character\":%d}},"
                         "\"severity\":1,\"source\":\"loam\",\"message\":\"%s\"}",
                         first ? "" : ",", sl, sc, el, ec, em ? em : "");
        first = 0;
        free(em);
        if (n + (size_t)m + 8 >= cap) {
            cap = (n + (size_t)m + 8) * 2;
            body = (char *)realloc(body, cap);
            if (!body) return;
        }
        memcpy(body + n, item, (size_t)m);
        n += (size_t)m;
    }
    if (n + 8 >= cap) {
        cap += 16;
        body = (char *)realloc(body, cap);
        if (!body) return;
    }
    memcpy(body + n, "]}}", 4);
    n += 3;
    body[n] = '\0';
    send_raw(body);
    free(body);
}

static void publish_all(void) {
    if (Guri) send_diagnostics(Guri, Gpath, 1);
    for (int i = 0; i < Gsess.nmods; i++) {
        const char *p = Gsess.mods[i].path;
        if (!p) continue;
        if (Gpath && strcmp(p, Gpath) == 0) continue;
        char *u = path_to_uri(p);
        if (u) {
            send_diagnostics(u, p, 0);
            free(u);
        }
    }
}

static int pos_to_off(const char *src, int line0, int col0);

static void compile_buffer(const char *uri, const char *path, const char *src) {
    free(Guri);
    free(Gpath);
    Guri = loam_dup(uri);
    Gpath = loam_dup(path);
    free(Gtext);
    Gtext = loam_dup(src);
    loam_session_check(&Gsess, path, src);
    Ghave = 1;
    publish_all();
}

/** LSP range is 0-based; splice `ins` over [sl,sc)…[el,ec) in `src`. */
static char *replace_range(const char *src, int sl, int sc, int el, int ec, const char *ins) {
    if (!src) src = "";
    if (!ins) ins = "";
    int a = pos_to_off(src, sl, sc);
    int b = pos_to_off(src, el, ec);
    if (b < a) b = a;
    size_t slen = strlen(src);
    size_t ilen = strlen(ins);
    size_t n = slen - (size_t)(b - a) + ilen;
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, (size_t)a);
    memcpy(out + a, ins, ilen);
    memcpy(out + a + ilen, src + b, slen - (size_t)b);
    out[n] = '\0';
    return out;
}

static void send_null_result(const char *id) {
    char out[128];
    snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":null}",
             id ? id : "null");
    send_raw(out);
}

static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static int pos_to_off(const char *src, int line0, int col0) {
    if (!src) return 0;
    int line = 0, col = 0;
    const char *p = src;
    while (*p && line < line0) {
        if (*p == '\n') line++;
        p++;
    }
    while (*p && *p != '\n' && col < col0) {
        p++;
        col++;
    }
    return (int)(p - src);
}

static void off_to_pos(const char *src, int off, int *line, int *col) {
    int l = 0, c = 0;
    for (int i = 0; i < off && src && src[i]; i++) {
        if (src[i] == '\n') {
            l++;
            c = 0;
        } else {
            c++;
        }
    }
    *line = l;
    *col = c;
}

static Type *peel_type(Type *t) {
    while (t && (t->kind == TY_PTR || t->kind == TY_BOX)) t = t->elem;
    return t;
}

static int recv_ok(Type *recv, Type *param) {
    if (!recv || !param) return 0;
    if (type_eq(recv, param)) return 1;
    if (param->kind == TY_PTR && type_eq(recv, param->elem)) return 1;
    if (recv->kind == TY_PTR && type_eq(recv->elem, param)) return 1;
    Type *a = peel_type(recv), *b = peel_type(param);
    return a && b && type_eq(a, b);
}

typedef struct {
    char **labels;
    char **details;
    char **docs;
    int *kinds;
    int n, cap;
} Completions;

static void add_std_import_comps(Completions *c, const char *prefix);

static void completions_free(Completions *c) {
    for (int i = 0; i < c->n; i++) {
        free(c->labels[i]);
        free(c->details[i]);
        free(c->docs[i]);
    }
    free(c->labels);
    free(c->details);
    free(c->docs);
    free(c->kinds);
    memset(c, 0, sizeof(*c));
}

static void add_comp_d(Completions *c, const char *lab, const char *detail, int kind,
                      const char *prefix, const char *doc) {
    if (!lab || !lab[0]) return;
    if (c->n >= 512) return;
    if (prefix && prefix[0] && strncmp(lab, prefix, strlen(prefix)) != 0) return;
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->labels[i], lab) == 0) return;
    if (c->n >= c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->labels = realloc(c->labels, (size_t)c->cap * sizeof(char *));
        c->details = realloc(c->details, (size_t)c->cap * sizeof(char *));
        c->docs = realloc(c->docs, (size_t)c->cap * sizeof(char *));
        c->kinds = realloc(c->kinds, (size_t)c->cap * sizeof(int));
    }
    c->labels[c->n] = loam_dup(lab);
    c->details[c->n] = loam_dup(detail ? detail : "");
    c->docs[c->n] = (doc && doc[0]) ? loam_dup(doc) : NULL;
    c->kinds[c->n] = kind;
    c->n++;
}

static void add_comp(Completions *c, const char *lab, const char *detail, int kind,
                    const char *prefix) {
    add_comp_d(c, lab, detail, kind, prefix, NULL);
}

static void add_mod_members(Completions *c, LoamModule *m, const char *prefix, Type *recv) {
    if (!m || !m->ast) return;
    AstNode *p = m->ast;
    for (size_t i = 0; i < p->as.program.decl_count; i++) {
        AstNode *d = p->as.program.decls[i];
        if (d->kind == AST_FN_DECL && d->as.fn.name) {
            if (recv) {
                Type *ft = d->ty;
                if (!ft || ft->kind != TY_PROC || ft->param_count < 1) continue;
                if (!recv_ok(recv, ft->params[0])) continue;
                add_comp_d(c, d->as.fn.name, type_name(ft), 2, prefix, d->doc);
            } else {
                add_comp_d(c, d->as.fn.name, d->ty ? type_name(d->ty) : "fn", 3, prefix, d->doc);
            }
        } else if (!recv && d->kind == AST_VAR_DECL && d->as.var.name) {
            add_comp_d(c, d->as.var.name, d->ty ? type_name(d->ty) : "", 6, prefix, d->doc);
        } else if (!recv && d->kind == AST_STRUCT_DECL && d->as.strct.name) {
            add_comp_d(c, d->as.strct.name, "struct", 22, prefix, d->doc);
        } else if (!recv && d->kind == AST_ENUM_DECL && d->as.enm.name) {
            add_comp_d(c, d->as.enm.name, "enum", 13, prefix, d->doc);
        }
    }
}

static AstNode *find_enum_decl(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < Gsess.nmods; i++) {
        AstNode *p = Gsess.mods[i].ast;
        if (!p) continue;
        for (size_t d = 0; d < p->as.program.decl_count; d++) {
            AstNode *n = p->as.program.decls[d];
            if (n && n->kind == AST_ENUM_DECL && n->as.enm.name &&
                strcmp(n->as.enm.name, name) == 0)
                return n;
        }
    }
    return NULL;
}

static void add_enum_variants(Completions *c, AstNode *en, const char *prefix) {
    if (!en || en->kind != AST_ENUM_DECL) return;
    for (size_t i = 0; i < en->as.enm.count; i++)
        add_comp(c, en->as.enm.vnames[i], en->as.enm.name, 20, prefix);
}

static void add_struct_fields(Completions *c, Type *t, const char *prefix) {
    t = peel_type(t);
    if (!t || t->kind != TY_STRUCT) return;
    for (size_t i = 0; i < t->field_count; i++) {
        const char *nm = t->field_names[i];
        const char *ty = t->field_types[i] ? type_name(t->field_types[i]) : "";
        add_comp(c, nm, ty, 5, prefix);
    }
}

static void add_vars_in(AstNode *n, Completions *c, const char *prefix, int line, int col) {
    if (!n) return;
    if (n->kind == AST_VAR_DECL && n->as.var.name) {
        int sl, sc, el, ec;
        loc_lsp(n->loc, &sl, &sc, &el, &ec);
        if (cmp_pos(sl, sc, line, col) <= 0)
            add_comp(c, n->as.var.name, n->ty ? type_name(n->ty) : "", 6, prefix);
    }
    switch (n->kind) {
        case AST_FN_DECL:
        case AST_CLOSURE:
            add_vars_in(n->as.fn.body, c, prefix, line, col);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                add_vars_in(n->as.block.stmts[i], c, prefix, line, col);
            break;
        case AST_IF:
            add_vars_in(n->as.if_stmt.then_block, c, prefix, line, col);
            add_vars_in(n->as.if_stmt.else_block, c, prefix, line, col);
            break;
        case AST_FOR:
            add_vars_in(n->as.for_stmt.body, c, prefix, line, col);
            break;
        case AST_EXPR_STMT:
            add_vars_in(n->as.expr_stmt.expr, c, prefix, line, col);
            break;
        case AST_ASSIGN:
            add_vars_in(n->as.assign.right, c, prefix, line, col);
            break;
        case AST_CALL:
            for (size_t i = 0; i < n->as.call.arg_count; i++)
                add_vars_in(n->as.call.args[i], c, prefix, line, col);
            break;
        default:
            break;
    }
}

static const char *src_for_uri(const char *uri) {
    LoamModule *m = mod_for_uri(uri);
    return m && m->src ? m->src : NULL;
}

static int ident_is_module(AstNode *n, LoamModule *cur) {
    if (is_module_ident(n)) return 1;
    if (!n || n->kind != AST_IDENT || !n->as.ident.name) return 0;
    if (mod_by_alias(n->as.ident.name)) return 1;
    if (cur && cur->ast && n->as.ident.name) {
        AstNode *p = cur->ast;
        for (size_t i = 0; i < p->as.program.import_count; i++) {
            AstNode *im = p->as.program.imports[i];
            if (im && im->as.import.alias && strcmp(im->as.import.alias, n->as.ident.name) == 0)
                return 1;
        }
    }
    return 0;
}

static void send_completions(const char *id, Completions *c) {
    size_t cap = 1024, n = 0;
    char *body = (char *)malloc(cap);
    if (!body) {
        send_null_result(id);
        return;
    }
    n += (size_t)snprintf(body + n, cap - n,
                          "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"isIncomplete\":false,\"items\":[",
                          id ? id : "null");
    for (int i = 0; i < c->n; i++) {
        char *el = json_escape(c->labels[i]);
        char *ed = json_escape(c->details[i]);
        char *edoc = (c->docs && c->docs[i]) ? json_escape(c->docs[i]) : NULL;
        size_t need = 256 + (el ? strlen(el) : 0) + (ed ? strlen(ed) : 0) +
                      (edoc ? strlen(edoc) : 0);
        char *item = (char *)malloc(need);
        int m;
        if (!item) {
            free(el);
            free(ed);
            free(edoc);
            continue;
        }
        if (edoc)
            m = snprintf(item, need,
                         "%s{\"label\":\"%s\",\"kind\":%d,\"detail\":\"%s\","
                         "\"documentation\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
                         i ? "," : "", el ? el : "", c->kinds[i], ed ? ed : "", edoc);
        else
            m = snprintf(item, need,
                         "%s{\"label\":\"%s\",\"kind\":%d,\"detail\":\"%s\"}",
                         i ? "," : "", el ? el : "", c->kinds[i], ed ? ed : "");
        if (m < 0) {
            free(item);
            free(el);
            free(ed);
            free(edoc);
            continue;
        }
        if ((size_t)m + 1 > need) {
            need = (size_t)m + 1;
            item = (char *)realloc(item, need);
            if (!item) {
                free(el);
                free(ed);
                free(edoc);
                continue;
            }
            if (edoc)
                m = snprintf(item, need,
                             "%s{\"label\":\"%s\",\"kind\":%d,\"detail\":\"%s\","
                             "\"documentation\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
                             i ? "," : "", el ? el : "", c->kinds[i], ed ? ed : "", edoc);
            else
                m = snprintf(item, need,
                             "%s{\"label\":\"%s\",\"kind\":%d,\"detail\":\"%s\"}",
                             i ? "," : "", el ? el : "", c->kinds[i], ed ? ed : "");
        }
        free(el);
        free(ed);
        free(edoc);
        if (m < 0) {
            free(item);
            continue;
        }
        if (n + (size_t)m + 8 >= cap) {
            cap = (n + (size_t)m + 8) * 2;
            body = realloc(body, cap);
            if (!body) {
                free(item);
                return;
            }
        }
        memcpy(body + n, item, (size_t)m);
        n += (size_t)m;
        free(item);
    }
    if (n + 8 >= cap) {
        cap += 16;
        body = realloc(body, cap);
        if (!body) return;
    }
    memcpy(body + n, "]}}", 4);
    send_raw(body);
    free(body);
}

static void handle_completion(const char *msg, const char *id) {
    char *uri = extract_uri(msg);
    const char *pos = find_key(msg, "position");
    int line = parse_json_int(find_key(pos ? pos : msg, "line"));
    int col = parse_json_int(find_key(pos ? pos : msg, "character"));
    const char *src = src_for_uri(uri);
    Completions c;
    memset(&c, 0, sizeof c);
    if (!src) {
        free(uri);
        send_completions(id, &c);
        return;
    }
    int off = pos_to_off(src, line, col);
    int j = off;
    while (j > 0 && is_ident_char(src[j - 1])) j--;
    char prefix[128];
    size_t plen = (size_t)(off - j);
    if (plen >= sizeof prefix) plen = sizeof prefix - 1;
    memcpy(prefix, src + j, plen);
    prefix[plen] = '\0';
    int after_dot = (j > 0 && src[j - 1] == '.');
    LoamModule *cur = mod_for_uri(uri);
    if (in_import_str(src, off)) {
        char ipre[128];
        import_str_prefix(src, off, ipre, sizeof ipre);
        add_std_import_comps(&c, ipre);
        send_completions(id, &c);
        completions_free(&c);
        free(uri);
        return;
    }
    if (after_dot) {
        int recv_off = j - 1;
        while (recv_off > 0 && isspace((unsigned char)src[recv_off - 1])) recv_off--;
        int rl, rc;
        off_to_pos(src, recv_off > 0 ? recv_off - 1 : 0, &rl, &rc);
        Pick pk = pick_at(uri, rl, rc);
        AstNode *recv = pk.node;
        if (recv && recv->kind == AST_FIELD && recv->as.access.target &&
            loc_covers(recv->loc, line, col))
            recv = recv->as.access.target;
        if (ident_is_module(recv, cur)) {
            const char *alias = recv->as.ident.name;
            add_mod_members(&c, mod_by_alias(alias), prefix, NULL);
        } else {
            const char *en_name = NULL;
            if (recv && recv->kind == AST_IDENT) en_name = recv->as.ident.name;
            else if (recv && recv->kind == AST_FIELD) en_name = recv->as.access.field;
            add_enum_variants(&c, find_enum_decl(en_name), prefix);
            Type *ty = recv && recv->ty ? recv->ty : NULL;
            add_struct_fields(&c, ty, prefix);
            if (ty) {
                for (int i = 0; i < Gsess.nmods; i++)
                    add_mod_members(&c, &Gsess.mods[i], prefix, ty);
            }
        }
    } else {
        if (cur && cur->ast) {
            for (size_t i = 0; i < cur->ast->as.program.import_count; i++) {
                AstNode *im = cur->ast->as.program.imports[i];
                if (im && im->as.import.alias)
                    add_comp(&c, im->as.import.alias, "module", 9, prefix);
            }
            add_mod_members(&c, cur, prefix, NULL);
        }
        Pick pk = pick_at(uri, line, col > 0 ? col - 1 : 0);
        if (pk.fn) {
            for (size_t i = 0; i < pk.fn->as.fn.param_count; i++)
                add_comp(&c, pk.fn->as.fn.params[i].name,
                         pk.fn->ty && pk.fn->ty->kind == TY_PROC &&
                                 i < pk.fn->ty->param_count && pk.fn->ty->params[i]
                             ? type_name(pk.fn->ty->params[i])
                             : "",
                         6, prefix);
            add_vars_in(pk.fn->as.fn.body, &c, prefix, line, col);
        }
        add_comp(&c, "true", "bool", 21, prefix);
        add_comp(&c, "false", "bool", 21, prefix);
        add_comp(&c, "fn", "keyword", 14, prefix);
        add_comp(&c, "async", "keyword", 14, prefix);
        add_comp(&c, "await", "keyword", 14, prefix);
        add_comp(&c, "let", "keyword", 14, prefix);
        add_comp(&c, "const", "keyword", 14, prefix);
        add_comp(&c, "mut", "keyword", 14, prefix);
        add_comp(&c, "struct", "keyword", 14, prefix);
        add_comp(&c, "enum", "keyword", 14, prefix);
        add_comp(&c, "import", "keyword", 14, prefix);
        add_comp(&c, "if", "keyword", 14, prefix);
        add_comp(&c, "else", "keyword", 14, prefix);
        add_comp(&c, "for", "keyword", 14, prefix);
        add_comp(&c, "while", "keyword", 14, prefix);
        add_comp(&c, "in", "keyword", 14, prefix);
        add_comp(&c, "return", "keyword", 14, prefix);
        add_comp(&c, "break", "keyword", 14, prefix);
        add_comp(&c, "continue", "keyword", 14, prefix);
        add_comp(&c, "match", "keyword", 14, prefix);
        add_comp(&c, "as", "keyword", 14, prefix);
        add_comp(&c, "int", "type", 25, prefix);
        add_comp(&c, "float", "type", 25, prefix);
        add_comp(&c, "i8", "type", 25, prefix);
        add_comp(&c, "i16", "type", 25, prefix);
        add_comp(&c, "i32", "type", 25, prefix);
        add_comp(&c, "i64", "type", 25, prefix);
        add_comp(&c, "u8", "type", 25, prefix);
        add_comp(&c, "u16", "type", 25, prefix);
        add_comp(&c, "u32", "type", 25, prefix);
        add_comp(&c, "u64", "type", 25, prefix);
        add_comp(&c, "f32", "type", 25, prefix);
        add_comp(&c, "f64", "type", 25, prefix);
        add_comp(&c, "bool", "type", 25, prefix);
        add_comp(&c, "string", "type", 25, prefix);
    }
    send_completions(id, &c);
    completions_free(&c);
    free(uri);
}

static void handle_hover(const char *msg, const char *id) {
    char *uri = extract_uri(msg);
    const char *pos = find_key(msg, "position");
    int line = parse_json_int(find_key(pos ? pos : msg, "line"));
    int col = parse_json_int(find_key(pos ? pos : msg, "character"));
    Pick pk = pick_at(uri, line, col);
    char *text = hover_alloc(&pk);
    free(uri);
    if (!text || !text[0]) {
        free(text);
        send_null_result(id);
        return;
    }
    char *esc = json_escape(text);
    size_t cap = (esc ? strlen(esc) : 0) + 160;
    char *out = (char *)malloc(cap);
    if (!out) {
        free(esc);
        free(text);
        send_null_result(id);
        return;
    }
    snprintf(out, cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"contents\":{\"kind\":\"markdown\","
             "\"value\":\"%s\"}}}",
             id ? id : "null", esc ? esc : "");
    send_raw(out);
    free(out);
    free(esc);
    free(text);
}

static void handle_definition(const char *msg, const char *id) {
    char *uri = extract_uri(msg);
    const char *pos = find_key(msg, "position");
    int line = parse_json_int(find_key(pos ? pos : msg, "line"));
    int col = parse_json_int(find_key(pos ? pos : msg, "character"));
    Pick pk = pick_at(uri, line, col);
    free(uri);
    SourceLoc dloc;
    if (!def_loc_of(&pk, &dloc) || dloc.line <= 0) {
        send_null_result(id);
        return;
    }
    int sl, sc, el, ec;
    loc_lsp(dloc, &sl, &sc, &el, &ec);
    const char *file = dloc.file;
    char *duri = NULL;
    int own = 0;
    if (Gpath && file && strcmp(Gpath, file) == 0 && Guri) {
        duri = Guri;
    } else {
        duri = path_to_uri(file ? file : Gpath);
        own = 1;
    }
    char *eu = json_escape(duri ? duri : "");
    char out[1024];
    snprintf(out, sizeof out,
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"uri\":\"%s\","
             "\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
             "\"end\":{\"line\":%d,\"character\":%d}}}}",
             id ? id : "null", eu ? eu : "", sl, sc, el, ec);
    free(eu);
    if (own) free(duri);
    send_raw(out);
}

/* --- Open documents ------------------------------------------------------
   loam-lsp compiles one document at a time (the session is single-module),
   but clients keep many buffers open and can re-open one already tracked
   (a "redundant" didOpen after a client or server restart). Every open
   buffer is stored here; the doc a request names is compiled lazily, and a
   repeat didOpen of the current buffer is a no-op when the text is
   unchanged. */
typedef struct DocBuf {
    char *uri;
    char *path;
    char *text;
    struct DocBuf *next;
} DocBuf;

static DocBuf *Gdocs;

static DocBuf *doc_find(const char *uri) {
    DocBuf *d;
    for (d = Gdocs; d; d = d->next)
        if (strcmp(d->uri, uri) == 0) return d;
    return NULL;
}

static void doc_put(const char *uri, const char *path, const char *text) {
    DocBuf *d = doc_find(uri);
    if (!d) {
        d = (DocBuf *)calloc(1, sizeof *d);
        d->uri = loam_dup(uri);
        d->next = Gdocs;
        Gdocs = d;
    }
    free(d->path);
    d->path = loam_dup(path ? path : "");
    free(d->text);
    d->text = loam_dup(text ? text : "");
}

static void doc_drop(const char *uri) {
    DocBuf **pp = &Gdocs;
    while (*pp) {
        DocBuf *d = *pp;
        if (strcmp(d->uri, uri) == 0) {
            *pp = d->next;
            free(d->uri);
            free(d->path);
            free(d->text);
            free(d);
            return;
        }
        pp = &d->next;
    }
}

static void doc_free_all(void) {
    while (Gdocs) doc_drop(Gdocs->uri);
}

/* Compile the stored buffer for `uri` when it is not already the compiled
   one — requests (hover, definition, completion, semantic tokens) name a
   uri, and the session must be showing that doc's program. */
static void doc_activate(const char *uri) {
    DocBuf *d;
    if (!uri || !uri[0]) return;
    if (Ghave && Guri && strcmp(Guri, uri) == 0) return;
    d = doc_find(uri);
    if (d) compile_buffer(d->uri, d->path, d->text);
}

static void handle_doc(const char *msg, const char *method) {
    const char *td = find_key(msg, "textDocument");
    char *uri = NULL;
    char *text = NULL;
    if (td) uri = parse_json_string(find_key(td, "uri"));
    if (!uri) uri = parse_json_string(find_key(msg, "uri"));
    if (strcmp(method, "textDocument/didChange") == 0) {
        const char *ch = strstr(msg, "\"contentChanges\"");
        if (!ch) ch = strstr(msg, "contentChanges");
        if (ch) {
            const char *range = find_key(ch, "range");
            const char *textp = find_key(ch, "text");
            text = parse_json_string(textp);
            /* A `range` key before `text` is a real incremental edit. One that
               appears after is almost certainly the word "range" inside source.
               Apply it to this document's buffer: the compiled one when this
               doc is active, otherwise its stored open-buffer text. */
            const char *base = NULL;
            if (range && textp && range < textp) {
                if (Guri && strcmp(Guri, uri) == 0) base = Gtext;
                if (!base) {
                    DocBuf *k = doc_find(uri);
                    if (k) base = k->text;
                }
            }
            if (range && textp && range < textp && base) {
                const char *start = find_key(range, "start");
                const char *end = find_key(range, "end");
                int sl = parse_json_int(find_key(start ? start : range, "line"));
                int sc = parse_json_int(find_key(start ? start : range, "character"));
                int el = parse_json_int(find_key(end ? end : range, "line"));
                int ec = parse_json_int(find_key(end ? end : range, "character"));
                char *next = replace_range(base, sl, sc, el, ec, text ? text : "");
                free(text);
                text = next;
            }
        }
    } else if (strcmp(method, "textDocument/didClose") != 0) {
        if (td) text = parse_json_string(find_key(td, "text"));
        if (!text) text = parse_json_string(find_key(msg, "text"));
    }
    if (!uri) {
        free(text);
        return;
    }
    char *path = uri_to_path(uri);
    if (strcmp(method, "textDocument/didClose") == 0) {
        doc_drop(uri);
        if (path) {
            /* clear squiggles for this buffer */
            char *saved_uri = Guri, *saved_path = Gpath;
            int saved_n = Gsess.ndiag;
            LoamDiag *saved_d = Gsess.diags;
            Guri = uri;
            Gpath = path;
            Gsess.ndiag = 0;
            Gsess.diags = NULL;
            send_diagnostics(uri, path, 1);
            Guri = saved_uri;
            Gpath = saved_path;
            Gsess.ndiag = saved_n;
            Gsess.diags = saved_d;
        }
        if (Guri && strcmp(Guri, uri) == 0) {
            loam_session_free(&Gsess);
            loam_session_init(&Gsess);
            Ghave = 0;
            free(Guri);
            free(Gpath);
            free(Gtext);
            Guri = NULL;
            Gpath = NULL;
            Gtext = NULL;
        }
        free(path);
        free(uri);
        free(text);
        return;
    }
    /* didChange must use the editor buffer, not a stale file on disk. */
    if (!text && strcmp(method, "textDocument/didChange") == 0 && Gtext &&
        Guri && strcmp(Guri, uri) == 0)
        text = loam_dup(Gtext);
    if (!text && path) {
        DocBuf *known = doc_find(uri);
        if (known && known->text && known->text[0]) {
            /* editing a stored buffer with no payload: keep editor content */
            text = loam_dup(known->text);
        } else {
            FILE *f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                rewind(f);
                text = (char *)malloc((size_t)sz + 1);
                size_t nr = fread(text, 1, (size_t)sz, f);
                text[nr] = '\0';
                fclose(f);
            }
        }
    }
    if (path && text) {
        doc_put(uri, path, text);
        /* The compiled doc is the one the client is looking at. A didOpen
           for another buffer just stores it; the request that names it
           activates it. A repeat didOpen of the current buffer with the
           same text (redundant open) does not recompile or re-publish. */
        if (!Guri || strcmp(Guri, uri) == 0) {
            int same = Ghave && Gtext && strcmp(Gtext, text) == 0;
            if (!same) compile_buffer(uri, path, text);
        }
    }
    free(path);
    free(uri);
    free(text);
}

/* Semantic token types advertised in initialize. Indices must match. */
enum {
    ST_NAMESPACE = 0,
    ST_TYPE,
    ST_ENUM,
    ST_STRUCT,
    ST_PARAMETER,
    ST_VARIABLE,
    ST_PROPERTY,
    ST_ENUM_MEMBER,
    ST_FUNCTION,
    ST_KEYWORD,
    ST_COMMENT,
    ST_STRING,
    ST_NUMBER,
    ST_OPERATOR,
    ST_MODIFIER,
    ST_PUNCTUATION
};

typedef struct {
    int line, col, len, type;
} SemTok;

typedef struct {
    SemTok *v;
    int n, cap;
} SemToks;

static void st_push(SemToks *s, int line, int col, int len, int type) {
    if (len <= 0) return;
    if (s->n >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->v = (SemTok *)realloc(s->v, (size_t)s->cap * sizeof(SemTok));
    }
    s->v[s->n].line = line;
    s->v[s->n].col = col;
    s->v[s->n].len = len;
    s->v[s->n].type = type;
    s->n++;
}

static void st_bytes(SemToks *s, const char *src, const char *start, int len, int type) {
    if (!src || !start || len <= 0) return;
    int line = 0, col = 0;
    const unsigned char *q = (const unsigned char *)src;
    const unsigned char *end0 = (const unsigned char *)start;
    while (q < end0) {
        if (*q == '\n') {
            line++;
            col = 0;
            q++;
            continue;
        }
        if (*q < 0x80) {
            q++;
            col++;
        } else if ((*q & 0xE0) == 0xC0 && q + 1 < end0) {
            q += 2;
            col++;
        } else if ((*q & 0xF0) == 0xE0 && q + 2 < end0) {
            q += 3;
            col++;
        } else if ((*q & 0xF8) == 0xF0 && q + 3 < end0) {
            q += 4;
            col += 2;
        } else {
            q++;
            col++;
        }
    }
    int run = 0, rl = line, rc = col, u16 = 0;
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)start[i];
        if (start[i] == '\n') {
            if (run > 0) st_push(s, rl, rc, u16, type);
            line++;
            col = 0;
            run = 0;
            u16 = 0;
            rl = line;
            rc = 0;
            continue;
        }
        if (run == 0) {
            rl = line;
            rc = col;
            u16 = 0;
        }
        run++;
        if (ch < 0x80) {
            col++;
            u16++;
        } else if ((ch & 0xC0) != 0x80) {
            /* UTF-8 lead: BMP is 1 UTF-16 unit, astral is 2. */
            if ((ch & 0xF8) == 0xF0) {
                col += 2;
                u16 += 2;
            } else {
                col++;
                u16++;
            }
        }
    }
    if (run > 0) st_push(s, rl, rc, u16, type);
}

static void st_scan_gap(SemToks *s, const char *src, const char *from, const char *to) {
    if (!from || !to || from >= to) return;
    const char *p = from;
    while (p < to) {
        if (*p == '/' && p + 1 < to && p[1] == '/') {
            const char *start = p;
            while (p < to && *p != '\n') p++;
            st_bytes(s, src, start, (int)(p - start), ST_COMMENT);
            continue;
        }
        if (*p == '/' && p + 1 < to && p[1] == '*') {
            const char *start = p;
            p += 2;
            while (p + 1 < to && !(*p == '*' && p[1] == '/')) p++;
            if (p + 1 < to) p += 2;
            else p = to;
            st_bytes(s, src, start, (int)(p - start), ST_COMMENT);
            continue;
        }
        p++;
    }
}

static int st_is_type_name(const char *s, int n) {
    static const char *names[] = {"int", "float", "i8", "i16", "i32", "i64",
                                  "u8", "u16", "u32", "u64", "f32", "f64",
                                  "bool", "string", "Box", NULL};
    for (int i = 0; names[i]; i++) {
        int m = (int)strlen(names[i]);
        if (n == m && strncmp(s, names[i], (size_t)n) == 0) return 1;
    }
    return 0;
}

static int st_is_punct(TokenKind k) {
    switch (k) {
        case TOK_LPAREN:
        case TOK_RPAREN:
        case TOK_LBRACE:
        case TOK_RBRACE:
        case TOK_LBRACKET:
        case TOK_RBRACKET:
        case TOK_COMMA:
        case TOK_DOT:
        case TOK_COLON:
        case TOK_SEMICOLON:
        case TOK_COLON_COLON:
            return 1;
        default:
            return 0;
    }
}

static int st_is_keyword_tok(TokenKind k) {
    switch (k) {
        case TOK_FN:
        case TOK_LET:
        case TOK_CONST:
        case TOK_STRUCT:
        case TOK_ENUM:
        case TOK_IMPORT:
        case TOK_IF:
        case TOK_ELSE:
        case TOK_FOR:
        case TOK_WHILE:
        case TOK_IN:
        case TOK_RETURN:
        case TOK_BREAK:
        case TOK_CONTINUE:
        case TOK_MATCH:
        case TOK_AS:
            return 1;
        default:
            return 0;
    }
}

static int st_pascal(Token t) {
    return t.kind == TOK_IDENT && t.len > 0 && t.start[0] >= 'A' && t.start[0] <= 'Z';
}

static TokenKind st_at(Token *ts, int n, int i) {
    if (i < 0 || i >= n) return TOK_EOF;
    return ts[i].kind;
}

#define ST_MAX_MOD 48
static char st_mods[ST_MAX_MOD][64];
static int st_nmods;

#ifndef LOAM_STD_DIR
#define LOAM_STD_DIR "packages/loam/std"
#endif

static char std_names[32][32];
static int nstd_names;

static void add_std_name(const char *name, size_t n) {
    if (n == 0 || n >= 31 || nstd_names >= 32) return;
    for (int i = 0; i < nstd_names; i++)
        if (strlen(std_names[i]) == n && strncmp(std_names[i], name, n) == 0) return;
    memcpy(std_names[nstd_names], name, n);
    std_names[nstd_names][n] = 0;
    nstd_names++;
}

static void load_std_names(void) {
    const char *dirs[LOAM_MAX_STD_DIRS];
    DIR *d;
    struct dirent *ent;
    int nd;
    if (nstd_names) return;
    nd = loam_std_dirs(dirs, LOAM_MAX_STD_DIRS);
    for (int i = 0; i < nd && nstd_names < 32; i++) {
        d = opendir(dirs[i]);
        if (!d) continue;
        while ((ent = readdir(d))) {
            size_t n = strlen(ent->d_name);
            size_t e = loam_ext_at(ent->d_name, n);
            if (!e) continue;
            add_std_name(ent->d_name, n - e);
        }
        closedir(d);
    }
    if (!nstd_names) {
        const char *fb[] = {"fmt", "async", "zeus", "http", "sys", "net",
                            "maya", "kv", NULL};
        for (int i = 0; fb[i] && nstd_names < 32; i++)
            add_std_name(fb[i], strlen(fb[i]));
    }
}

static int in_import_str(const char *src, int off) {
    int i = off;
    if (!src || off < 0) return 0;
    while (i > 0 && src[i - 1] != '\n') i--;
    while (src[i] == ' ' || src[i] == '\t') i++;
    return strncmp(src + i, "import", 6) == 0;
}

static void import_str_prefix(const char *src, int off, char *pre, size_t cap) {
    int q = off;
    if (!pre || cap == 0) return;
    pre[0] = 0;
    if (!src) return;
    while (q > 0 && src[q - 1] != '"' && src[q - 1] != '\n') q--;
    if ((size_t)(off - q) >= cap) {
        memcpy(pre, src + q, cap - 1);
        pre[cap - 1] = 0;
    } else {
        memcpy(pre, src + q, (size_t)(off - q));
        pre[off - q] = 0;
    }
}

/** Basename of the root owning a `loam_std_dirs` entry: `<root>/std` -> `<root>`'s
 *  last segment, which is the name the `package:module` form matches on. Returns
 *  0 for an entry that is not `<something>/std`, so the bare language std dir is
 *  skipped unless a LOAM_PATH root happens to sit beside it. */
static int pkg_of_std_dir(const char *dir, char *out, size_t cap) {
    size_t n = dir ? strlen(dir) : 0;
    if (n < 5 || strcmp(dir + n - 4, "/std") != 0) return 0;
    n -= 4;
    size_t b = n;
    while (b > 0 && dir[b - 1] != '/') b--;
    size_t len = n - b;
    if (len == 0 || len >= cap) return 0;
    memcpy(out, dir + b, len);
    out[len] = 0;
    return 1;
}

/** Import completion: `std:name` for every std-dir module, plus `pkg:name` for
 *  each LOAM_PATH root that actually resolves that module. Packages below `std`
 *  now name each other (`zeus:zeusbase`, `zeus-components:atoms`), so completing
 *  only the `std:` spelling would hide the form those packages use. */
static void add_std_import_comps(Completions *c, const char *prefix) {
    char lab[96], pkg[64], hit[1024];
    const char *dirs[LOAM_MAX_STD_DIRS];
    load_std_names();
    for (int i = 0; i < nstd_names; i++) {
        snprintf(lab, sizeof lab, "std:%s", std_names[i]);
        add_comp(c, lab, "module", 9, prefix);
    }
    int nd = loam_std_dirs(dirs, LOAM_MAX_STD_DIRS);
    for (int d = 0; d < nd; d++) {
        if (!pkg_of_std_dir(dirs[d], pkg, sizeof pkg)) continue;
        for (int i = 0; i < nstd_names; i++) {
            if (!loam_package_module(pkg, std_names[i], hit, sizeof hit)) continue;
            snprintf(lab, sizeof lab, "%s:%s", pkg, std_names[i]);
            add_comp(c, lab, "module", 9, prefix);
        }
    }
}

static void st_add_mod(const char *s, int n) {
    if (st_nmods >= ST_MAX_MOD || n <= 0 || n >= 63) return;
    for (int i = 0; i < st_nmods; i++) {
        if ((int)strlen(st_mods[i]) == n && strncmp(st_mods[i], s, (size_t)n) == 0)
            return;
    }
    memcpy(st_mods[st_nmods], s, (size_t)n);
    st_mods[st_nmods][n] = 0;
    st_nmods++;
}

static int st_is_mod(Token t) {
    for (int i = 0; i < st_nmods; i++) {
        int m = (int)strlen(st_mods[i]);
        if (t.len == m && strncmp(t.start, st_mods[i], (size_t)m) == 0) return 1;
    }
    return 0;
}

static void st_mod_from_import(Token str) {
    if (str.kind != TOK_STRING || str.len < 3) return;
    const char *s = str.start + 1;
    int n = str.len - 2;
    const char *stem = s;
    for (int i = 0; i < n; i++) {
        if (s[i] == '/' || s[i] == ':') stem = s + i + 1;
    }
    int len = (int)((s + n) - stem);
    len -= (int)loam_ext_at(stem, (size_t)len);
    st_add_mod(stem, len);
}

static int st_tok_is(Token t, const char *s) {
    size_t n = strlen(s);
    return t.kind == TOK_IDENT && (size_t)t.len == n && strncmp(t.start, s, n) == 0;
}

static int st_prev_ends_value(TokenKind k) {
    switch (k) {
        case TOK_IDENT:
        case TOK_NUMBER:
        case TOK_FLOAT:
        case TOK_STRING:
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_RPAREN:
        case TOK_RBRACKET:
        case TOK_DOT:
        case TOK_COLON_COLON:
            return 1;
        default:
            return 0;
    }
}

static int st_ident_kind(Token *ts, int n, int i, int paren) {
    Token t = ts[i];
    TokenKind prev = st_at(ts, n, i - 1);
    TokenKind next = st_at(ts, n, i + 1);
    int after_dot = (prev == TOK_DOT || prev == TOK_COLON_COLON);

    /* Contextual keywords: `async fn` (decl position) and expression-leading
       `await`. After a value/dot they are ordinary identifiers. */
    if (st_tok_is(t, "async") && next == TOK_FN) return ST_KEYWORD;
    if (st_tok_is(t, "await") && !st_prev_ends_value(prev)) return ST_KEYWORD;

    /* `#[test]`, `#[proto]` — the identifier between `[` and `]` after `#`. */
    if (prev == TOK_LBRACKET && next == TOK_RBRACKET && st_at(ts, n, i - 2) == TOK_HASH)
        return ST_KEYWORD;

    if (prev == TOK_FN) return ST_FUNCTION;
    if (prev == TOK_ENUM) return ST_ENUM;
    if (prev == TOK_STRUCT) return ST_STRUCT;
    if (prev == TOK_LET || prev == TOK_CONST || prev == TOK_MUT) return ST_VARIABLE;
    if (prev == TOK_COLON || prev == TOK_ARROW) return ST_TYPE;
    if (st_is_type_name(t.start, t.len)) return ST_TYPE;

    if (after_dot) {
        if (next == TOK_LPAREN) return ST_FUNCTION;
        if (next == TOK_DOT) return ST_ENUM;
        if (st_pascal(t)) return ST_ENUM_MEMBER;
        return ST_PROPERTY;
    }

    if (next == TOK_COLON) return paren > 0 ? ST_PARAMETER : ST_PROPERTY;
    if (next == TOK_EQ && prev != TOK_LET && prev != TOK_CONST && prev != TOK_MUT) {
        if (paren > 0) return ST_PROPERTY;
    }
    if (next == TOK_LT || next == TOK_DOT) {
        if (st_is_mod(t)) return ST_NAMESPACE;
        if (st_pascal(t)) return next == TOK_LT ? ST_STRUCT : ST_ENUM;
    }
    if (st_is_mod(t) && next == TOK_DOT) return ST_NAMESPACE;
    if (next == TOK_LPAREN) return ST_FUNCTION;
    if (st_pascal(t)) return ST_FUNCTION;
    return ST_VARIABLE;
}

static int st_tok_kind(Token *ts, int n, int i, int paren) {
    Token t = ts[i];
    if (st_is_keyword_tok(t.kind)) return ST_KEYWORD;
    if (t.kind == TOK_MUT) return ST_MODIFIER;
    if (t.kind == TOK_TRUE || t.kind == TOK_FALSE) return ST_ENUM_MEMBER;
    if (t.kind == TOK_NUMBER || t.kind == TOK_FLOAT) return ST_NUMBER;
    if (t.kind == TOK_STRING) return ST_STRING;
    if (t.kind == TOK_DOC || t.kind == TOK_MOD_DOC) return ST_COMMENT;
    if (t.kind == TOK_IDENT) return st_ident_kind(ts, n, i, paren);
    return ST_OPERATOR;
}

static int st_cmp(const void *a, const void *b) {
    const SemTok *x = (const SemTok *)a, *y = (const SemTok *)b;
    if (x->line != y->line) return x->line - y->line;
    return x->col - y->col;
}

static void handle_semantic_tokens(const char *msg, const char *id) {
    char *uri = extract_uri(msg);
    const char *src = src_for_uri(uri);
    free(uri);
    SemToks toks;
    memset(&toks, 0, sizeof toks);
    st_nmods = 0;
    load_std_names();
    for (int i = 0; i < nstd_names; i++)
        st_add_mod(std_names[i], (int)strlen(std_names[i]));
    if (src && src[0]) {
        Token *ts = NULL;
        int ntok = 0, tcap = 0;
        Lexer lx;
        lexer_init(&lx, src, "");
        const char *prev_end = src;
        TokenKind prevk = TOK_EOF;
        for (;;) {
            Token t = lexer_next(&lx);
            if (t.kind == TOK_EOF) {
                st_scan_gap(&toks, src, prev_end, src + strlen(src));
                break;
            }
            st_scan_gap(&toks, src, prev_end, t.start);
            if (prevk == TOK_IMPORT && t.kind == TOK_STRING) st_mod_from_import(t);
            if (ntok >= tcap) {
                tcap = tcap ? tcap * 2 : 256;
                ts = (Token *)realloc(ts, (size_t)tcap * sizeof(Token));
            }
            ts[ntok++] = t;
            prevk = t.kind;
            prev_end = t.start + t.len;
        }
        int paren = 0;
        for (int i = 0; i < ntok; i++) {
            TokenKind k = ts[i].kind;
            if (k == TOK_LPAREN) paren++;
            if (k == TOK_RPAREN && paren > 0) paren--;
            if (k == TOK_UNKNOWN) continue;
            int ty;
            if (st_is_punct(k)) ty = ST_PUNCTUATION;
            else ty = st_tok_kind(ts, ntok, i, paren);
            st_bytes(&toks, src, ts[i].start, ts[i].len, ty);
        }
        free(ts);
        if (toks.n > 1)
            qsort(toks.v, (size_t)toks.n, sizeof(SemTok), st_cmp);
    }
    size_t cap = 64 + (size_t)toks.n * 24;
    char *body = (char *)malloc(cap);
    if (!body) {
        send_null_result(id);
        free(toks.v);
        return;
    }
    size_t n = (size_t)snprintf(body, cap,
                                "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"data\":[",
                                id ? id : "null");
    int pl = 0, pc = 0;
    for (int i = 0; i < toks.n; i++) {
        int dl = toks.v[i].line - pl;
        int dc = dl == 0 ? toks.v[i].col - pc : toks.v[i].col;
        char item[80];
        int m = snprintf(item, sizeof item, "%s%d,%d,%d,%d,0", i ? "," : "", dl, dc,
                         toks.v[i].len, toks.v[i].type);
        if (n + (size_t)m + 8 >= cap) {
            cap = cap * 2 + (size_t)m + 32;
            char *nb = (char *)realloc(body, cap);
            if (!nb) break;
            body = nb;
        }
        memcpy(body + n, item, (size_t)m);
        n += (size_t)m;
        pl = toks.v[i].line;
        pc = toks.v[i].col;
    }
    memcpy(body + n, "]}}", 4);
    send_raw(body);
    free(body);
    free(toks.v);
}

static void handle(const char *msg) {
    char *method = extract_method(msg);
    char *id = extract_id(msg);
    if (!method) {
        free(id);
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        char out[1600];
        snprintf(out, sizeof out,
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                 "\"capabilities\":{\"textDocumentSync\":1,"
                 "\"hoverProvider\":true,\"definitionProvider\":true,"
                 "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
                 "\"semanticTokensProvider\":{\"legend\":{"
                 "\"tokenTypes\":[\"namespace\",\"type\",\"enum\",\"struct\","
                 "\"parameter\",\"variable\",\"property\",\"enumMember\","
                 "\"function\",\"keyword\",\"comment\",\"string\",\"number\","
                 "\"operator\",\"modifier\",\"punctuation\"],"
                 "\"tokenModifiers\":[\"declaration\"]},"
                 "\"full\":true,\"range\":false}},"
                 "\"serverInfo\":{\"name\":\"loam-lsp\",\"version\":\"0.4\"}}}",
                 id ? id : "null");
        send_raw(out);
    } else if (strcmp(method, "initialized") == 0) {
        /* ack not required */
    } else if (strcmp(method, "shutdown") == 0) {
        char out[128];
        snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":null}",
                 id ? id : "null");
        send_raw(out);
    } else if (strcmp(method, "exit") == 0) {
        free(method);
        free(id);
        doc_free_all();
        loam_session_free(&Gsess);
        free(Guri);
        free(Gpath);
        free(Gtext);
        exit(0);
    } else if (strcmp(method, "textDocument/didOpen") == 0 ||
               strcmp(method, "textDocument/didChange") == 0 ||
               strcmp(method, "textDocument/didSave") == 0 ||
               strcmp(method, "textDocument/didClose") == 0) {
        handle_doc(msg, method);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        char *u = extract_uri(msg);
        doc_activate(u);
        free(u);
        handle_hover(msg, id);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        char *u = extract_uri(msg);
        doc_activate(u);
        free(u);
        handle_definition(msg, id);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        char *u = extract_uri(msg);
        doc_activate(u);
        free(u);
        handle_completion(msg, id);
    } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
        char *u = extract_uri(msg);
        doc_activate(u);
        free(u);
        handle_semantic_tokens(msg, id);
    } else if (id && method[0] != '$') {
        char out[256];
        snprintf(out, sizeof out,
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,"
                 "\"message\":\"Method not found\"}}",
                 id);
        send_raw(out);
    }

    free(method);
    free(id);
}

int main(void) {
    loam_session_init(&Gsess);
    /* Unbuffered: a 64KiB stdin buffer can deadlock JSON-RPC over a pipe. */
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (;;) {
        char *msg = read_message();
        if (!msg) break;
        handle(msg);
        free(msg);
    }
    doc_free_all();
    loam_session_free(&Gsess);
    free(Guri);
    free(Gpath);
    free(Gtext);
    return 0;
}
