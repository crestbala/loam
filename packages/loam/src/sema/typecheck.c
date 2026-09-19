/**
 * typecheck.c — types, names, auto-borrow, generics, closure capture.
 *
 * Nested scopes. Generic fns are monomorphized per call (record_mono).
 * Empty bodies in fmt/zeus/maya/platform/sys/net are the link boundary
 * (`loam_<mod>_<fn>`). wrapping_* / string_from_bytes are language builtins.
 * Capturing closures copy Copy locals into a heap env and may escape.
 */
#include "typecheck.h"
#include "../diagnostics.h"
#include "../ast.h"
#include "type.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static LoamModule *Gmods;
static int Gn;
static int Gcur;
static int Gerr;

typedef struct Scope {
    const char **names;
    Type **types;
    int *muts;
    int *capfns;
    SourceLoc *locs;
    AstNode **nodes;
    int n, cap;
    struct Scope *parent;
} Scope;

static Scope *scope;

static void err(SourceLoc loc, const char *fmt, ...) {
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    loam_error(loc, "%s", buf);
    Gerr = 1;
}

static void scope_push(void) {
    Scope *s = (Scope *)calloc(1, sizeof(Scope));
    s->parent = scope;
    scope = s;
}

static void scope_pop(void) {
    Scope *s = scope;
    if (!s) return;
    scope = s->parent;
    free(s->names);
    free(s->types);
    free(s->muts);
    free(s->capfns);
    free(s->locs);
    free(s->nodes);
    free(s);
}

static void err(SourceLoc loc, const char *fmt, ...);

static void scope_add(const char *name, Type *ty, int is_mut, int capfn, SourceLoc loc,
                      AstNode *node) {
    if (!scope) return;
    /* A name defined twice in one scope is an error at the second definition,
       with the first site named. Shadowing in a nested scope is unchanged. */
    if (name && strcmp(name, "_") != 0) {
        for (int i = 0; i < scope->n; i++) {
            if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                err(loc, "'%s' is already defined in this scope (first at line %d)", name,
                    scope->locs[i].line);
                break;
            }
        }
    }
    if (scope->n >= scope->cap) {
        scope->cap = scope->cap ? scope->cap * 2 : 16;
        scope->names = realloc(scope->names, (size_t)scope->cap * sizeof(char *));
        scope->types = realloc(scope->types, (size_t)scope->cap * sizeof(Type *));
        scope->muts = realloc(scope->muts, (size_t)scope->cap * sizeof(int));
        scope->capfns = realloc(scope->capfns, (size_t)scope->cap * sizeof(int));
        scope->locs = realloc(scope->locs, (size_t)scope->cap * sizeof(SourceLoc));
        scope->nodes = realloc(scope->nodes, (size_t)scope->cap * sizeof(AstNode *));
    }
    scope->names[scope->n] = name;
    scope->types[scope->n] = ty;
    scope->muts[scope->n] = is_mut;
    scope->capfns[scope->n] = capfn;
    scope->locs[scope->n] = loc;
    scope->nodes[scope->n] = node;
    scope->n++;
}

static Scope *scope_find_s(const char *name, Type **ty, int *is_mut, int *capfn,
                           SourceLoc *loc, AstNode **node) {
    for (Scope *s = scope; s; s = s->parent) {
        for (int i = s->n - 1; i >= 0; i--) {
            if (strcmp(s->names[i], name) == 0) {
                if (ty) *ty = s->types[i];
                if (is_mut) *is_mut = s->muts[i];
                if (capfn) *capfn = s->capfns[i];
                if (loc) *loc = s->locs[i];
                if (node) *node = s->nodes[i];
                return s;
            }
        }
    }
    return NULL;
}

static int scope_find(const char *name, Type **ty, int *is_mut) {
    return scope_find_s(name, ty, is_mut, NULL, NULL, NULL) != NULL;
}

static Type *cur_ret;
static int loop_depth;
static int cur_async; /* 1 inside an `async fn` body: `await` is legal */
static const char *cur_mod_name;
static const char **cur_tparams;
static size_t cur_ntparams;
static AstNode *cur_clos;
static Scope *clos_scope;
/* Enclosing closures, innermost last. A name used in a nested closure is
   captured by every closure between it and its definition. */
static AstNode *clos_stack[64];
static Scope *clos_scope_stack[64];
static int clos_depth;
static int next_clos_id = 1;
static int clos_infer;
static Type *clos_infer_ty;

typedef struct {
    AstNode *fn;
    Type **args;
    size_t n;
    char *cname;
} Mono;

/* Caps on the session-wide symbol tables. These were 256 and, worse, dropped
   entries SILENTLY once full (`gvars`) or without a diagnostic (`monos`), so a
   large program failed later with a bogus "unknown identifier" for a global
   that was merely the one-thousandth. Sized for the whole std tree plus app
   modules, and every overflow is now a compile error, not a drop. */
#define LOAM_MAX_MONOS 2048
#define LOAM_MAX_STRUCT_INSTS 2048

static Mono monos[LOAM_MAX_MONOS];
static int nmono;

static Type *struct_insts[LOAM_MAX_STRUCT_INSTS];
static int nstruct_insts;

typedef struct {
    AstNode *var;
    char *cname;
    int mod;
} GVar;
/* Session-wide module globals. 4096 leaves ample room for the std tree (Zeus
   alone declares hundreds) plus a large app. */
#define LOAM_MAX_GLOBALS 4096
static GVar gvars[LOAM_MAX_GLOBALS];
static int ngvars;

static AstNode *find_struct(const char *name);
static Type *struct_type_of(AstNode *st);
static Type *make_struct_inst(AstNode *st, Type **args, size_t n);

static int scope_is_inside(Scope *s, Scope *outer) {
    for (; s; s = s->parent)
        if (s == outer) return 1;
    return 0;
}

/** Record a local captured by the current closure (Copy only). */
static void add_cap(AstNode *clos, const char *name, Type *ty) {
    if (!clos || !name) return;
    for (size_t i = 0; i < clos->as.fn.cap_count; i++)
        if (strcmp(clos->as.fn.caps[i], name) == 0) return;
    size_t n = clos->as.fn.cap_count;
    clos->as.fn.caps = realloc(clos->as.fn.caps, (n + 1) * sizeof(char *));
    clos->as.fn.cap_types = realloc(clos->as.fn.cap_types, (n + 1) * sizeof(Type *));
    clos->as.fn.caps[n] = loam_dup(name);
    clos->as.fn.cap_types[n] = ty;
    clos->as.fn.cap_count = n + 1;
}

/** 1 if `n` is a capturing closure or a local that holds one. */
static int expr_is_cap_fn(AstNode *n) {
    if (!n) return 0;
    if (n->kind == AST_CLOSURE) return n->as.fn.cap_count > 0;
    if (n->kind == AST_IDENT) {
        int capfn = 0;
        if (scope_find_s(n->as.ident.name, NULL, NULL, &capfn, NULL, NULL)) return capfn;
    }
    return 0;
}

/** Substitute type params in `t` (for monomorphization). */
static Type *subst_type(Type *t, const char **names, Type **args, size_t n) {
    if (!t) return t;
    if (t->kind == TY_PARAM) {
        for (size_t i = 0; i < n; i++)
            if (names[i] && t->name && strcmp(names[i], t->name) == 0 && args[i])
                return args[i];
        return t;
    }
    if (t->kind == TY_PTR) return type_ptr(subst_type(t->elem, names, args, n), t->is_mut);
    if (t->kind == TY_BOX) return type_box(subst_type(t->elem, names, args, n));
    if (t->kind == TY_ARRAY) return type_array(subst_type(t->elem, names, args, n), t->array_len);
    if (t->kind == TY_VEC) return type_vec(subst_type(t->elem, names, args, n));
    if (t->kind == TY_STRUCT && t->param_count) {
        Type **ps = (Type **)calloc(t->param_count, sizeof(Type *));
        int changed = 0;
        for (size_t i = 0; i < t->param_count; i++) {
            ps[i] = subst_type(t->params[i], names, args, n);
            if (ps[i] != t->params[i]) changed = 1;
        }
        if (!changed) {
            free(ps);
            return t;
        }
        AstNode *st = find_struct(t->name);
        Type *r = st ? make_struct_inst(st, ps, t->param_count) : t;
        free(ps);
        return r;
    }
    if (t->kind == TY_PROC) {
        Type **ps = NULL;
        if (t->param_count) ps = calloc(t->param_count, sizeof(Type *));
        for (size_t i = 0; i < t->param_count; i++)
            ps[i] = subst_type(t->params[i], names, args, n);
        return type_proc(ps, t->param_count, subst_type(t->ret ? t->ret : ty_void(), names, args, n));
    }
    return t;
}

static int type_has_param(Type *t) {
    if (!t) return 0;
    if (t->kind == TY_PARAM) return 1;
    if (t->elem && type_has_param(t->elem)) return 1;
    if (t->ret && type_has_param(t->ret)) return 1;
    for (size_t i = 0; i < t->param_count; i++) {
        if (type_has_param(t->params[i])) return 1;
    }
    return 0;
}

/** Unify generic pattern `pat` with `got`, filling `bound`. */
static int unify(Type *pat, Type *got, const char **names, Type **bound, size_t n) {
    if (!pat || !got) return 0;
    if (pat->kind == TY_PARAM) {
        for (size_t i = 0; i < n; i++) {
            if (!names[i] || !pat->name || strcmp(names[i], pat->name) != 0) continue;
            if (bound[i]) return type_eq(bound[i], got);
            bound[i] = got;
            return 1;
        }
        return 0;
    }
    if (got->kind == TY_PARAM) return type_eq(pat, got);
    if (pat->kind != got->kind) return 0;
    switch (pat->kind) {
        case TY_PTR:
            return pat->is_mut == got->is_mut && unify(pat->elem, got->elem, names, bound, n);
        case TY_BOX:
            return unify(pat->elem, got->elem, names, bound, n);
        case TY_ARRAY:
            return pat->array_len == got->array_len &&
                   unify(pat->elem, got->elem, names, bound, n);
        case TY_VEC:
            return unify(pat->elem, got->elem, names, bound, n);
        case TY_STRUCT: {
            if (!pat->name || !got->name || strcmp(pat->name, got->name) != 0) return 0;
            if (pat->param_count != got->param_count) return 0;
            for (size_t i = 0; i < pat->param_count; i++)
                if (!unify(pat->params[i], got->params[i], names, bound, n)) return 0;
            return 1;
        }
        case TY_PROC: {
            if (pat->param_count != got->param_count) return 0;
            Type *pr = pat->ret ? pat->ret : ty_void();
            Type *gr = got->ret ? got->ret : ty_void();
            if (!unify(pr, gr, names, bound, n)) return 0;
            for (size_t i = 0; i < pat->param_count; i++)
                if (!unify(pat->params[i], got->params[i], names, bound, n)) return 0;
            return 1;
        }
        default:
            return 1;
    }
}

static void cname_append_ty(char *buf, size_t cap, const Type *t) {
    if (t && t->kind == TY_VEC) {
        size_t used = strlen(buf);
        if (used + 2 < cap) {
            buf[used++] = 'v';
            buf[used] = '\0';
        }
        cname_append_ty(buf, cap, t->elem);
        return;
    }
    const char *nm = type_name(t);
    size_t used = strlen(buf);
    for (const char *p = nm; *p && used + 1 < cap; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            buf[used++] = c;
        else if (used && buf[used - 1] != '_')
            buf[used++] = '_';
    }
    buf[used] = '\0';
}

static char *mono_cname(AstNode *fn, Type **args, size_t n) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", fn->as.fn.cname ? fn->as.fn.cname : fn->as.fn.name);
    for (size_t i = 0; i < n; i++) {
        size_t used = strlen(buf);
        if (used + 3 >= sizeof buf) break;
        snprintf(buf + used, sizeof buf - used, "__");
        cname_append_ty(buf, sizeof buf, args[i]);
    }
    return loam_dup(buf);
}

static void record_mono(AstNode *fn, Type **args, size_t n, const char *cname) {
    for (int i = 0; i < nmono; i++) {
        if (monos[i].fn != fn || monos[i].n != n) continue;
        int same = 1;
        for (size_t k = 0; k < n; k++)
            if (!type_eq(monos[i].args[k], args[k])) same = 0;
        if (same) return;
    }
    if (nmono >= LOAM_MAX_MONOS) {
        err(fn->loc, "too many generic instantiations (max %d)", LOAM_MAX_MONOS);
        return;
    }
    Type **copy = calloc(n, sizeof(Type *));
    if (n && args) memcpy(copy, args, n * sizeof(Type *));
    monos[nmono].fn = fn;
    monos[nmono].args = copy;
    monos[nmono].n = n;
    monos[nmono].cname = loam_dup(cname);
    nmono++;
}

static int module_imported(AstNode *prog, const char *name) {
    if (!prog) return 0;
    for (size_t i = 0; i < prog->as.program.import_count; i++) {
        AstNode *im = prog->as.program.imports[i];
        if (im && im->as.import.alias && strcmp(im->as.import.alias, name) == 0)
            return 1;
    }
    return 0;
}

/* Module for a qualified `name.fn` / `name.global` reference. Prefer the
   module the *current* module imported under that alias: file stems name the
   entry module (e.g. `raygui.loam`), so a global scan would otherwise return a
   file that happens to share the alias instead of the imported module. */
static LoamModule *find_mod(const char *name) {
    AstNode *prog = Gcur >= 0 && Gcur < Gn ? Gmods[Gcur].ast : NULL;
    if (prog) {
        for (size_t i = 0; i < prog->as.program.import_count; i++) {
            AstNode *im = prog->as.program.imports[i];
            if (!im || !im->as.import.alias || !im->as.import.resolved) continue;
            if (strcmp(im->as.import.alias, name) != 0) continue;
            for (int m = 0; m < Gn; m++) {
                if (Gmods[m].path && strcmp(Gmods[m].path, im->as.import.resolved) == 0)
                    return &Gmods[m];
            }
        }
    }
    for (int i = 0; i < Gn; i++) {
        if (Gmods[i].name && strcmp(Gmods[i].name, name) == 0) return &Gmods[i];
    }
    return NULL;
}

/** Point `mod.fn` / `mod.global` prefixes at the module file for hover/def. */
static void mark_module_ident(AstNode *id, const char *name) {
    if (!id || id->kind != AST_IDENT || !name) return;
    LoamModule *m = find_mod(name);
    if (!m || !m->ast) return;
    id->as.ident.resolved = m->ast;
    id->as.ident.def_loc.file = m->path;
    id->as.ident.def_loc.line = 1;
    id->as.ident.def_loc.col = 1;
    id->as.ident.def_loc.end_line = 1;
    int n = m->name ? (int)strlen(m->name) : 1;
    id->as.ident.def_loc.end_col = n + 1;
}

static AstNode *find_fn_in(LoamModule *m, const char *name) {
    if (!m || !m->ast) return NULL;
    AstNode *p = m->ast;
    for (size_t i = 0; i < p->as.program.decl_count; i++) {
        AstNode *d = p->as.program.decls[i];
        if (d->kind == AST_FN_DECL && strcmp(d->as.fn.name, name) == 0) return d;
    }
    return NULL;
}

static AstNode *find_enum_in(LoamModule *m, const char *name) {
    if (!m || !m->ast || !name) return NULL;
    AstNode *p = m->ast;
    for (size_t i = 0; i < p->as.program.decl_count; i++) {
        AstNode *d = p->as.program.decls[i];
        if (d->kind == AST_ENUM_DECL && d->as.enm.name && strcmp(d->as.enm.name, name) == 0)
            return d;
    }
    return NULL;
}

static AstNode *find_struct_in(LoamModule *m, const char *name) {
    if (!m || !m->ast || !name) return NULL;
    AstNode *p = m->ast;
    for (size_t i = 0; i < p->as.program.decl_count; i++) {
        AstNode *d = p->as.program.decls[i];
        if (d->kind == AST_STRUCT_DECL && d->as.strct.name && strcmp(d->as.strct.name, name) == 0)
            return d;
    }
    return NULL;
}

/** Resolve a bare name by import depth: this module's own declarations first,
 *  then each direct import in reverse source order (a later import shadows an
 *  earlier one), and only then the imports of those imports. This is what lets
 *  a barrel module re-export widgets by importing them. */
static AstNode *lookup_unqualified(const char *name,
                                   AstNode *(*pick)(LoamModule *, const char *)) {
    if (!name) return NULL;
    AstNode *d = pick(&Gmods[Gcur], name);
    if (d) return d;
    AstNode *prog = Gmods[Gcur].ast;
    if (!prog) return NULL;
    size_t i = prog->as.program.import_count;
    while (i > 0) {
        i--;
        AstNode *im = prog->as.program.imports[i];
        if (!im || !im->as.import.alias) continue;
        LoamModule *m = find_mod(im->as.import.alias);
        d = m ? pick(m, name) : NULL;
        if (d) return d;
    }
    i = prog->as.program.import_count;
    while (i > 0) {
        i--;
        AstNode *im = prog->as.program.imports[i];
        if (!im || !im->as.import.alias) continue;
        LoamModule *m = find_mod(im->as.import.alias);
        if (!m || !m->ast) continue;
        size_t k = m->ast->as.program.import_count;
        while (k > 0) {
            k--;
            AstNode *im2 = m->ast->as.program.imports[k];
            if (!im2 || !im2->as.import.alias) continue;
            LoamModule *m2 = find_mod(im2->as.import.alias);
            d = m2 ? pick(m2, name) : NULL;
            if (d) return d;
        }
    }
    return NULL;
}

/** A struct reachable from the current module by unqualified name. Unlike
 *  find_struct this does not fall back to every loaded module. */
static AstNode *find_struct_visible(const char *name) {
    return lookup_unqualified(name, find_struct_in);
}

/** An enum visible from the current module, by unqualified name. */
static AstNode *find_enum(const char *name) {
    return lookup_unqualified(name, find_enum_in);
}

/** `Name.Variant` → its int value. 0 if there is no such variant. */
static int enum_value(AstNode *en, const char *v, int64_t *out) {
    if (!en || !v) return 0;
    for (size_t i = 0; i < en->as.enm.count; i++) {
        if (en->as.enm.vnames[i] && strcmp(en->as.enm.vnames[i], v) == 0) {
            *out = en->as.enm.vals[i];
            return 1;
        }
    }
    return 0;
}

/** Rewrite `n` in place into the int literal `v`. */
static Type *lower_to_int(AstNode *n, int64_t v) {
    n->kind = AST_NUMBER;
    n->as.lit.value = v;
    n->ty = ty_int();
    n->place_mut = 0;
    return n->ty;
}

/** A numeric literal that context may still give a type (§3.4). A negated
 *  literal counts, so `-128` can take the i8 minimum. */
static int is_num_lit(const AstNode *n) {
    if (!n) return 0;
    if (n->kind == AST_NUMBER || n->kind == AST_FLOAT) return 1;
    if (n->kind == AST_UNARY && n->as.unary.op == TOK_MINUS)
        return is_num_lit(n->as.unary.operand);
    return 0;
}

/** 1 if integer literal `v` fits integer type `t`. */
static int int_lit_fits(int64_t v, const Type *t) {
    if (!t || t->kind != TY_INT) return 0;
    if (t->is_unsigned) {
        if (v < 0) return 0;
        if (t->bits >= 64) return 1;
        return v < ((int64_t)1 << t->bits);
    }
    if (t->bits >= 64) return 1;
    if (t->bits == 32) return v >= INT32_MIN && v <= INT32_MAX;
    if (t->bits == 16) return v >= INT16_MIN && v <= INT16_MAX;
    return v >= INT8_MIN && v <= INT8_MAX;
}

/** A literal checked against `expect`: adopt it when it fits, otherwise
 *  report at the literal and stay at the default width (so checking can
 *  continue without cascading). */
static Type *lit_with_expect(AstNode *n, Type *expect) {
    if (n->kind == AST_FLOAT) {
        if (expect && expect->kind == TY_FLOAT) {
            n->ty = expect;
            return n->ty;
        }
        n->ty = ty_float();
        return n->ty;
    }
    if (expect && expect->kind == TY_FLOAT) {
        n->kind = AST_FLOAT;
        n->as.lit.f = (double)n->as.lit.value;
        n->ty = expect;
        return n->ty;
    }
    if (expect && expect->kind == TY_INT) {
        int64_t v = n->as.lit.value;
        if (!int_lit_fits(v, expect))
            err(n->loc, "literal %lld does not fit %s", (long long)v, type_name(expect));
        n->ty = expect;
        return n->ty;
    }
    /* Unconstrained: prefer the default integer, but a value that only fits a
       wider type takes it rather than silently truncating (§3.6). */
    if (!int_lit_fits(n->as.lit.value, ty_int())) {
        Type *wide = ty_int_bits(64, 0);
        if (ty_int()->bits < 64 && int_lit_fits(n->as.lit.value, wide))
            n->ty = wide;
        else
            err(n->loc, "integer literal %lld is too large", (long long)n->as.lit.value);
        return n->ty;
    }
    n->ty = ty_int();
    return n->ty;
}


static Type *fn_type_of(AstNode *fn);
static Type *peel_ref(Type *t);

/** 1 if `fn`'s first parameter can be the method receiver `recv`. */
static int method_recv_matches(AstNode *fn, Type *recv) {
    if (!fn || !recv) return 0;
    Type *ft = fn_type_of(fn);
    if (!ft || ft->kind != TY_PROC || ft->param_count < 1 || !ft->params[0]) return 0;
    Type *pt = peel_ref(ft->params[0]);
    Type *rt = peel_ref(recv);
    if (!pt || !rt) return 0;
    if (pt->kind == TY_PARAM) return 1;
    if (type_eq(pt, rt)) return 1;
    /* `Signal<T>` vs `Signal<int>` — same named struct, different args. */
    if (pt->kind == TY_STRUCT && rt->kind == TY_STRUCT &&
        pt->name && rt->name && strcmp(pt->name, rt->name) == 0)
        return 1;
    return 0;
}

/** UFCS: current module, then last matching import. `n.radius(8)` skips
 *  `spacing.radius(int, int)` and takes `zeus.radius(Node, int)`. Two Node
 *  methods named `size` — later import wins (`ui.size` over `zeus.size`). */
static AstNode *find_method(const char *fnn, Type *recv) {
    AstNode *fn = find_fn_in(&Gmods[Gcur], fnn);
    if (fn && method_recv_matches(fn, recv)) return fn;
    AstNode *prog = Gmods[Gcur].ast;
    if (!prog || prog->as.program.import_count == 0) return NULL;
    size_t i = prog->as.program.import_count;
    while (i > 0) {
        i--;
        AstNode *im = prog->as.program.imports[i];
        if (!im || !im->as.import.alias) continue;
        LoamModule *m = find_mod(im->as.import.alias);
        fn = m ? find_fn_in(m, fnn) : NULL;
        if (fn && method_recv_matches(fn, recv)) return fn;
    }
    return NULL;
}

static AstNode *find_global_in(LoamModule *m, const char *name) {
    if (!m || !m->ast || !name) return NULL;
    AstNode *p = m->ast;
    for (size_t i = 0; i < p->as.program.decl_count; i++) {
        AstNode *d = p->as.program.decls[i];
        if (d->kind == AST_VAR_DECL && d->as.var.name && strcmp(d->as.var.name, name) == 0)
            return d;
    }
    return NULL;
}

/** Bodyless fns in these modules are the link boundary (`loam_<mod>_<fn>`).
 *  Not a keyword: ordinary empty `fn` in a std seam module. See docs/boundary.md. */
static int is_ffi_mod(const char *name) {
    if (!name) return 0;
    return strcmp(name, "zeus") == 0 || strcmp(name, "http") == 0 ||
           strcmp(name, "fmt") == 0 || strcmp(name, "maya") == 0 ||
           strcmp(name, "platform") == 0 || strcmp(name, "sys") == 0 ||
           strcmp(name, "net") == 0 || strcmp(name, "async") == 0 ||
           strcmp(name, "thread") == 0;
}

/* User (non-std) modules may declare bodyless C-seam hooks the same way std
 * modules do: an empty body marks the fn as intrinsic (extern C symbol, no
 * Loam body emitted — see codegen_c.c). std modules keep the allow-list
 * above; `main` stays a plain function so `fn main() {}` still links. */
static int user_seam_mod(const char *path) {
    if (!path) return 0;
    size_t sd = strlen(LOAM_STD_DIR);
    return !(strncmp(path, LOAM_STD_DIR, sd) == 0 &&
             (path[sd] == '/' || path[sd] == '\0'));
}

static AstNode *find_struct(const char *name) {
    if (Gmods && Gn > 0) {
        AstNode *d = lookup_unqualified(name, find_struct_in);
        if (d) return d;
    }
    for (int mi = 0; mi < Gn; mi++) {
        AstNode *p = Gmods[mi].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind == AST_STRUCT_DECL && strcmp(d->as.strct.name, name) == 0)
                return d;
        }
    }
    return NULL;
}

static Type *resolve_type(AstNode *tn);

static Type *struct_type_of(AstNode *st) {
    if (st->ty) return st->ty;
    const char **save_tp = cur_tparams;
    size_t save_n = cur_ntparams;
    cur_tparams = st->as.strct.tparams;
    cur_ntparams = st->as.strct.tparam_count;
    Type *t = type_new(TY_STRUCT);
    t->name = loam_dup(st->as.strct.name);
    t->must_check = st->as.strct.is_must_check;
    t->field_count = st->as.strct.field_count;
    t->field_names = calloc(t->field_count, sizeof(char *));
    t->field_types = calloc(t->field_count, sizeof(Type *));
    if (st->as.strct.tparam_count) {
        t->param_count = st->as.strct.tparam_count;
        t->params = calloc(t->param_count, sizeof(Type *));
        for (size_t i = 0; i < t->param_count; i++)
            t->params[i] = type_param(st->as.strct.tparams[i]);
    }
    for (size_t i = 0; i < t->field_count; i++) {
        t->field_names[i] = loam_dup(st->as.strct.fields[i].name);
        t->field_types[i] = resolve_type(st->as.strct.fields[i].type);
    }
    st->ty = t;
    cur_tparams = save_tp;
    cur_ntparams = save_n;
    return t;
}

Type *typecheck_signal_type(void) {
    AstNode *st = find_struct("Signal");
    if (!st) return NULL;
    if (!st->as.strct.tparam_count) return struct_type_of(st);
    Type *args[1] = { ty_int() };
    return make_struct_inst(st, args, 1);
}

/** Captured `let mut int` is component state: keep the Loam type as int (props
    stay int) but record a Signal handle in the closure env. */
static Type *cap_type_for(AstNode *dnode, Type *ty) {
    if (!dnode || dnode->kind != AST_VAR_DECL || !dnode->as.var.is_mut) return ty;
    if (!ty || ty->kind != TY_INT) return ty;
    for (int i = 0; i < ngvars; i++)
        if (gvars[i].var == dnode) return ty;
    Type *sig = typecheck_signal_type();
    if (!sig) return ty;
    dnode->flags |= ASTF_STATE;
    return sig;
}

static Type *make_struct_inst(AstNode *st, Type **args, size_t n) {
    if (!st) return ty_void();
    Type *tmpl = struct_type_of(st);
    for (int i = 0; i < nstruct_insts; i++) {
        Type *ex = struct_insts[i];
        if (!ex || !ex->name || !tmpl->name || strcmp(ex->name, tmpl->name) != 0) continue;
        if (ex->param_count != n) continue;
        int same = 1;
        for (size_t k = 0; k < n; k++)
            if (!type_eq(ex->params[k], args[k])) same = 0;
        if (same) return ex;
    }
    Type *t = type_new(TY_STRUCT);
    t->name = loam_dup(st->as.strct.name);
    t->must_check = tmpl->must_check;
    t->param_count = n;
    if (n) {
        t->params = calloc(n, sizeof(Type *));
        memcpy(t->params, args, n * sizeof(Type *));
    }
    t->field_count = tmpl->field_count;
    t->field_names = calloc(t->field_count, sizeof(char *));
    t->field_types = calloc(t->field_count, sizeof(Type *));
    for (size_t i = 0; i < t->field_count; i++) {
        t->field_names[i] = loam_dup(tmpl->field_names[i]);
        t->field_types[i] = subst_type(tmpl->field_types[i], st->as.strct.tparams, args, n);
    }
    if (nstruct_insts >= LOAM_MAX_STRUCT_INSTS) {
        err(st->loc, "too many generic struct instantiations (max %d)", LOAM_MAX_STRUCT_INSTS);
        return t;
    }
    struct_insts[nstruct_insts++] = t;
    return t;
}

/** `int`/`float` aliases plus the sized primitives (i8..u64, f32/f64). */
static Type *primitive_type(const char *nm) {
    if (!nm) return NULL;
    if (strcmp(nm, "int") == 0) return ty_int();
    if (strcmp(nm, "float") == 0) return ty_float();
    if (nm[0] == 'i' || nm[0] == 'u') {
        int uns = nm[0] == 'u';
        const char *d = nm + 1;
        if (strcmp(d, "8") == 0) return ty_int_bits(8, uns);
        if (strcmp(d, "16") == 0) return ty_int_bits(16, uns);
        if (strcmp(d, "32") == 0) return ty_int_bits(32, uns);
        if (strcmp(d, "64") == 0) return ty_int_bits(64, uns);
    }
    if (strcmp(nm, "f32") == 0) return ty_float_bits(32);
    if (strcmp(nm, "f64") == 0) return ty_float_bits(64);
    return NULL;
}

/** AST type syntax → Type. */
static Type *resolve_type(AstNode *tn) {
    if (!tn) return ty_void();
    if (tn->kind != AST_TYPE) return ty_void();
    if (tn->as.type.tag == 0 || tn->as.type.tag == 1)
        return type_ptr(resolve_type(tn->as.type.elem), tn->as.type.tag == 1);
    if (tn->as.type.tag == 3) {
        Type *et = resolve_type(tn->as.type.elem);
        if (tn->as.type.array_len < 0) return type_vec(et);
        return type_array(et, tn->as.type.array_len);
    }
    if (tn->as.type.tag == 4)
        return type_box(resolve_type(tn->as.type.elem));
    if (tn->as.type.tag == 5) {
        size_t n = tn->as.type.fn_param_count;
        Type **ps = NULL;
        if (n) ps = calloc(n, sizeof(Type *));
        for (size_t i = 0; i < n; i++) ps[i] = resolve_type(tn->as.type.fn_params[i]);
        return type_proc(ps, n, resolve_type(tn->as.type.elem));
    }
    const char *nm = tn->as.type.name;
    if (!nm) return ty_void();
    {
        Type *prim = primitive_type(nm);
        if (prim) return prim;
    }
    if (strcmp(nm, "bool") == 0) return ty_bool();
    if (strcmp(nm, "string") == 0) return ty_string();
    if (strcmp(nm, "void") == 0) return ty_void();
    for (size_t i = 0; i < cur_ntparams; i++) {
        if (cur_tparams[i] && strcmp(cur_tparams[i], nm) == 0)
            return type_param(nm);
    }
    {
        AstNode *en = find_enum(nm);
        if (en) {
            if (tn->as.type.targ_count)
                err(tn->loc, "enum '%s' does not take type arguments", nm);
            return ty_int();
        }
    }
    AstNode *st = find_struct(nm);
    if (st) {
        size_t want = st->as.strct.tparam_count;
        size_t got = tn->as.type.targ_count;
        if (want == 0) {
            if (got) err(tn->loc, "type '%s' does not take type arguments", nm);
            return struct_type_of(st);
        }
        if (got != want) {
            err(tn->loc, "'%s' requires %zu type argument(s), got %zu", nm, want, got);
            return struct_type_of(st);
        }
        Type **args = calloc(got, sizeof(Type *));
        for (size_t i = 0; i < got; i++) args[i] = resolve_type(tn->as.type.targs[i]);
        Type *inst = make_struct_inst(st, args, got);
        free(args);
        return inst;
    }
    err(tn->loc, "unknown type '%s'", nm);
    return ty_void();
}

static Type *fn_type_of(AstNode *fn) {
    if (fn->ty) return fn->ty;
    const char **save_tp = cur_tparams;
    size_t save_n = cur_ntparams;
    cur_tparams = fn->as.fn.tparams;
    cur_ntparams = fn->as.fn.tparam_count;
    Type **ps = NULL;
    size_t n = fn->as.fn.param_count;
    if (n) ps = calloc(n, sizeof(Type *));
    for (size_t i = 0; i < n; i++) {
        if (fn->as.fn.params[i].type)
            ps[i] = resolve_type(fn->as.fn.params[i].type);
        else if (fn->ty && i < fn->ty->param_count)
            ps[i] = fn->ty->params[i];
        else
            ps[i] = ty_void();
    }
    Type *ret = fn->as.fn.ret_type ? resolve_type(fn->as.fn.ret_type) : ty_void();
    Type *t = type_proc(ps, n, ret);
    fn->ty = t;
    cur_tparams = save_tp;
    cur_ntparams = save_n;
    return t;
}

static Type *check_expr_ty(AstNode *n, Type *expect);
static Type *check_expr(AstNode *n);
static void check_block(AstNode *n);
static void check_block_in_scope(AstNode *n);
static void check_stmt(AstNode *n);

/** A block in value position: every statement is checked, and the type is
 *  that of a trailing expression statement (void if there is none). */
static Type *check_block_value(AstNode *blk, Type *expect) {
    if (!blk || blk->kind != AST_BLOCK) return ty_void();
    Type *t = ty_void();
    scope_push();
    for (size_t i = 0; i < blk->as.block.stmt_count; i++) {
        AstNode *st = blk->as.block.stmts[i];
        int last = (i + 1 == blk->as.block.stmt_count);
        if (last && st && st->kind == AST_EXPR_STMT) {
            t = check_expr_ty(st->as.expr_stmt.expr, expect);
            st->ty = t;
        } else {
            check_stmt(st);
        }
    }
    scope_pop();
    return t;
}

static Type *peel_ref(Type *t) {
    if (!t) return t;
    if (t->kind == TY_PTR || t->kind == TY_BOX) return t->elem;
    return t;
}

static int is_printable(Type *t) {
    if (!t) return 0;
    return t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_BOOL || t->kind == TY_STRING;
}

static AstNode *wrap_addr(AstNode **slot, int is_mut) {
    AstNode *inner = *slot;
    AstNode *a = ast_addr(inner, is_mut, inner->loc);
    a->ty = type_ptr(inner->ty, is_mut);
    a->place_mut = 0;
    *slot = a;
    return a;
}

/** If `at` is owned and `pt` is &T/&mut T, insert an addr node (auto-borrow). */
static void match_arg(AstNode **slot, Type *pt, Type *at) {
    AstNode *arg = *slot;
    if (!pt || !at) return;
    if (pt->kind == TY_PTR) {
        if (at->kind == TY_PTR) {
            if (type_eq(at->elem, pt->elem)) {
                if (pt->is_mut && !at->is_mut)
                    err(arg->loc, "cannot pass &T where &mut T is required");
                return;
            }
        }
        Type *owned = at;
        if (at->kind == TY_BOX) owned = at->elem;
        if (type_eq(owned, pt->elem) || type_eq(at, pt->elem)) {
            if (pt->is_mut && !arg->place_mut && at->kind != TY_BOX)
                err(arg->loc, "cannot auto-borrow immutable place as &mut");
            if (at->kind == TY_BOX) {
                if (pt->is_mut && !arg->place_mut)
                    err(arg->loc, "cannot mutably borrow immutable box");
                AstNode *d = ast_deref(arg, arg->loc);
                d->ty = at->elem;
                d->place_mut = arg->place_mut;
                *slot = d;
                wrap_addr(slot, pt->is_mut);
            } else if (at->kind != TY_PTR) {
                wrap_addr(slot, pt->is_mut);
            }
            return;
        }
        err(arg->loc, "expected %s, got %s", type_name(pt), type_name(at));
    } else {
        if (!type_eq(at, pt))
            err(arg->loc, "expected %s, got %s", type_name(pt), type_name(at));
    }
}


/** Named arguments arrive from the parser as one struct literal with no type
 *  name, in the last argument slot; the callee's last parameter names it. */
static int is_props_lit(AstNode *a) {
    return a && a->kind == AST_STRUCT_LIT && !a->as.struct_lit.type_name;
}

/** The module index that declares `d`, or Gcur if it is not a top-level decl. */
static int module_of_decl(AstNode *d) {
    for (int m = 0; m < Gn; m++) {
        AstNode *p = Gmods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++)
            if (p->as.program.decls[i] == d) return m;
    }
    return Gcur;
}

/** Copy a constant default expression: literals, negated numbers, names,
 *  enum constants, and struct literals. Defaults are data, not computation. */
static AstNode *clone_const(AstNode *e) {
    if (!e) return NULL;
    switch (e->kind) {
        case AST_NUMBER: return ast_number(e->as.lit.value, e->loc);
        case AST_FLOAT: return ast_float(e->as.lit.f, e->loc);
        case AST_BOOL: return ast_bool(e->as.lit.b, e->loc);
        case AST_STRING: return ast_string(loam_dup(e->as.lit.str ? e->as.lit.str : ""), e->loc);
        case AST_IDENT: return ast_ident(loam_dup(e->as.ident.name), e->loc);
        case AST_UNARY: return ast_unary(e->as.unary.op, clone_const(e->as.unary.operand), e->loc);
        case AST_FIELD:
            return ast_field(clone_const(e->as.access.target), loam_dup(e->as.access.field),
                             e->as.access.via_colon, e->loc);
        case AST_CALL: {
            size_t n = e->as.call.arg_count;
            AstNode **as = n ? (AstNode **)calloc(n, sizeof(AstNode *)) : NULL;
            for (size_t i = 0; i < n; i++) as[i] = clone_const(e->as.call.args[i]);
            return ast_call(clone_const(e->as.call.callee), as, n, e->loc);
        }
        case AST_STRUCT_LIT: {
            size_t n = e->as.struct_lit.field_count;
            FieldInit *fi = n ? (FieldInit *)calloc(n, sizeof(FieldInit)) : NULL;
            for (size_t i = 0; i < n; i++) {
                fi[i].name = loam_dup(e->as.struct_lit.fields[i].name);
                fi[i].init = clone_const(e->as.struct_lit.fields[i].init);
            }
            return ast_struct_lit(e->as.struct_lit.type_name
                                      ? loam_dup(e->as.struct_lit.type_name) : NULL,
                                  fi, n, e->loc);
        }
        default:
            return NULL;
    }
}

/** 1 if every field of `st` has a declared default, so the whole props struct
 *  can be omitted at a call site (`Text("hi")`). */
static int struct_all_defaulted(AstNode *st) {
    if (!st || st->kind != AST_STRUCT_DECL) return 0;
    for (size_t i = 0; i < st->as.strct.field_count; i++)
        if (!st->as.strct.fields[i].def) return 0;
    return 1;
}

/** A value where `fn() -> T` is expected becomes a thunk, so the read happens
 *  inside the closure and is tracked as that node's own effect. Closures and
 *  names that already hold a function are passed through. */
/** The type of a name or a field path, without checking (and so without
 *  rewriting) the expression. NULL when it cannot be told cheaply. Used only
 *  to decide whether an argument already holds a function. */
static Type *peek_type(AstNode *a) {
    if (!a) return NULL;
    if (a->kind == AST_IDENT) {
        Type *lt = NULL;
        if (scope_find(a->as.ident.name, &lt, NULL)) return lt;
        AstNode *f = lookup_unqualified(a->as.ident.name, find_fn_in);
        return f ? fn_type_of(f) : NULL;
    }
    if (a->kind == AST_FIELD && !a->as.access.via_colon && a->as.access.field) {
        if (a->as.access.target && a->as.access.target->kind == AST_IDENT) {
            const char *base = a->as.access.target->as.ident.name;
            if (module_imported(Gmods[Gcur].ast, base)) {
                LoamModule *m = find_mod(base);
                AstNode *gv = m ? find_global_in(m, a->as.access.field) : NULL;
                return gv ? gv->ty : NULL;
            }
        }
        Type *bt = peel_ref(peek_type(a->as.access.target));
        if (!bt || bt->kind != TY_STRUCT) return NULL;
        for (size_t i = 0; i < bt->field_count; i++)
            if (bt->field_names[i] && strcmp(bt->field_names[i], a->as.access.field) == 0)
                return bt->field_types[i];
        return NULL;
    }
    return NULL;
}

static int wants_thunk(Type *pt, AstNode *arg) {
    if (!pt || pt->kind != TY_PROC || pt->param_count != 0) return 0;
    if (!arg || arg->kind == AST_CLOSURE) return 0;
    Type *at = peek_type(arg);
    if (at && at->kind == TY_PROC) return 0;
    return 1;
}

static AstNode *make_thunk(AstNode *arg) {
    AstNode **st = (AstNode **)malloc(sizeof(AstNode *));
    if (!st) loam_fatal("out of memory");
    st[0] = ast_expr_stmt(arg, arg->loc);
    AstNode *body = ast_block(st, 1, arg->loc);
    AstNode *c = ast_fn(NULL, NULL, 0, NULL, body, arg->loc);
    c->kind = AST_CLOSURE;
    return c;
}

/** Check an argument against `pt`, wrapping it in a thunk when `pt` is a
 *  no-argument function type and the argument is a plain value. */
static Type *check_arg_ty(AstNode **slot, Type *pt) {
    if (wants_thunk(pt, *slot)) *slot = make_thunk(*slot);
    return check_expr_ty(*slot, pt);
}

/** Check a call against a procedure type; monomorphize if `named` is generic. */
static Type *finish_proc_call(AstNode *n, Type *ft, AstNode *named) {
    if (!ft) {
        err(n->loc, "invalid call");
        return ty_void();
    }
    size_t nt = named ? named->as.fn.tparam_count : 0;
    Type **bound = NULL;
    if (nt) bound = calloc(nt, sizeof(Type *));
    const char *nm = named && named->as.fn.name ? named->as.fn.name : "fn";
    /* Named arguments become the callee's last parameter, which must be a
       struct; an all-defaulted props struct may be left out entirely. */
    if (ft->param_count) {
        Type *last = ft->params[ft->param_count - 1];
        if (last && last->kind == TY_STRUCT && last->name) {
            if (n->as.call.arg_count == ft->param_count &&
                is_props_lit(n->as.call.args[ft->param_count - 1])) {
                n->as.call.args[ft->param_count - 1]->as.struct_lit.type_name =
                    loam_dup(last->name);
            } else if (n->as.call.arg_count > ft->param_count &&
                       is_props_lit(n->as.call.args[n->as.call.arg_count - 1])) {
                /* `Button("Go", look = Solid)`: extra leading positionals
                   fill the first fields of the trailing props struct. */
                size_t extra = n->as.call.arg_count - ft->param_count;
                AstNode *lit = n->as.call.args[n->as.call.arg_count - 1];
                AstNode *st = find_struct(last->name);
                if (st && extra <= st->as.strct.field_count) {
                    size_t oldn = lit->as.struct_lit.field_count;
                    FieldInit *nf = (FieldInit *)calloc(extra + oldn, sizeof(FieldInit));
                    size_t k = 0;
                    for (size_t i = 0; i < extra; i++) {
                        nf[k].name = loam_dup(st->as.strct.fields[i].name);
                        nf[k].init = n->as.call.args[i];
                        k++;
                    }
                    memcpy(nf + k, lit->as.struct_lit.fields, oldn * sizeof(FieldInit));
                    free(lit->as.struct_lit.fields);
                    lit->as.struct_lit.fields = nf;
                    lit->as.struct_lit.field_count = extra + oldn;
                    lit->as.struct_lit.type_name = loam_dup(last->name);
                    size_t keep = ft->param_count;
                    AstNode **na = (AstNode **)malloc(keep * sizeof(AstNode *));
                    for (size_t i = 0; i + 1 < keep; i++)
                        na[i] = n->as.call.args[extra + i];
                    na[keep - 1] = lit;
                    n->as.call.args = na;
                    n->as.call.arg_count = keep;
                }
            } else if (n->as.call.arg_count + 1 == ft->param_count &&
                       struct_all_defaulted(find_struct(last->name))) {
                size_t ac = n->as.call.arg_count;
                AstNode **na = (AstNode **)realloc(n->as.call.args,
                                                   (ac + 1) * sizeof(AstNode *));
                if (!na) loam_fatal("out of memory");
                na[ac] = ast_struct_lit(loam_dup(last->name), NULL, 0, n->loc);
                n->as.call.args = na;
                n->as.call.arg_count = ac + 1;
            }
        }
    }
    /* Any other omitted trailing parameters are filled from their declared
       defaults, cloned and type-checked in the module that declares the fn
       (a default may name that module's fns, e.g. `= __noop`). `filled`
       marks those tail args as pre-checked so the loop below skips them. */
    size_t pre = n->as.call.arg_count;
    int filled = 0;
    if (named && pre < ft->param_count) {
        size_t ok = 1;
        for (size_t i = pre; i < ft->param_count; i++)
            if (i >= named->as.fn.param_count || !named->as.fn.params[i].def)
                ok = 0;
        if (ok) {
            for (size_t i = pre; i < ft->param_count; i++) {
                AstNode *def = named->as.fn.params[i].def;
                AstNode *cp = clone_const(def);
                if (!cp) {
                    err(n->loc, "default for parameter '%s' of '%s' is not a constant expression",
                        named->as.fn.params[i].name, nm);
                    ok = 0;
                    break;
                }
                int save_cur = Gcur;
                const char *save_mod = cur_mod_name;
                Gcur = module_of_decl(named);
                cur_mod_name = Gmods[Gcur].name;
                Type *pt = ft->params[i];
                Type *expect = type_has_param(pt) ? NULL : pt;
                Type *dt = check_arg_ty(&cp, expect);
                Gcur = save_cur;
                cur_mod_name = save_mod;
                if (dt && expect && !type_eq(dt, pt))
                    err(n->loc, "default for parameter '%s' of '%s' has type %s, expected %s",
                        named->as.fn.params[i].name, nm, type_name(dt), type_name(pt));
                AstNode **na = (AstNode **)realloc(
                    n->as.call.args, (n->as.call.arg_count + 1) * sizeof(AstNode *));
                if (!na) loam_fatal("out of memory");
                na[n->as.call.arg_count] = cp;
                n->as.call.args = na;
                n->as.call.arg_count++;
            }
            if (ok) filled = 1;
        }
    }
    /* A props literal is nameless, so nothing downstream can resolve it as a
       struct. Report and stop here: checking it against a non-struct
       parameter dereferences that null type name. */
    int bad_props = 0;
    for (size_t i = 0; i < n->as.call.arg_count && i < ft->param_count; i++) {
        if (is_props_lit(n->as.call.args[i])) {
            err(n->as.call.args[i]->loc,
                "named arguments only fill the last parameter, which must be a struct");
            bad_props = 1;
        }
    }
    if (bad_props) {
        free(bound);
        return ft->ret ? ft->ret : ty_void();
    }
    if (n->as.call.arg_count != ft->param_count) {
        err(n->loc, "'%s' expects %zu argument(s), got %zu",
            nm, ft->param_count, n->as.call.arg_count);
        free(bound);
        return ft->ret ? ft->ret : ty_void();
    }
    /* Defaults filled above were checked in the declaring module; the rest
       are checked here, in the caller's module. */
    size_t limit = filled ? pre : ft->param_count;
    for (size_t i = 0; i < limit; i++) {
        AstNode *arg = n->as.call.args[i];
        Type *pt = ft->params[i];
        Type *expect = pt;
        if (nt) {
            expect = subst_type(pt, named->as.fn.tparams, bound, nt);
            if (type_has_param(expect)) expect = NULL;
        }
        Type *at = check_arg_ty(&n->as.call.args[i], expect);
        arg = n->as.call.args[i];
        if (nt) {
            if (!unify(pt, at, named->as.fn.tparams, bound, nt))
                err(arg->loc, "cannot match %s to %s", type_name(at), type_name(pt));
            Type *want = subst_type(pt, named->as.fn.tparams, bound, nt);
            if (!type_has_param(want)) match_arg(&n->as.call.args[i], want, at);
        } else {
            match_arg(&n->as.call.args[i], pt, at);
        }
    }
    /* Defaults filled above were checked in the declaring module. For a
       generic call they still have to unify so `fn f<T>(x: T = 5)` can
       infer T from the default when every argument is omitted. */
    if (nt && filled) {
        for (size_t i = pre; i < ft->param_count; i++) {
            Type *at = n->as.call.args[i] ? n->as.call.args[i]->ty : NULL;
            Type *pt = ft->params[i];
            if (!at || !pt) continue;
            if (!unify(pt, at, named->as.fn.tparams, bound, nt))
                err(n->loc, "cannot match %s to %s", type_name(at), type_name(pt));
        }
    }
    Type *ret = ft->ret ? ft->ret : ty_void();
    if (nt) {
        for (size_t i = 0; i < nt; i++) {
            if (!bound[i]) {
                err(n->loc, "cannot infer type parameter '%s'", named->as.fn.tparams[i]);
                free(bound);
                n->ty = ret;
                return ret;
            }
        }
        ret = subst_type(ret, named->as.fn.tparams, bound, nt);
        int concrete = 1;
        for (size_t i = 0; i < nt; i++)
            if (bound[i]->kind == TY_PARAM) concrete = 0;
        if (concrete) {
            char *cn = mono_cname(named, bound, nt);
            n->as.call.resolved_cname = cn;
            record_mono(named, bound, nt, cn);
        }
        if (named->as.fn.cname && bound && bound[0] &&
            (strcmp(named->as.fn.cname, "loam_zeus_signal") == 0 ||
             strcmp(named->as.fn.cname, "loam_zeus_get") == 0 ||
             strcmp(named->as.fn.cname, "loam_zeus_set") == 0) &&
            !type_is_copy(bound[0]))
            err(n->loc, "Signal<%s> requires a Copy type", type_name(bound[0]));
        if (named->as.fn.cname && bound && bound[0] &&
            (strcmp(named->as.fn.cname, "loam_async_future") == 0 ||
             strcmp(named->as.fn.cname, "loam_async_resolve") == 0 ||
             strcmp(named->as.fn.cname, "loam_async_await_value") == 0) &&
            !type_is_copy(bound[0]))
            err(n->loc, "Future<%s> requires a Copy type", type_name(bound[0]));
        if (named->as.fn.cname && bound && bound[0] &&
            (strcmp(named->as.fn.cname, "loam_thread_chan") == 0 ||
             strcmp(named->as.fn.cname, "loam_thread_send") == 0 ||
             strcmp(named->as.fn.cname, "loam_thread_try_send") == 0 ||
             strcmp(named->as.fn.cname, "loam_thread_recv") == 0 ||
             strcmp(named->as.fn.cname, "loam_thread_try_recv") == 0) &&
            !type_is_send(bound[0]))
            err(n->loc, "Chan<%s> requires a Send type (plain data: no []T, fn, Box, or borrows)",
                type_name(bound[0]));
        free(bound);
    }
    n->ty = ret;
    return ret;
}

/** push/pop on []T, as `push(v, x)` / `v.push(x)` and `pop(v)` / `v.pop()`. */
static int try_vec_builtin(AstNode *n) {
    AstNode *cal = n->as.call.callee;
    int is_push = 0, is_pop = 0, method = 0;
    if (cal && cal->kind == AST_IDENT) {
        if (strcmp(cal->as.ident.name, "push") == 0) is_push = 1;
        else if (strcmp(cal->as.ident.name, "pop") == 0) is_pop = 1;
    } else if (cal && cal->kind == AST_FIELD && !cal->as.access.via_colon &&
               cal->as.access.field) {
        int skip = 0;
        if (cal->as.access.target && cal->as.access.target->kind == AST_IDENT) {
            const char *base = cal->as.access.target->as.ident.name;
            if (module_imported(Gmods[Gcur].ast, base) || strcmp(base, "Box") == 0) skip = 1;
        }
        if (!skip) {
            if (strcmp(cal->as.access.field, "push") == 0) {
                is_push = 1;
                method = 1;
            } else if (strcmp(cal->as.access.field, "pop") == 0) {
                is_pop = 1;
                method = 1;
            }
        }
    }
    if (!is_push && !is_pop) return 0;
    if (method) {
        AstNode *recv = cal->as.access.target;
        cal->as.access.target = NULL;
        size_t ac = n->as.call.arg_count;
        AstNode **na = (AstNode **)malloc((ac + 1) * sizeof(AstNode *));
        na[0] = recv;
        if (ac) memcpy(na + 1, n->as.call.args, ac * sizeof(AstNode *));
        free(n->as.call.args);
        n->as.call.args = na;
        n->as.call.arg_count = ac + 1;
    }
    if (is_push) {
        if (n->as.call.arg_count != 2) {
            err(n->loc, "push expects the array and one element");
            n->ty = ty_void();
            return 1;
        }
        Type *vt = check_expr(n->as.call.args[0]);
        Type *xt = check_expr(n->as.call.args[1]);
        Type *vec = peel_ref(vt);
        if (!vec || vec->kind != TY_VEC) {
            err(n->loc, "push requires []T, got %s", type_name(vt));
            n->ty = ty_void();
            return 1;
        }
        if (!(vt->kind == TY_PTR && vt->is_mut))
            match_arg(&n->as.call.args[0], type_ptr(vec, 1), vt);
        if (!type_eq(xt, vec->elem))
            err(n->as.call.args[1]->loc, "push element has type %s, expected %s",
                type_name(xt), type_name(vec->elem));
        n->as.call.is_vec_push = 1;
        n->ty = ty_void();
        return 1;
    }
    if (n->as.call.arg_count != 1) {
        err(n->loc, "pop expects the array");
        n->ty = ty_void();
        return 1;
    }
    Type *vt = check_expr(n->as.call.args[0]);
    Type *vec = peel_ref(vt);
    if (!vec || vec->kind != TY_VEC) {
        err(n->loc, "pop requires []T, got %s", type_name(vt));
        n->ty = ty_void();
        return 1;
    }
    if (!(vt->kind == TY_PTR && vt->is_mut))
        match_arg(&n->as.call.args[0], type_ptr(vec, 1), vt);
    n->as.call.is_vec_pop = 1;
    n->ty = vec->elem;
    n->place_mut = 0;
    return 1;
}

/** `.children(a, b)` / `zeus.children(p, a, b)` → nested `.child` calls.
    `.children((a, b))` still unwraps a tuple. `.child` takes one node. */
static int is_mod_field(AstNode *cal) {
    return cal && cal->kind == AST_FIELD && !cal->as.access.via_colon &&
           cal->as.access.target && cal->as.access.target->kind == AST_IDENT &&
           (module_imported(Gmods[Gcur].ast, cal->as.access.target->as.ident.name) ||
            strcmp(cal->as.access.target->as.ident.name, "Box") == 0);
}

static int reject_multi_child(AstNode *n) {
    AstNode *cal = n->as.call.callee;
    if (!cal || cal->kind != AST_FIELD || cal->as.access.via_colon) return 0;
    if (!cal->as.access.field || strcmp(cal->as.access.field, "child") != 0) return 0;
    if (!cal->as.access.target) return 0;
    int mod = is_mod_field(cal);
    int many = 0;
    if (!mod && n->as.call.arg_count >= 2) many = 1;
    if (mod && n->as.call.arg_count >= 3) many = 1;
    if (n->as.call.arg_count == 1 && n->as.call.args[0] &&
        n->as.call.args[0]->kind == AST_TUPLE && n->as.call.args[0]->as.array_lit.count > 1)
        many = 1;
    if (mod && n->as.call.arg_count == 2 && n->as.call.args[1] &&
        n->as.call.args[1]->kind == AST_TUPLE && n->as.call.args[1]->as.array_lit.count > 1)
        many = 1;
    if (!many) return 0;
    err(n->loc, "'.child' takes one node; use '.children(...)' for several");
    return 1;
}

static int expand_children(AstNode *n) {
    AstNode *cal = n->as.call.callee;
    if (!cal || cal->kind != AST_FIELD || cal->as.access.via_colon) return 0;
    if (!cal->as.access.field || strcmp(cal->as.access.field, "children") != 0) return 0;
    if (!cal->as.access.target) return 0;
    int mod = is_mod_field(cal);
    if (n->as.call.arg_count == 0 || (mod && n->as.call.arg_count == 1)) {
        err(n->loc, "'.children' needs at least one node");
        return 0;
    }
    if (n->as.call.arg_count == 1 && n->as.call.args[0] &&
        n->as.call.args[0]->kind == AST_TUPLE) {
        AstNode *tup = n->as.call.args[0];
        size_t nc = tup->as.array_lit.count;
        AstNode **els = tup->as.array_lit.elems;
        if (nc == 0) return 0;
        if (nc == 1) {
            n->as.call.args[0] = els[0];
            tup->as.array_lit.elems = NULL;
            tup->as.array_lit.count = 0;
            ast_free(tup);
            cal->as.access.field = loam_dup("child");
            return 1;
        }
        AstNode *inner = cal->as.access.target;
        for (size_t i = 0; i + 1 < nc; i++) {
            AstNode *fld = ast_field(inner, loam_dup("child"), 0, n->loc);
            AstNode **a = (AstNode **)malloc(sizeof(AstNode *));
            a[0] = els[i];
            inner = ast_call(fld, a, 1, n->loc);
        }
        cal->as.access.target = inner;
        n->as.call.args[0] = els[nc - 1];
        cal->as.access.field = loam_dup("child");
        tup->as.array_lit.elems = NULL;
        tup->as.array_lit.count = 0;
        ast_free(tup);
        return 1;
    }
    if (mod && n->as.call.arg_count == 2 && n->as.call.args[1] &&
        n->as.call.args[1]->kind == AST_TUPLE) {
        AstNode *tup = n->as.call.args[1];
        size_t nc = tup->as.array_lit.count;
        AstNode **els = tup->as.array_lit.elems;
        if (nc == 0) return 0;
        const char *modn = cal->as.access.target->as.ident.name;
        AstNode *acc = n->as.call.args[0];
        for (size_t i = 0; i + 1 < nc; i++) {
            AstNode *fld = ast_field(ast_ident(loam_dup(modn), cal->as.access.target->loc),
                                    loam_dup("child"), 0, n->loc);
            AstNode **a = (AstNode **)malloc(2 * sizeof(AstNode *));
            a[0] = acc;
            a[1] = els[i];
            acc = ast_call(fld, a, 2, n->loc);
        }
        n->as.call.args[0] = acc;
        n->as.call.args[1] = els[nc - 1];
        cal->as.access.field = loam_dup("child");
        tup->as.array_lit.elems = NULL;
        tup->as.array_lit.count = 0;
        ast_free(tup);
        return 1;
    }
    if (!mod && n->as.call.arg_count == 1) {
        cal->as.access.field = loam_dup("child");
        return 1;
    }
    if (!mod && n->as.call.arg_count >= 2) {
        AstNode **els = n->as.call.args;
        size_t nc = n->as.call.arg_count;
        AstNode *inner = cal->as.access.target;
        for (size_t i = 0; i + 1 < nc; i++) {
            AstNode *fld = ast_field(inner, loam_dup("child"), 0, n->loc);
            AstNode **a = (AstNode **)malloc(sizeof(AstNode *));
            a[0] = els[i];
            inner = ast_call(fld, a, 1, n->loc);
        }
        cal->as.access.target = inner;
        n->as.call.args = (AstNode **)malloc(sizeof(AstNode *));
        n->as.call.args[0] = els[nc - 1];
        n->as.call.arg_count = 1;
        cal->as.access.field = loam_dup("child");
        free(els);
        return 1;
    }
    if (mod && n->as.call.arg_count == 2) {
        cal->as.access.field = loam_dup("child");
        return 1;
    }
    if (mod && n->as.call.arg_count >= 3) {
        AstNode **els = n->as.call.args;
        size_t nc = n->as.call.arg_count;
        const char *modn = cal->as.access.target->as.ident.name;
        AstNode *acc = els[0];
        for (size_t i = 1; i + 1 < nc; i++) {
            AstNode *fld = ast_field(ast_ident(loam_dup(modn), cal->as.access.target->loc),
                                    loam_dup("child"), 0, n->loc);
            AstNode **a = (AstNode **)malloc(2 * sizeof(AstNode *));
            a[0] = acc;
            a[1] = els[i];
            acc = ast_call(fld, a, 2, n->loc);
        }
        n->as.call.args = (AstNode **)malloc(2 * sizeof(AstNode *));
        n->as.call.args[0] = acc;
        n->as.call.args[1] = els[nc - 1];
        n->as.call.arg_count = 2;
        cal->as.access.field = loam_dup("child");
        free(els);
        return 1;
    }
    return 0;
}

/** `BoxStyle(width = 300)` / `zeus.GridConfig(columns = 3)` — a struct
 *  literal, not a call. Rewrites `n` in place and returns 1. */
static int as_struct_ctor(AstNode *n) {
    AstNode *cal = n->as.call.callee;
    const char *sname = NULL;
    if (cal && cal->kind == AST_IDENT) {
        if (lookup_unqualified(cal->as.ident.name, find_fn_in)) return 0;
        Type *lt = NULL;
        if (scope_find(cal->as.ident.name, &lt, NULL)) return 0;
        if (find_struct_visible(cal->as.ident.name)) sname = cal->as.ident.name;
    } else if (cal && cal->kind == AST_FIELD && !cal->as.access.via_colon &&
               cal->as.access.target && cal->as.access.target->kind == AST_IDENT &&
               module_imported(Gmods[Gcur].ast, cal->as.access.target->as.ident.name)) {
        LoamModule *m = find_mod(cal->as.access.target->as.ident.name);
        if (m && find_fn_in(m, cal->as.access.field)) return 0;
        if (m && find_struct_in(m, cal->as.access.field)) sname = cal->as.access.field;
    }
    if (!sname) return 0;
    FieldInit *fi = NULL;
    size_t fc = 0;
    if (n->as.call.arg_count == 1 && is_props_lit(n->as.call.args[0])) {
        AstNode *lit = n->as.call.args[0];
        fi = lit->as.struct_lit.fields;
        fc = lit->as.struct_lit.field_count;
    } else if (n->as.call.arg_count != 0) {
        err(n->loc, "'%s' is a struct — construct it with named fields", sname);
        return 0;
    }
    SourceLoc loc = n->loc;
    char *nm = loam_dup(sname);
    free(n->as.call.args);
    n->kind = AST_STRUCT_LIT;
    n->as.struct_lit.type_name = nm;
    n->as.struct_lit.fields = fi;
    n->as.struct_lit.field_count = fc;
    n->loc = loc;
    return 1;
}

/** Dispatch: Box::new, wrapping_add, fmt.println, method rewrite, mod.fn. */
static Type *check_call(AstNode *n, Type *expect) {
    /* `await e` sugar desugars to `async.await_value(e)`; JS semantics: it is
       only legal lexically inside an `async fn` body (closures reset it). */
    if (n->flags & ASTF_AWAIT) {
        if (!cur_async) err(n->loc, "await is only allowed inside an `async fn`");
    }
    if (as_struct_ctor(n)) return check_expr_ty(n, expect);
    if (reject_multi_child(n)) return ty_void();
    if (expand_children(n)) return check_call(n, expect);
    AstNode *cal = n->as.call.callee;

    /* Box::new(expr) */
    if (cal && cal->kind == AST_FIELD && cal->as.access.via_colon &&
        cal->as.access.target && cal->as.access.target->kind == AST_IDENT &&
        strcmp(cal->as.access.target->as.ident.name, "Box") == 0 &&
        cal->as.access.field && strcmp(cal->as.access.field, "new") == 0) {
        if (n->as.call.arg_count != 1) {
            err(n->loc, "Box::new expects 1 argument");
            return ty_void();
        }
        Type *et = check_expr(n->as.call.args[0]);
        n->as.call.is_box_new = 1;
        n->ty = type_box(et);
        n->place_mut = 0;
        return n->ty;
    }

    /* wrapping_add / saturating_add / wrapping bit ops / string_from_bytes */
    if (cal && cal->kind == AST_IDENT) {
        const char *nm = cal->as.ident.name;
        /* `__sizeof(x)` — byte size of x's type, a compile-time constant. Used
           by the Phase 11 arena-size benchmark; x is not evaluated. */
        if (strcmp(nm, "__sizeof") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__sizeof expects 1 argument");
                return ty_void();
            }
            Type *at = check_expr(n->as.call.args[0]);
            (void)at;
            n->as.call.is_sizeof = 1;
            n->ty = ty_int();
            return n->ty;
        }
        /* `panic(msg)` — trap with `msg` at the call site. The one primitive
           `std:test` builds its assertions on. */
        if (strcmp(nm, "panic") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "panic expects 1 argument");
                return ty_void();
            }
            Type *at = check_expr(n->as.call.args[0]);
            if (!at || at->kind != TY_STRING)
                err(n->as.call.args[0]->loc, "panic expects a string");
            n->as.call.is_panic = 1;
            n->ty = ty_void();
            return n->ty;
        }
        /* `"a {{x}} b"` arrives as __interp(parts...). Fold it here into
           loam_str_of_* / loam_str_concat builtin calls, so neither backend
           needs to know interpolation existed. */
        if (strcmp(nm, "__interp") == 0) {
            size_t np = n->as.call.arg_count;
            AstNode *acc = NULL;
            for (size_t i = 0; i < np; i++) {
                AstNode *part = n->as.call.args[i];
                Type *t = check_expr(part);
                if (!t || (t->kind != TY_STRING && t->kind != TY_INT &&
                           t->kind != TY_FLOAT && t->kind != TY_BOOL)) {
                    err(part->loc, "cannot interpolate %s", type_name(t));
                    n->ty = ty_string();
                    return n->ty;
                }
                if (t->kind != TY_STRING) {
                    AstNode **a = (AstNode **)malloc(sizeof(AstNode *));
                    a[0] = part;
                    AstNode *w = ast_call(NULL, a, 1, part->loc);
                    w->as.call.c_builtin = t->kind == TY_INT     ? "loam_str_of_int"
                                           : t->kind == TY_FLOAT ? "loam_str_of_float"
                                                                 : "loam_str_of_bool";
                    w->ty = ty_string();
                    part = w;
                }
                if (!acc) {
                    acc = part;
                    continue;
                }
                AstNode **a = (AstNode **)malloc(2 * sizeof(AstNode *));
                a[0] = acc;
                a[1] = part;
                AstNode *cat = ast_call(NULL, a, 2, n->loc);
                cat->as.call.c_builtin = "loam_str_concat";
                cat->ty = ty_string();
                acc = cat;
            }
            if (!acc) {
                acc = ast_string(loam_dup(""), n->loc);
                acc->ty = ty_string();
            }
            free(n->as.call.args);
            ast_free(cal);
            *n = *acc;
            return n->ty;
        }
        if (strcmp(nm, "__sig_push") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__sig_push expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            n->as.call.sig_cell = 1;
            n->ty = ty_int();
            n->place_mut = 0;
            (void)a;
            return n->ty;
        }
        if (strcmp(nm, "__sig_load") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__sig_load expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__sig_load requires int id");
            if (!expect || expect->kind == TY_VOID) {
                err(n->loc, "cannot infer type of __sig_load");
                n->ty = ty_void();
                return n->ty;
            }
            n->as.call.sig_cell = 2;
            n->ty = expect;
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__sig_store") == 0) {
            if (n->as.call.arg_count != 2) {
                err(n->loc, "__sig_store expects 2 arguments");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            check_expr(n->as.call.args[1]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__sig_store requires int id");
            n->as.call.sig_cell = 3;
            n->ty = ty_void();
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__fut_push") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__fut_push expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            n->as.call.fut_cell = 1;
            n->ty = ty_int();
            n->place_mut = 0;
            (void)a;
            return n->ty;
        }
        if (strcmp(nm, "__fut_load") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__fut_load expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__fut_load requires int id");
            if (!expect || expect->kind == TY_VOID) {
                err(n->loc, "cannot infer type of __fut_load");
                n->ty = ty_void();
                return n->ty;
            }
            n->as.call.fut_cell = 2;
            n->ty = expect;
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__fut_store") == 0) {
            if (n->as.call.arg_count != 2) {
                err(n->loc, "__fut_store expects 2 arguments");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            check_expr(n->as.call.args[1]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__fut_store requires int id");
            n->as.call.fut_cell = 3;
            n->ty = ty_void();
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__fut_ready") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__fut_ready expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__fut_ready requires int id");
            n->as.call.fut_cell = 4;
            n->ty = ty_int();
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__fut_clear") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__fut_clear expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__fut_clear requires int id");
            n->as.call.fut_cell = 5;
            n->ty = ty_void();
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__ch_new") == 0) {
            if (n->as.call.arg_count != 2) {
                err(n->loc, "__ch_new expects 2 arguments");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            check_expr(n->as.call.args[1]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__ch_new requires int cap");
            n->as.call.ch_cell = 1;
            n->ty = ty_int();
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__ch_send") == 0) {
            if (n->as.call.arg_count != 2) {
                err(n->loc, "__ch_send expects 2 arguments");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            check_expr(n->as.call.args[1]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__ch_send requires int id");
            n->as.call.ch_cell = 2;
            n->ty = ty_void();
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__ch_try_send") == 0) {
            if (n->as.call.arg_count != 2) {
                err(n->loc, "__ch_try_send expects 2 arguments");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            check_expr(n->as.call.args[1]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__ch_try_send requires int id");
            n->as.call.ch_cell = 3;
            n->ty = ty_int();
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__ch_recv") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__ch_recv expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__ch_recv requires int id");
            if (!expect || expect->kind == TY_VOID) {
                err(n->loc, "cannot infer type of __ch_recv");
                n->ty = ty_void();
                return n->ty;
            }
            n->as.call.ch_cell = 4;
            n->ty = expect;
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__ch_pop") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__ch_pop expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__ch_pop requires int id");
            if (!expect || expect->kind == TY_VOID) {
                err(n->loc, "cannot infer type of __ch_pop");
                n->ty = ty_void();
                return n->ty;
            }
            n->as.call.ch_cell = 5;
            n->ty = expect;
            n->place_mut = 0;
            return n->ty;
        }
        if (strcmp(nm, "__ch_ready") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "__ch_ready expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            if (!type_eq(a, ty_int()))
                err(n->as.call.args[0]->loc, "__ch_ready requires int id");
            n->as.call.ch_cell = 6;
            n->ty = ty_int();
            n->place_mut = 0;
            return n->ty;
        }
        /* Numeric conversions: `i64(x)`, `f32(x)`, `u8(x)`, and the wrap /
           saturate opt-outs `wrapping_u8(x)` / `saturating_u8(x)` (§3.4). */
        {
            Type *ct = primitive_type(nm);
            int mode = 0;
            if (!ct) {
                const char *sfx = NULL;
                if (strncmp(nm, "wrapping_", 9) == 0) {
                    mode = 1;
                    sfx = nm + 9;
                } else if (strncmp(nm, "saturating_", 11) == 0) {
                    mode = 2;
                    sfx = nm + 11;
                }
                if (mode) {
                    Type *wt = primitive_type(sfx);
                    if (wt && wt->kind == TY_INT) ct = wt;
                }
            }
            if (ct && type_is_numeric(ct)) {
                if (n->as.call.arg_count != 1) {
                    err(n->loc, "%s(x) expects 1 argument", nm);
                    return ty_void();
                }
                AstNode *arg = n->as.call.args[0];
                Type *at = check_expr(arg);
                if (!type_is_numeric(at) && at->kind != TY_BOOL)
                    err(arg->loc, "cannot convert %s to %s", type_name(at), type_name(ct));
                if (mode && !type_is_int_kind(at))
                    err(arg->loc, "%s requires an integer argument", nm);
                /* Rewrite in place into a cast so both backends see one node. */
                free(n->as.call.args);
                if (cal) ast_free(cal);
                n->kind = AST_CAST;
                n->as.cast.expr = arg;
                n->as.cast.type = ast_type(loam_dup(type_name(ct)), 2, NULL, 0, n->loc);
                n->as.cast.conv_mode = mode;
                n->ty = ct;
                n->place_mut = 0;
                return n->ty;
            }
        }
        /* wrapping_* / saturating_*: the width comes from the operands. */
        {
            NumericBuiltin nb = NUMB_NONE;
            int arity = 2;
            if (strcmp(nm, "wrapping_add") == 0) nb = NUMB_WRAP_ADD;
            else if (strcmp(nm, "wrapping_sub") == 0) nb = NUMB_WRAP_SUB;
            else if (strcmp(nm, "wrapping_mul") == 0) nb = NUMB_WRAP_MUL;
            else if (strcmp(nm, "wrapping_neg") == 0) { nb = NUMB_WRAP_NEG; arity = 1; }
            else if (strcmp(nm, "wrapping_shl") == 0) nb = NUMB_WRAP_SHL;
            else if (strcmp(nm, "wrapping_shr") == 0) nb = NUMB_WRAP_SHR;
            else if (strcmp(nm, "wrapping_and") == 0) nb = NUMB_WRAP_AND;
            else if (strcmp(nm, "wrapping_or") == 0) nb = NUMB_WRAP_OR;
            else if (strcmp(nm, "wrapping_xor") == 0) nb = NUMB_WRAP_XOR;
            else if (strcmp(nm, "saturating_add") == 0) nb = NUMB_SAT_ADD;
            else if (strcmp(nm, "saturating_sub") == 0) nb = NUMB_SAT_SUB;
            else if (strcmp(nm, "saturating_mul") == 0) nb = NUMB_SAT_MUL;
            if (nb != NUMB_NONE) {
                if (n->as.call.arg_count != (size_t)arity) {
                    err(n->loc, "%s expects %d argument(s)", nm, arity);
                    return ty_void();
                }
                Type *a = check_expr(n->as.call.args[0]);
                Type *b = arity == 2
                    ? check_expr_ty(n->as.call.args[1], type_is_int_kind(a) ? a : NULL)
                    : NULL;
                if (!type_is_int_kind(a) || (arity == 2 && !type_is_int_kind(b)))
                    err(n->loc, "%s requires int arguments", nm);
                else if (arity == 2 && !type_eq(a, b))
                    err(n->loc, "%s requires matching int widths (%s, %s)", nm, type_name(a),
                        type_name(b));
                n->as.call.num_builtin = nb;
                n->ty = type_is_int_kind(a) ? a : ty_int();
                return n->ty;
            }
        }
        if (strcmp(nm, "string_from_bytes") == 0) {
            if (n->as.call.arg_count != 1) {
                err(n->loc, "string_from_bytes expects 1 argument");
                return ty_void();
            }
            Type *a = check_expr(n->as.call.args[0]);
            if (!a || a->kind != TY_VEC || !type_eq(a->elem, ty_int()))
                err(n->loc, "string_from_bytes requires []int");
            n->as.call.c_builtin = "loam_string_from_bytes";
            n->ty = ty_string();
            return n->ty;
        }
    }

    if (try_vec_builtin(n)) return n->ty;

    /* fmt.println(...) */
    if (cal && cal->kind == AST_FIELD && !cal->as.access.via_colon &&
        cal->as.access.target && cal->as.access.target->kind == AST_IDENT &&
        strcmp(cal->as.access.target->as.ident.name, "fmt") == 0 &&
        cal->as.access.field && strcmp(cal->as.access.field, "println") == 0) {
        if (!module_imported(Gmods[Gcur].ast, "fmt")) {
            err(n->loc, "unknown module 'fmt' — add `import \"std:fmt\"`");
            return ty_void();
        }
        for (size_t i = 0; i < n->as.call.arg_count; i++) {
            Type *t = check_expr(n->as.call.args[i]);
            if (!is_printable(t))
                err(n->as.call.args[i]->loc, "fmt.println cannot print %s", type_name(t));
        }
        n->as.call.is_println = 1;
        n->ty = ty_void();
        mark_module_ident(cal->as.access.target, "fmt");
        {
            AstNode *prn = find_fn_in(find_mod("fmt"), "println");
            cal->as.access.resolved = prn;
            if (prn) cal->ty = fn_type_of(prn);
        }
        return n->ty;
    }

    /* UI chaining: node.w(32) → zeus.w(node, 32). Signals stay variables. */
    AstNode *fn = NULL;
    int is_mod_path = cal && cal->kind == AST_FIELD && !cal->as.access.via_colon &&
                      cal->as.access.target && cal->as.access.target->kind == AST_IDENT &&
                      (module_imported(Gmods[Gcur].ast, cal->as.access.target->as.ident.name) ||
                       strcmp(cal->as.access.target->as.ident.name, "Box") == 0);
    if (cal && cal->kind == AST_FIELD && !cal->as.access.via_colon && !is_mod_path) {
        const char *fnn = cal->as.access.field;
        Type *done = cal->as.access.resolved ? fn_type_of(cal->as.access.resolved) : NULL;
        if (done && n->as.call.arg_count == done->param_count) {
            fn = cal->as.access.resolved;
            cal->ty = done;
        } else {
            Type *recv_ty = cal->as.access.target ? check_expr(cal->as.access.target) : NULL;
            fn = find_method(fnn, recv_ty);
            if (!fn) {
                /* `h.f(x)` — field of type fn, not a method. */
                Type *base = peel_ref(recv_ty);
                if (base && base->kind == TY_STRUCT && fnn) {
                    for (size_t i = 0; i < base->field_count; i++) {
                        if (base->field_names[i] && strcmp(base->field_names[i], fnn) == 0 &&
                            base->field_types[i] && base->field_types[i]->kind == TY_PROC) {
                            cal->ty = base->field_types[i];
                            n->as.call.is_fn_val = 1;
                            return finish_proc_call(n, cal->ty, NULL);
                        }
                    }
                }
                err(n->loc, "no method '%s'", fnn);
                return ty_void();
            }
            cal->as.access.resolved = fn;
            cal->ty = fn_type_of(fn);
            AstNode *recv = cal->as.access.target;
            cal->as.access.target = NULL;
            size_t ac = n->as.call.arg_count;
            AstNode **na = (AstNode **)malloc((ac + 1) * sizeof(AstNode *));
            na[0] = recv;
            if (ac) memcpy(na + 1, n->as.call.args, ac * sizeof(AstNode *));
            free(n->as.call.args);
            n->as.call.args = na;
            n->as.call.arg_count = ac + 1;
            n->as.call.resolved_cname = fn->as.fn.cname;
        }
    } else if (cal && cal->kind == AST_FIELD && !cal->as.access.via_colon &&
        cal->as.access.target && cal->as.access.target->kind == AST_IDENT) {
        const char *mod = cal->as.access.target->as.ident.name;
        const char *fnn = cal->as.access.field;
        if (!module_imported(Gmods[Gcur].ast, mod)) {
            err(n->loc, "unknown module '%s' — add `import \"std:%s\"` or `import \"%s.loam\"`",
                mod, mod, mod);
            return ty_void();
        }
        if (strcmp(mod, "zeus") == 0 && fnn &&
            (strcmp(fnn, "get") == 0 || strcmp(fnn, "set") == 0)) {
            err(n->loc, "use sig.%s(...) instead of zeus.%s", fnn, fnn);
            return ty_void();
        }
        LoamModule *m = find_mod(mod);
        fn = m ? find_fn_in(m, fnn) : NULL;
        if (!fn) {
            err(n->loc, "no function '%s' in module '%s'", fnn, mod);
            return ty_void();
        }
        cal->as.access.resolved = fn;
        cal->ty = fn_type_of(fn);
        mark_module_ident(cal->as.access.target, mod);
    } else if (cal && cal->kind == AST_IDENT) {
        Type *lt = NULL;
        if (scope_find(cal->as.ident.name, &lt, NULL) && lt && lt->kind == TY_PROC) {
            /* A local fn value. Check it as an identifier expression so a
               closure that calls a captured `key(t)` records `key` as a
               capture — resolving the name here directly skipped that, and
               the closure body then referenced an undeclared C variable. */
            Type *ct = check_expr(cal);
            n->as.call.is_fn_val = 1;
            return finish_proc_call(n, ct, NULL);
        }
        fn = lookup_unqualified(cal->as.ident.name, find_fn_in);
        if (!fn) {
            err(n->loc, "unknown function '%s'", cal->as.ident.name);
            return ty_void();
        }
        cal->as.ident.resolved = fn;
        cal->as.ident.def_loc = fn->loc;
        cal->ty = fn_type_of(fn);
    } else if (cal) {
        Type *ct = check_expr(cal);
        if (ct && ct->kind == TY_PROC) {
            n->as.call.is_fn_val = 1;
            return finish_proc_call(n, ct, NULL);
        }
        err(n->loc, "invalid call");
        return ty_void();
    } else {
        err(n->loc, "invalid call");
        return ty_void();
    }

    Type *ft = fn_type_of(fn);
    return finish_proc_call(n, ft, fn);
}

/** Last expression in a block is the return value (`fn f() -> int { 1 }`,
    `|x| { x + 1 }`). `ret.expr` and `expr_stmt.expr` share the union layout. */
static int block_ends_in_value(AstNode *blk);

/** 1 if this `if` is a value: it has an `else` and every arm ends in an
 *  expression (an `else if` arm counts if the nested `if` does). */
static int if_is_value(AstNode *n) {
    if (!n || n->kind != AST_IF || !n->as.if_stmt.else_block) return 0;
    return block_ends_in_value(n->as.if_stmt.then_block) &&
           block_ends_in_value(n->as.if_stmt.else_block);
}

/** `else if` parses as a bare `if` statement. In value position, wrap it in a
 *  one-statement block so every arm of the chain is a block. Only called once
 *  the chain is known to be a value — a statement `else if` keeps its shape. */
static void wrap_else_if(AstNode *n) {
    if (!n || n->kind != AST_IF) return;
    AstNode *e = n->as.if_stmt.else_block;
    if (!e || e->kind != AST_IF) return;
    wrap_else_if(e);
    AstNode **st = (AstNode **)malloc(sizeof(AstNode *));
    if (!st) loam_fatal("out of memory");
    st[0] = ast_expr_stmt(e, e->loc);
    n->as.if_stmt.else_block = ast_block(st, 1, e->loc);
}

static int block_ends_in_value(AstNode *blk) {
    if (!blk) return 0;
    if (blk->kind == AST_IF) return if_is_value(blk);
    if (blk->kind != AST_BLOCK || !blk->as.block.stmt_count) return 0;
    AstNode *last = blk->as.block.stmts[blk->as.block.stmt_count - 1];
    if (!last) return 0;
    if (last->kind == AST_EXPR_STMT) {
        AstNode *e = last->as.expr_stmt.expr;
        if (e && e->kind == AST_IF) return if_is_value(e);
        return 1;
    }
    if (last->kind == AST_IF) return if_is_value(last);
    return 0;
}

static void tail_expr_to_return(AstNode *body) {
    if (!body || body->kind != AST_BLOCK || !body->as.block.stmt_count) return;
    size_t li = body->as.block.stmt_count - 1;
    AstNode *last = body->as.block.stmts[li];
    /* `fn f() -> string { if c { "a" } else { "b" } }` returns the if. */
    if (last->kind == AST_IF && if_is_value(last)) {
        wrap_else_if(last);
        body->as.block.stmts[li] = ast_return(last, last->loc);
        return;
    }
    if (last->kind != AST_EXPR_STMT) return;
    last->kind = AST_RETURN;
}

/** `on_mouse_down = fn(e: MouseEvent) => ...` where a `fn()` is expected.
 *  The engine invokes handlers with no arguments, so the payload is staged in
 *  module globals: drop the parameter and open the body with
 *  `let e = current_mouse()` (or `current_key()`). */
static void stage_event_param(AstNode *n, Type *expect) {
    if (!expect || expect->kind != TY_PROC || expect->param_count != 0) return;
    if (n->as.fn.param_count != 1) return;
    AstNode *pt = n->as.fn.params[0].type;
    if (!pt || pt->kind != AST_TYPE || pt->as.type.tag != 2 || !pt->as.type.name) return;
    const char *reader = NULL;
    if (strcmp(pt->as.type.name, "MouseEvent") == 0) reader = "current_mouse";
    else if (strcmp(pt->as.type.name, "KeyEvent") == 0) reader = "current_key";
    if (!reader) return;
    SourceLoc loc = n->as.fn.params[0].loc;
    AstNode *call = ast_call(ast_ident(loam_dup(reader), loc), NULL, 0, loc);
    AstNode *let = ast_var(loam_dup(n->as.fn.params[0].name), NULL, call, 0, loc);
    AstNode *body = n->as.fn.body;
    if (!body || body->kind != AST_BLOCK) return;
    size_t c = body->as.block.stmt_count;
    AstNode **st = (AstNode **)malloc((c + 1) * sizeof(AstNode *));
    if (!st) loam_fatal("out of memory");
    st[0] = let;
    if (c) memcpy(st + 1, body->as.block.stmts, c * sizeof(AstNode *));
    free(body->as.block.stmts);
    body->as.block.stmts = st;
    body->as.block.stmt_count = c + 1;
    ast_free(pt);
    free((void *)n->as.fn.params[0].name);
    free(n->as.fn.params);
    n->as.fn.params = NULL;
    n->as.fn.param_count = 0;
}

static Type *check_closure(AstNode *n, Type *expect) {
    stage_event_param(n, expect);
    size_t npc = n->as.fn.param_count;
    Type **ps = NULL;
    if (npc) ps = calloc(npc, sizeof(Type *));
    if (expect && expect->kind != TY_PROC) expect = NULL;
    if (expect && expect->param_count != npc)
        err(n->loc, "closure has %zu parameter(s), expected %zu", npc, expect->param_count);
    for (size_t i = 0; i < npc; i++) {
        if (n->as.fn.params[i].type) {
            ps[i] = resolve_type(n->as.fn.params[i].type);
            if (expect && i < expect->param_count && !type_eq(ps[i], expect->params[i]))
                err(n->as.fn.params[i].loc, "parameter '%s' has type %s, expected %s",
                    n->as.fn.params[i].name, type_name(ps[i]), type_name(expect->params[i]));
        } else if (expect && i < expect->param_count) {
            ps[i] = expect->params[i];
        } else {
            err(n->as.fn.params[i].loc, "cannot infer type of closure parameter '%s'",
                n->as.fn.params[i].name);
            ps[i] = ty_void();
        }
    }

    Type *ret = n->as.fn.ret_type ? resolve_type(n->as.fn.ret_type) : NULL;
    if (!ret && expect) ret = expect->ret ? expect->ret : ty_void();

    int saved_infer = clos_infer;
    Type *saved_infer_ty = clos_infer_ty;
    Type *saved_ret = cur_ret;
    AstNode *saved_clos = cur_clos;
    Scope *saved_cs = clos_scope;
    int saved_async = cur_async;
    cur_clos = n;
    cur_async = 0; /* closures cannot be async: `await` inside one is an error */
    scope_push();
    clos_scope = scope;
    if (clos_depth < 64) {
        clos_stack[clos_depth] = n;
        clos_scope_stack[clos_depth] = scope;
        clos_depth++;
    }
    for (size_t i = 0; i < npc; i++)
        scope_add(n->as.fn.params[i].name, ps[i], 0, 0, n->as.fn.params[i].loc, n);

    if (!n->as.fn.clos_id) n->as.fn.clos_id = next_clos_id++;

    if (!ret) {
        clos_infer = 1;
        clos_infer_ty = NULL;
        tail_expr_to_return(n->as.fn.body);
        n->ty = type_proc(ps, npc, ty_void());
        cur_ret = ty_void();
        check_block_in_scope(n->as.fn.body);
        n->ty->ret = clos_infer_ty ? clos_infer_ty : ty_void();
    } else {
        clos_infer = 0;
        n->ty = type_proc(ps, npc, ret);
        if (ret->kind != TY_VOID) tail_expr_to_return(n->as.fn.body);
        cur_ret = ret;
        check_block_in_scope(n->as.fn.body);
    }

    if (clos_depth > 0) clos_depth--;
    scope_pop();
    cur_ret = saved_ret;
    cur_clos = saved_clos;
    clos_scope = saved_cs;
    clos_infer = saved_infer;
    clos_infer_ty = saved_infer_ty;
    cur_async = saved_async;
    n->place_mut = 0;
    return n->ty;
}

/** Check both operands of a binary expression, letting an untyped numeric
 *  literal adopt the other operand's (or the expected) width (§3.4). */
static void check_bin_ops(AstNode *n, Type *expect, Type **out_l, Type **out_r) {
    AstNode *ln = n->as.binary.left, *rn = n->as.binary.right;
    Type *en = (expect && type_is_numeric(expect)) ? expect : NULL;
    if (en) {
        *out_l = check_expr_ty(ln, en);
        *out_r = check_expr_ty(rn, en);
        return;
    }
    int ll = is_num_lit(ln), rl = is_num_lit(rn);
    if (ll && rl) {
        if (ln->kind == AST_FLOAT || rn->kind == AST_FLOAT) {
            Type *want = ty_float();
            *out_l = check_expr_ty(ln, want);
            *out_r = check_expr_ty(rn, want);
        } else {
            *out_l = check_expr_ty(ln, NULL);
            *out_r = check_expr_ty(rn, type_is_int_kind(*out_l) ? *out_l : NULL);
        }
        return;
    }
    if (ll) {
        Type *r = check_expr(rn);
        *out_r = r;
        *out_l = check_expr_ty(ln, type_is_numeric(r) ? r : NULL);
        return;
    }
    if (rl) {
        Type *l = check_expr(ln);
        *out_l = l;
        *out_r = check_expr_ty(rn, type_is_numeric(l) ? l : NULL);
        return;
    }
    *out_l = check_expr(ln);
    *out_r = check_expr(rn);
}

/** Infer/check an expression; sets n->ty. May rewrite args for auto-borrow. */
static Type *check_expr_ty(AstNode *n, Type *expect) {
    if (!n) return ty_void();
    switch (n->kind) {
        case AST_NUMBER:
            return lit_with_expect(n, expect);
        case AST_FLOAT:
            return lit_with_expect(n, expect);
        case AST_BOOL:
            n->ty = ty_bool();
            return n->ty;
        case AST_STRING:
            n->ty = ty_string();
            return n->ty;
        case AST_IDENT: {
            Type *ty = NULL;
            int mut = 0;
            SourceLoc dloc = {0};
            AstNode *dnode = NULL;
            Scope *found = scope_find_s(n->as.ident.name, &ty, &mut, NULL, &dloc, &dnode);
            if (found) {
                /* Module globals have static storage: copying one into an env
                   would produce a member reference on a void env. */
                int is_global = 0;
                for (int g = 0; g < ngvars; g++)
                    if (gvars[g].var == dnode) is_global = 1;
                if (!is_global) {
                    for (int c = clos_depth - 1; c >= 0; c--) {
                        if (scope_is_inside(found, clos_scope_stack[c])) break;
                        if (!type_is_copy(ty) || (ty->kind == TY_PTR && ty->is_mut)) {
                            err(n->loc, "cannot capture non-Copy '%s'", n->as.ident.name);
                            break;
                        }
                        add_cap(clos_stack[c], n->as.ident.name, cap_type_for(dnode, ty));
                    }
                }
                n->ty = ty;
                n->place_mut = mut;
                n->as.ident.resolved = dnode;
                n->as.ident.def_loc = dloc;
                return ty;
            }
            AstNode *fn = find_fn_in(&Gmods[Gcur], n->as.ident.name);
            if (fn) {
                if (fn->as.fn.tparam_count) {
                    err(n->loc, "cannot use generic function '%s' as a value", n->as.ident.name);
                    n->ty = ty_void();
                    return n->ty;
                }
                fn->as.fn.used_as_value = 1;
                n->flags |= ASTF_FN_VAL;
                n->ty = fn_type_of(fn);
                n->place_mut = 0;
                n->as.ident.resolved = fn;
                n->as.ident.def_loc = fn->loc;
                return n->ty;
            }
            err(n->loc, "unknown identifier '%s'", n->as.ident.name);
            n->ty = ty_void();
            return n->ty;
        }
        case AST_BINARY: {
            TokenKind op = n->as.binary.op;
            Type *l, *r;
            if (op == TOK_DOT_DOT) {
                l = check_expr(n->as.binary.left);
                r = check_expr_ty(n->as.binary.right, type_is_int_kind(l) ? l : NULL);
                if (!type_is_int_kind(l) || !type_is_int_kind(r))
                    err(n->loc, "range bounds must be int");
                n->ty = ty_int(); /* marker; for-loop consumes this */
                return n->ty;
            }
            if (op == TOK_AMP_AMP || op == TOK_PIPE_PIPE) {
                l = check_expr(n->as.binary.left);
                r = check_expr(n->as.binary.right);
                if (!type_eq(l, ty_bool()) || !type_eq(r, ty_bool()))
                    err(n->loc, "logical operators require bool");
                n->ty = ty_bool();
                return n->ty;
            }
            if (op == TOK_EQ_EQ || op == TOK_BANG_EQ || op == TOK_LT || op == TOK_GT ||
                op == TOK_LT_EQ || op == TOK_GT_EQ) {
                check_bin_ops(n, NULL, &l, &r);
                if (!type_eq(l, r))
                    err(n->loc, "cannot compare %s with %s", type_name(l), type_name(r));
                if (op == TOK_LT || op == TOK_GT || op == TOK_LT_EQ || op == TOK_GT_EQ) {
                    if (!type_is_numeric(l))
                        err(n->loc, "ordering comparison requires int or float");
                } else if (l && l->kind != TY_INT && l->kind != TY_FLOAT && l->kind != TY_BOOL &&
                           l->kind != TY_STRING) {
                    err(n->loc, "cannot compare %s", type_name(l));
                }
                n->ty = ty_bool();
                return n->ty;
            }
            /* Bitwise & shifts: integer widths only, same width on both sides
               (§3.4). Shifts lower through the unsigned type (§3.5). */
            if (op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET) {
                check_bin_ops(n, expect, &l, &r);
                if (!type_is_int_kind(l) || !type_is_int_kind(r))
                    err(n->loc, "bitwise operator requires int operands");
                else if (!type_eq(l, r))
                    err(n->loc, "cannot mix %s and %s; convert explicitly (%s(x))",
                        type_name(l), type_name(r), type_name(l));
                n->ty = type_is_int_kind(l) ? l : ty_int();
                return n->ty;
            }
            if (op == TOK_SHL || op == TOK_SHR) {
                check_bin_ops(n, expect, &l, &r);
                if (!type_is_int_kind(l) || !type_is_int_kind(r)) {
                    err(n->loc, "shift requires int operands");
                } else {
                    AstNode *amt = n->as.binary.right;
                    /* The right operand was checked with the left's type when it
                       is an untyped literal; read the literal value directly. */
                    if (amt->kind == AST_NUMBER &&
                        (amt->as.lit.value < 0 || amt->as.lit.value >= (int64_t)l->bits))
                        err(amt->loc, "shift by %lld is out of range for %s (width %d)",
                            (long long)amt->as.lit.value, type_name(l), (int)l->bits);
                }
                n->ty = type_is_int_kind(l) ? l : ty_int();
                return n->ty;
            }
            check_bin_ops(n, expect, &l, &r);
            if (type_is_float_kind(l) && type_is_float_kind(r)) {
                if (!type_eq(l, r))
                    err(n->loc, "cannot mix %s and %s; convert explicitly (%s(x))",
                        type_name(l), type_name(r), type_name(l->bits >= r->bits ? l : r));
                if (op == TOK_PERCENT)
                    err(n->loc, "%% is not defined for float");
                n->ty = l;
                return n->ty;
            }
            if (type_is_int_kind(l) && type_is_int_kind(r)) {
                if (!type_eq(l, r))
                    err(n->loc, "cannot mix %s and %s; convert explicitly (%s(x))",
                        type_name(l), type_name(r), type_name(l->bits >= r->bits ? l : r));
                n->ty = l;
                return n->ty;
            }
            if (type_is_numeric(l) && type_is_numeric(r))
                err(n->loc, "cannot mix %s and %s; convert explicitly", type_name(l), type_name(r));
            else
                err(n->loc, "arithmetic requires int or float");
            n->ty = l;
            return n->ty;
        }
        case AST_UNARY: {
            /* A negated untyped literal is still a literal: give it the
               expected width so `-128` can be the i8 minimum. */
            if (n->as.unary.op == TOK_MINUS && is_num_lit(n->as.unary.operand) &&
                expect && type_is_numeric(expect)) {
                AstNode *op0 = n->as.unary.operand;
                if (op0->kind == AST_NUMBER) {
                    int64_t v = -op0->as.lit.value;
                    if (expect->kind == TY_FLOAT) {
                        n->kind = AST_FLOAT;
                        n->as.lit.f = -(double)op0->as.lit.value;
                    } else {
                        if (!int_lit_fits(v, expect))
                            err(n->loc, "literal %lld does not fit %s", (long long)v,
                                type_name(expect));
                        n->kind = AST_NUMBER;
                        n->as.lit.value = v;
                    }
                    n->ty = expect;
                    n->place_mut = 0;
                    return n->ty;
                }
                if (op0->kind == AST_FLOAT && expect->kind == TY_FLOAT) {
                    double fv = -op0->as.lit.f;
                    n->kind = AST_FLOAT;
                    n->as.lit.f = fv;
                    n->ty = expect;
                    n->place_mut = 0;
                    return n->ty;
                }
            }
            Type *o = check_expr(n->as.unary.operand);
            if (n->as.unary.op == TOK_BANG) {
                if (!type_eq(o, ty_bool())) err(n->loc, "! requires bool");
                n->ty = ty_bool();
            } else if (n->as.unary.op == TOK_TILDE) {
                if (!type_is_int_kind(o)) err(n->loc, "~ requires int");
                n->ty = o ? o : ty_int();
            } else {
                if (!type_is_numeric(o))
                    err(n->loc, "unary minus requires int or float");
                else if (o->kind == TY_INT && o->is_unsigned)
                    err(n->loc, "cannot negate an unsigned value (use a signed width)");
                n->ty = o;
            }
            return n->ty;
        }
        case AST_INCDEC: {
            Type *o = check_expr(n->as.incdec.operand);
            if (!n->as.incdec.operand->place_mut)
                err(n->loc, n->as.incdec.is_dec ? "cannot decrement immutable place"
                                                : "cannot increment immutable place");
            if (!type_is_numeric(o))
                err(n->loc, n->as.incdec.is_dec ? "-- requires int or float"
                                                : "++ requires int or float");
            n->ty = o;
            return n->ty;
        }
        case AST_CAST: {
            Type *from = check_expr(n->as.cast.expr);
            Type *to = resolve_type(n->as.cast.type);
            int ok = 0;
            if (from && to) {
                if (type_is_numeric(from) && type_is_numeric(to)) ok = 1;
                if (from->kind == TY_BOOL && to->kind == TY_INT) ok = 1;
                if (type_is_int_kind(from) && to->kind == TY_BOOL) ok = 1;
                if (type_eq(from, to)) ok = 1;
            }
            if (!ok)
                err(n->loc, "cannot cast %s as %s", type_name(from), type_name(to));
            n->ty = to;
            n->place_mut = 0;
            return n->ty;
        }
        case AST_ADDR: {
            Type *t = check_expr(n->as.access.target);
            if (n->as.access.is_mut && !n->as.access.target->place_mut)
                err(n->loc, "cannot take &mut of immutable place");
            n->ty = type_ptr(t, n->as.access.is_mut);
            n->place_mut = 0;
            return n->ty;
        }
        case AST_DEREF: {
            Type *t = check_expr(n->as.access.target);
            if (t->kind != TY_PTR && t->kind != TY_BOX) {
                err(n->loc, "cannot dereference %s", type_name(t));
                n->ty = ty_void();
                return n->ty;
            }
            n->ty = t->elem;
            if (t->kind == TY_PTR) n->place_mut = t->is_mut;
            else n->place_mut = n->as.access.target->place_mut;
            return n->ty;
        }
        case AST_FIELD: {
            /* `ALIGN.Center` / `zeus.ALIGN.Center` — an enum constant is an
               int literal from here on; no backend sees the enum. */
            if (n->as.access.target && n->as.access.target->kind == AST_IDENT &&
                n->as.access.field) {
                AstNode *en = find_enum(n->as.access.target->as.ident.name);
                if (en) {
                    int64_t v = 0;
                    if (!enum_value(en, n->as.access.field, &v)) {
                        err(n->loc, "enum '%s' has no variant '%s'", en->as.enm.name,
                            n->as.access.field);
                        n->ty = ty_int();
                        return n->ty;
                    }
                    return lower_to_int(n, v);
                }
            }
            if (n->as.access.target && n->as.access.target->kind == AST_FIELD &&
                !n->as.access.target->as.access.via_colon && n->as.access.field &&
                n->as.access.target->as.access.target &&
                n->as.access.target->as.access.target->kind == AST_IDENT &&
                module_imported(Gmods[Gcur].ast,
                                n->as.access.target->as.access.target->as.ident.name)) {
                LoamModule *em = find_mod(n->as.access.target->as.access.target->as.ident.name);
                AstNode *en = em ? find_enum_in(em, n->as.access.target->as.access.field) : NULL;
                if (en) {
                    int64_t v = 0;
                    if (!enum_value(en, n->as.access.field, &v)) {
                        err(n->loc, "enum '%s' has no variant '%s'", en->as.enm.name,
                            n->as.access.field);
                        n->ty = ty_int();
                        return n->ty;
                    }
                    return lower_to_int(n, v);
                }
            }
            /* module.fn handled in check_call; `mod.global` is a place. */
            if (n->as.access.target && n->as.access.target->kind == AST_IDENT) {
                const char *base = n->as.access.target->as.ident.name;
                if (module_imported(Gmods[Gcur].ast, base) || strcmp(base, "Box") == 0) {
                    mark_module_ident(n->as.access.target, base);
                    LoamModule *mod = find_mod(base);
                    AstNode *gv = mod ? find_global_in(mod, n->as.access.field) : NULL;
                    if (gv && gv->ty) {
                        n->as.access.resolved = gv;
                        n->ty = gv->ty;
                        n->place_mut = gv->as.var.is_mut;
                        return n->ty;
                    }
                    /* path used as value — only valid as call callee */
                    n->ty = ty_void();
                    return n->ty;
                }
            }
            Type *t = check_expr(n->as.access.target);
            Type *base = peel_ref(t);
            if (base && (base->kind == TY_VEC || base->kind == TY_ARRAY ||
                         base->kind == TY_STRING) && n->as.access.field) {
                if (strcmp(n->as.access.field, "len") == 0 ||
                    (base->kind == TY_VEC && strcmp(n->as.access.field, "cap") == 0)) {
                    n->ty = ty_int();
                    n->place_mut = 0;
                    return n->ty;
                }
            }
            if (!base || base->kind != TY_STRUCT) {
                err(n->loc, "field access on non-struct %s", type_name(t));
                n->ty = ty_void();
                return n->ty;
            }
            for (size_t i = 0; i < base->field_count; i++) {
                if (strcmp(base->field_names[i], n->as.access.field) == 0) {
                    n->ty = base->field_types[i];
                    if (t->kind == TY_PTR) n->place_mut = t->is_mut;
                    else if (t->kind == TY_BOX) n->place_mut = n->as.access.target->place_mut;
                    else n->place_mut = n->as.access.target->place_mut;
                    return n->ty;
                }
            }
            err(n->loc, "no field '%s' on %s", n->as.access.field, type_name(base));
            n->ty = ty_void();
            return n->ty;
        }
        case AST_INDEX: {
            Type *t = check_expr(n->as.access.target);
            Type *ix = check_expr(n->as.access.index);
            if (!type_is_int_kind(ix)) err(n->as.access.index->loc, "index must be int");
            Type *base = peel_ref(t);
            if (base && base->kind == TY_STRING) {
                n->ty = ty_int();
                n->place_mut = 0;
                return n->ty;
            }
            if (!base || (base->kind != TY_ARRAY && base->kind != TY_VEC)) {
                err(n->loc, "cannot index %s", type_name(t));
                n->ty = ty_void();
                return n->ty;
            }
            n->ty = base->elem;
            n->place_mut = n->as.access.target->place_mut;
            if (t->kind == TY_PTR) n->place_mut = t->is_mut;
            return n->ty;
        }
        case AST_IF: {
            /* `if c { a } else { b }` as a value: both branches end in an
               expression of the same type, and `else` is required. */
            wrap_else_if(n);
            if (!type_eq(check_expr(n->as.if_stmt.cond), ty_bool()))
                err(n->as.if_stmt.cond->loc, "if condition must be bool");
            if (!n->as.if_stmt.else_block) {
                err(n->loc, "an `if` used as a value needs an `else` branch");
                n->ty = ty_void();
                return n->ty;
            }
            Type *a = check_block_value(n->as.if_stmt.then_block, expect);
            Type *b = check_block_value(n->as.if_stmt.else_block, expect);
            if (!type_eq(a, b)) {
                err(n->loc, "if branches have types %s and %s", type_name(a), type_name(b));
                n->ty = a;
                return n->ty;
            }
            n->ty = a;
            n->place_mut = 0;
            return n->ty;
        }
        case AST_CALL:
            return check_call(n, expect);
        case AST_CLOSURE:
            return check_closure(n, expect);
        case AST_STRUCT_LIT: {
            AstNode *st = find_struct(n->as.struct_lit.type_name);
            if (!st) {
                err(n->loc, "unknown struct '%s'", n->as.struct_lit.type_name);
                n->ty = ty_void();
                return n->ty;
            }
            Type *tmpl = struct_type_of(st);
            size_t nt = st->as.strct.tparam_count;
            Type **bound = NULL;
            if (nt) bound = calloc(nt, sizeof(Type *));
            if (nt && expect && expect->kind == TY_STRUCT && expect->name && tmpl->name &&
                strcmp(expect->name, tmpl->name) == 0 && expect->param_count == nt) {
                for (size_t i = 0; i < nt; i++) bound[i] = expect->params[i];
            }
            int *seen = calloc(tmpl->field_count, sizeof(int));
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++) {
                FieldInit *fi = &n->as.struct_lit.fields[i];
                int found = -1;
                for (size_t f = 0; f < tmpl->field_count; f++) {
                    if (strcmp(tmpl->field_names[f], fi->name) == 0) {
                        found = (int)f;
                        break;
                    }
                }
                if (found < 0) {
                    err(fi->init->loc, "unknown field '%s'", fi->name);
                    continue;
                }
                seen[found] = 1;
                Type *want = nt ? NULL : tmpl->field_types[found];
                Type *it = check_arg_ty(&fi->init, want);
                if (nt) {
                    if (!unify(tmpl->field_types[found], it, st->as.strct.tparams, bound, nt))
                        err(fi->init->loc, "cannot match %s to %s", type_name(it),
                            type_name(tmpl->field_types[found]));
                } else if (!type_eq(it, tmpl->field_types[found])) {
                    err(fi->init->loc, "field '%s' has type %s, got %s",
                        fi->name, type_name(tmpl->field_types[found]), type_name(it));
                }
            }
            /* A field the caller left out takes its declared default, resolved
               in the module that declares the struct — not the caller's. A
               `foo__set: bool` companion field records whether `foo` was
               passed, which is how an optional handler is told from its
               default (function values cannot be compared). */
            for (size_t f = 0; f < tmpl->field_count; f++) {
                if (seen[f]) continue;
                const char *fname = tmpl->field_names[f];
                size_t fl = fname ? strlen(fname) : 0;
                if (fl > 5 && strcmp(fname + fl - 5, "__set") == 0) {
                    /* `seen` is the caller's fields only — defaults appended
                       to the literal above must not count as "passed". */
                    int was_set = 0;
                    for (size_t g = 0; g < tmpl->field_count; g++) {
                        const char *gn = tmpl->field_names[g];
                        if (gn && strlen(gn) == fl - 5 && strncmp(gn, fname, fl - 5) == 0)
                            was_set = seen[g];
                    }
                    if (was_set) {
                        AstNode *b = ast_bool(1, n->loc);
                        b->ty = ty_bool();
                        n->as.struct_lit.fields = (FieldInit *)realloc(
                            n->as.struct_lit.fields,
                            (n->as.struct_lit.field_count + 1) * sizeof(FieldInit));
                        if (!n->as.struct_lit.fields) loam_fatal("out of memory");
                        n->as.struct_lit.fields[n->as.struct_lit.field_count].name =
                            loam_dup(fname);
                        n->as.struct_lit.fields[n->as.struct_lit.field_count].init = b;
                        n->as.struct_lit.field_count++;
                        continue;
                    }
                }
                AstNode *def = f < st->as.strct.field_count ? st->as.strct.fields[f].def : NULL;
                AstNode *cp = clone_const(def);
                if (!cp) {
                    if (def)
                        err(n->loc, "default for '%s' is not a constant expression", fname);
                    else
                        err(n->loc, "missing field '%s'", fname);
                    continue;
                }
                int save_cur = Gcur;
                const char *save_mod = cur_mod_name;
                Gcur = module_of_decl(st);
                cur_mod_name = Gmods[Gcur].name;
                Type *dt = check_arg_ty(&cp, tmpl->field_types[f]);
                Gcur = save_cur;
                cur_mod_name = save_mod;
                if (!nt && !type_eq(dt, tmpl->field_types[f]))
                    err(n->loc, "default for '%s' has type %s, expected %s", fname,
                        type_name(dt), type_name(tmpl->field_types[f]));
                n->as.struct_lit.fields = (FieldInit *)realloc(
                    n->as.struct_lit.fields,
                    (n->as.struct_lit.field_count + 1) * sizeof(FieldInit));
                if (!n->as.struct_lit.fields) loam_fatal("out of memory");
                n->as.struct_lit.fields[n->as.struct_lit.field_count].name = loam_dup(fname);
                n->as.struct_lit.fields[n->as.struct_lit.field_count].init = cp;
                n->as.struct_lit.field_count++;
            }
            free(seen);
            if (nt) {
                for (size_t i = 0; i < nt; i++) {
                    if (!bound[i]) {
                        err(n->loc, "cannot infer type parameter '%s'", st->as.strct.tparams[i]);
                        bound[i] = ty_void();
                    }
                }
                n->ty = make_struct_inst(st, bound, nt);
                free(bound);
            } else {
                n->ty = tmpl;
            }
            n->place_mut = 0;
            return n->ty;
        }
        case AST_ARRAY_LIT: {
            Type *et = resolve_type(n->as.array_lit.elem_type);
            if (n->as.array_lit.len >= 0 &&
                (int64_t)n->as.array_lit.count != n->as.array_lit.len)
                err(n->loc, "array literal has %zu elements, expected %lld",
                    n->as.array_lit.count, (long long)n->as.array_lit.len);
            for (size_t i = 0; i < n->as.array_lit.count; i++) {
                Type *e = check_expr_ty(n->as.array_lit.elems[i], et);
                if (!type_eq(e, et))
                    err(n->as.array_lit.elems[i]->loc, "array element type mismatch");
            }
            n->ty = n->as.array_lit.len < 0 ? type_vec(et) : type_array(et, n->as.array_lit.len);
            return n->ty;
        }
        case AST_TUPLE:
            err(n->loc, "tuple is not a value; use `.children(a, b, ...)`");
            n->ty = ty_void();
            return n->ty;
        default:
            n->ty = ty_void();
            return n->ty;
    }
}

static Type *check_expr(AstNode *n) { return check_expr_ty(n, NULL); }

static void check_stmt(AstNode *n);

/** Statements of `n` in the caller's scope — used for a function or closure
 *  body, so that a `let` shadowing a parameter is a same-scope collision. */
static void check_block_in_scope(AstNode *n) {
    if (!n || n->kind != AST_BLOCK) return;
    for (size_t i = 0; i < n->as.block.stmt_count; i++) check_stmt(n->as.block.stmts[i]);
}

static void check_block(AstNode *n) {
    if (!n || n->kind != AST_BLOCK) return;
    scope_push();
    check_block_in_scope(n);
    scope_pop();
}

/** Check a statement; `let` records whether the init is a capturing fn. */
static void check_stmt(AstNode *n) {
    if (!n) return;
    switch (n->kind) {
        case AST_VAR_DECL: {
            Type *ann = n->as.var.type ? resolve_type(n->as.var.type) : NULL;
            Type *it = n->as.var.init ? check_expr_ty(n->as.var.init, ann) : ty_void();
            if (ann && it && !type_eq(ann, it))
                err(n->loc, "let type %s does not match initializer %s", type_name(ann), type_name(it));
            Type *ty = ann ? ann : it;
            n->ty = ty;
            scope_add(n->as.var.name, ty, n->as.var.is_mut, expr_is_cap_fn(n->as.var.init), n->loc,
                      n);
            break;
        }
        case AST_ASSIGN: {
            Type *l = check_expr(n->as.assign.left);
            TokenKind aop = n->as.assign.op;
            Type *r = check_expr_ty(n->as.assign.right,
                                    (aop == TOK_EQ || type_is_numeric(l)) ? l : NULL);
            if (!n->as.assign.left->place_mut)
                err(n->loc, "cannot assign to immutable place");
            if (aop == TOK_EQ) {
                if (!type_eq(l, r))
                    err(n->loc, "cannot assign %s to %s", type_name(r), type_name(l));
            } else {
                int bit_mod = aop == TOK_PERCENT_EQ || aop == TOK_AMP_EQ ||
                              aop == TOK_PIPE_EQ || aop == TOK_CARET_EQ ||
                              aop == TOK_SHL_EQ || aop == TOK_SHR_EQ;
                int ok = 0;
                if (type_is_int_kind(l) && type_is_int_kind(r))
                    ok = type_eq(l, r);
                else if (!bit_mod && type_is_float_kind(l) && type_is_float_kind(r))
                    ok = type_eq(l, r);
                if (!ok) {
                    if (type_is_numeric(l) && type_is_numeric(r) && !type_eq(l, r))
                        err(n->loc, "cannot mix %s and %s in compound assignment; convert explicitly",
                            type_name(l), type_name(r));
                    else if (bit_mod)
                        err(n->loc, "%%=, &=, |=, ^=, <<=, >>= require matching int operands");
                    else
                        err(n->loc, "compound assignment requires int or float");
                }
            }
            break;
        }
        case AST_EXPR_STMT:
            check_expr(n->as.expr_stmt.expr);
            break;
        case AST_RETURN: {
            Type *want = cur_ret ? cur_ret : ty_void();
            Type *t = n->as.ret.expr
                ? check_expr_ty(n->as.ret.expr, clos_infer ? NULL : want)
                : ty_void();
            if (n->as.ret.expr && want->kind == TY_PTR) {
                if (n->as.ret.expr->kind == AST_ADDR &&
                    n->as.ret.expr->as.access.target &&
                    n->as.ret.expr->as.access.target->kind == AST_IDENT)
                    err(n->loc, "cannot return borrow of local '%s'",
                        n->as.ret.expr->as.access.target->as.ident.name);
            }
            if (clos_infer) {
                if (!clos_infer_ty) clos_infer_ty = t;
                else if (!type_eq(t, clos_infer_ty))
                    err(n->loc, "return type %s does not match %s", type_name(t),
                        type_name(clos_infer_ty));
            } else if (!type_eq(t, want)) {
                err(n->loc, "return type %s does not match %s", type_name(t), type_name(want));
            }
            break;
        }
        case AST_IF:
            if (!type_eq(check_expr(n->as.if_stmt.cond), ty_bool()))
                err(n->as.if_stmt.cond->loc, "if condition must be bool");
            check_block(n->as.if_stmt.then_block);
            if (n->as.if_stmt.else_block) {
                if (n->as.if_stmt.else_block->kind == AST_IF)
                    check_stmt(n->as.if_stmt.else_block);
                else
                    check_block(n->as.if_stmt.else_block);
            }
            break;
        case AST_FOR: {
            if (!n->as.for_stmt.iter || n->as.for_stmt.iter->kind != AST_BINARY ||
                n->as.for_stmt.iter->as.binary.op != TOK_DOT_DOT)
                err(n->loc, "for-loop requires a range `lo..hi`");
            else
                check_expr(n->as.for_stmt.iter);
            scope_push();
            scope_add(n->as.for_stmt.var, ty_int(), 0, 0, n->loc, n);
            loop_depth++;
            check_block(n->as.for_stmt.body);
            loop_depth--;
            scope_pop();
            break;
        }
        case AST_WHILE:
            if (!type_eq(check_expr(n->as.if_stmt.cond), ty_bool()))
                err(n->as.if_stmt.cond->loc, "while condition must be bool");
            loop_depth++;
            check_block(n->as.if_stmt.then_block);
            loop_depth--;
            break;
        case AST_BREAK:
            if (loop_depth <= 0) err(n->loc, "`break` outside of a loop");
            break;
        case AST_CONTINUE:
            if (loop_depth <= 0) err(n->loc, "`continue` outside of a loop");
            break;
        case AST_MATCH: {
            Type *st = check_expr(n->as.match_stmt.scrut);
            if (st && st->kind != TY_INT && st->kind != TY_FLOAT && st->kind != TY_BOOL &&
                st->kind != TY_STRING)
                err(n->loc, "match requires int, float, bool, or string");
            int saw_wild = 0;
            int saw_true = 0, saw_false = 0;
            if (n->as.match_stmt.arm_count == 0)
                err(n->loc, "match needs at least one arm");
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++) {
                AstNode *arm = n->as.match_stmt.arms[i];
                if (!arm || arm->kind != AST_MATCH_ARM) continue;
                if (saw_wild)
                    err(arm->loc, "unreachable match arm after `_`");
                if (arm->as.match_arm.is_wild) {
                    saw_wild = 1;
                    check_block(arm->as.match_arm.body);
                    continue;
                }
                for (size_t p = 0; p < arm->as.match_arm.pat_count; p++) {
                    Type *pt = check_expr_ty(arm->as.match_arm.pats[p],
                                             type_is_numeric(st) ? st : NULL);
                    if (st && pt && !type_eq(st, pt))
                        err(arm->as.match_arm.pats[p]->loc, "pattern type %s does not match %s",
                            type_name(pt), type_name(st));
                    AstNode *pat = arm->as.match_arm.pats[p];
                    if (pat && pat->kind == AST_BOOL) {
                        if (pat->as.lit.b) saw_true = 1;
                        else saw_false = 1;
                    }
                }
                check_block(arm->as.match_arm.body);
            }
            if (st && st->kind == TY_BOOL) {
                if (!saw_wild && !(saw_true && saw_false))
                    err(n->loc, "match on bool is not exhaustive (add `_` or both true and false)");
            } else if (!saw_wild) {
                err(n->loc, "match must be exhaustive (add `_`)");
            }
            break;
        }
        case AST_BLOCK:
            check_block(n);
            break;
        default:
            break;
    }
}

/** Check one function body with params in scope. */
static void check_fn(AstNode *fn) {
    Type *ft = fn_type_of(fn);
    const char **save_tp = cur_tparams;
    size_t save_n = cur_ntparams;
    cur_tparams = fn->as.fn.tparams;
    cur_ntparams = fn->as.fn.tparam_count;
    cur_ret = ft->ret ? ft->ret : ty_void();
    loop_depth = 0;
    int saved_async = cur_async;
    cur_async = fn->as.fn.is_async;
    /* Parameter defaults (`name: T = expr`) must trail the list, be
       constant, and type-check against the parameter — in this module's
       scope, before params are bound, so a default cannot see a caller's
       locals or a sibling parameter. */
    if (fn->as.fn.param_count) {
        int seen_default = 0;
        for (size_t i = 0; i < fn->as.fn.param_count; i++) {
            AstNode *def = fn->as.fn.params[i].def;
            if (!def) {
                if (seen_default)
                    err(fn->as.fn.params[i].loc,
                        "parameter '%s' follows a defaulted parameter and needs a default too",
                        fn->as.fn.params[i].name);
                continue;
            }
            seen_default = 1;
            if (!clone_const(def)) {
                err(fn->as.fn.params[i].loc,
                    "default for parameter '%s' must be a constant expression",
                    fn->as.fn.params[i].name);
                continue;
            }
            Type *pt = (i < ft->param_count) ? ft->params[i] : ty_void();
            /* A default may mention a type parameter (`x: T = 5`): the
               expression is checked on its own, and the call site unifies
               it with T after the other arguments have bound T — or, if
               every argument is omitted, from the default itself. */
            Type *dt = check_expr_ty(def, type_has_param(pt) ? NULL : pt);
            if (dt && pt && !type_eq(dt, pt) && !type_has_param(pt))
                err(fn->as.fn.params[i].loc,
                    "default for parameter '%s' has type %s, expected %s",
                    fn->as.fn.params[i].name, type_name(dt), type_name(pt));
        }
    }
    scope_push();
    for (size_t i = 0; i < fn->as.fn.param_count; i++) {
        Type *pt = ft->params[i];
        /* params are immutable bindings; &mut T still allows mutating through the ref */
        scope_add(fn->as.fn.params[i].name, pt, 0, 0, fn->as.fn.params[i].loc, fn);
    }
    if (cur_ret && cur_ret->kind != TY_VOID) tail_expr_to_return(fn->as.fn.body);
    check_block_in_scope(fn->as.fn.body);
    scope_pop();
    cur_ret = NULL;
    cur_async = saved_async;
    cur_tparams = save_tp;
    cur_ntparams = save_n;
}

static void program_add_decl(AstNode *prog, AstNode *d) {
    if (!prog || !d) return;
    size_t n = prog->as.program.decl_count;
    AstNode **ds = (AstNode **)realloc(prog->as.program.decls, (n + 1) * sizeof(AstNode *));
    if (!ds) loam_fatal("out of memory");
    ds[n] = d;
    prog->as.program.decls = ds;
    prog->as.program.decl_count = n + 1;
}

static AstNode *named_type_node(const char *name, SourceLoc loc) {
    return ast_type(loam_dup(name), 2, NULL, 0, loc);
}

static int proto_scalar_ok(Type *t) {
    return t && (t->kind == TY_INT || t->kind == TY_STRING);
}

/* A field is a scalar, or `[]T` of one — i.e. a `repeated` field. There are no
   nested messages yet. */
static int proto_field_ok(Type *t) {
    if (proto_scalar_ok(t)) return 1;
    return t && t->kind == TY_VEC && proto_scalar_ok(t->elem);
}

static int proto_repeated(Type *t) { return t && t->kind == TY_VEC; }

/* The wire type a field's payload carries: 2 for length-delimited (a string),
   0 for a varint. An element of a repeated string field is still a string. */
static int proto_want_wt(Type *ft) {
    Type *et = proto_repeated(ft) ? ft->elem : ft;
    return (et && et->kind == TY_STRING) ? 2 : 0;
}

static AstNode *proto_id(const char *n, SourceLoc loc) {
    return ast_ident(loam_dup(n), loc);
}

static AstNode *proto_http(const char *fn, SourceLoc loc) {
    return ast_field(proto_id("http", loc), loam_dup(fn), 0, loc);
}

static AstNode *proto_call(const char *fn, AstNode **args, size_t n, SourceLoc loc) {
    return ast_call(proto_http(fn, loc), args, n, loc);
}

static void proto_stmts_add(AstNode ***ps, size_t *n, AstNode *s) {
    *ps = (AstNode **)realloc(*ps, (*n + 1) * sizeof(AstNode *));
    if (!*ps) loam_fatal("out of memory");
    (*ps)[*n] = s;
    (*n)++;
}

static AstNode *proto_empty_bytes(SourceLoc loc) {
    return ast_array_lit(ast_type(loam_dup("int"), 2, NULL, 0, loc), -1, NULL, 0, loc);
}

static AstNode *proto_skip_i(SourceLoc loc) {
    AstNode **a = (AstNode **)malloc(3 * sizeof(AstNode *));
    a[0] = proto_id("b", loc);
    a[1] = proto_id("i", loc);
    a[2] = proto_id("wt", loc);
    return ast_assign(TOK_EQ, proto_id("i", loc), proto_call("skip_field", a, 3, loc), loc);
}

static AstNode *proto_block1(AstNode *s, SourceLoc loc) {
    AstNode **st = (AstNode **)malloc(sizeof(AstNode *));
    st[0] = s;
    return ast_block(st, 1, loc);
}

static AstNode *proto_field_payload(Type *ft, const char *fnm, SourceLoc loc) {
    AstNode **st = NULL;
    size_t n = 0;
    int repeated = proto_repeated(ft);
    Type *et = repeated ? ft->elem : ft;
    AstNode **ga = (AstNode **)malloc(2 * sizeof(AstNode *));
    ga[0] = proto_id("b", loc);
    ga[1] = proto_id("i", loc);
    if (et && et->kind == TY_STRING) {
        proto_stmts_add(&st, &n,
                        ast_var(loam_dup("v"), NULL, proto_call("decode_string_at", ga, 2, loc), 0,
                                loc));
    } else {
        proto_stmts_add(&st, &n,
                        ast_var(loam_dup("v"), NULL, proto_call("get_varint", ga, 2, loc), 0, loc));
    }
    proto_stmts_add(&st, &n,
                    ast_assign(TOK_EQ, proto_id("i", loc),
                               ast_field(proto_id("v", loc), loam_dup("next"), 0, loc), loc));
    if (repeated) {
        /* `m.f.push(v.val)` — an element appends, it does not replace the list. */
        AstNode **pa = (AstNode **)malloc(sizeof(AstNode *));
        pa[0] = ast_field(proto_id("v", loc), loam_dup("val"), 0, loc);
        AstNode *push = ast_call(ast_field(ast_field(proto_id("m", loc), loam_dup(fnm), 0, loc),
                                           loam_dup("push"), 0, loc),
                                 pa, 1, loc);
        proto_stmts_add(&st, &n, ast_expr_stmt(push, loc));
    } else {
        proto_stmts_add(&st, &n,
                        ast_assign(TOK_EQ,
                                   ast_field(proto_id("m", loc), loam_dup(fnm), 0, loc),
                                   ast_field(proto_id("v", loc), loam_dup("val"), 0, loc), loc));
    }
    return ast_block(st, n, loc);
}

/* The wire-type-2 branch of a repeated int field: `i = push_packed_ints(m.f, b, i)`.
   Packed is what another language's protobuf sends by default, and reading it as
   the unpacked form would silently yield an empty list. */
static AstNode *proto_packed_ints(const char *fnm, SourceLoc loc) {
    AstNode **aa = (AstNode **)malloc(3 * sizeof(AstNode *));
    aa[0] = ast_field(proto_id("m", loc), loam_dup(fnm), 0, loc);
    aa[1] = proto_id("b", loc);
    aa[2] = proto_id("i", loc);
    return ast_assign(TOK_EQ, proto_id("i", loc),
                      proto_call("push_packed_ints", aa, 3, loc), loc);
}

static AstNode *proto_one_field(Type *ft, const char *fnm, SourceLoc loc) {
    int want_wt = proto_want_wt(ft);
    AstNode *ok = proto_field_payload(ft, fnm, loc);
    AstNode *bad = proto_block1(proto_skip_i(loc), loc);
    if (proto_repeated(ft) && want_wt == 0) {
        /* One key per element, or one packed run for the whole list. */
        AstNode *scalar = ast_if(ast_binary(TOK_EQ_EQ, proto_id("wt", loc),
                                            ast_number(0, loc), loc),
                                 ok, bad, loc);
        return ast_if(ast_binary(TOK_EQ_EQ, proto_id("wt", loc), ast_number(2, loc), loc),
                      proto_block1(proto_packed_ints(fnm, loc), loc), scalar, loc);
    }
    return ast_if(ast_binary(TOK_EQ_EQ, proto_id("wt", loc), ast_number(want_wt, loc), loc), ok,
                  bad, loc);
}

static AstNode *proto_tag_chain(Type *t, size_t from, SourceLoc loc) {
    if (!t || from >= t->field_count)
        return proto_block1(proto_skip_i(loc), loc);
    const char *fnm = t->field_names[from];
    Type *ft = t->field_types[from];
    int tag = (int)from + 1;
    AstNode *thenb = proto_block1(proto_one_field(ft, fnm ? fnm : "_", loc), loc);
    AstNode *elseb = proto_tag_chain(t, from + 1, loc);
    return ast_if(ast_binary(TOK_EQ_EQ, proto_id("tag", loc), ast_number(tag, loc), loc), thenb,
                  elseb, loc);
}

static AstNode *proto_encode_body(AstNode *st, Type *t, SourceLoc loc) {
    AstNode **stmts = NULL;
    size_t n = 0;
    proto_stmts_add(&stmts, &n, ast_var(loam_dup("out"), NULL, proto_empty_bytes(loc), 1, loc));
    if (t) {
        for (size_t f = 0; f < t->field_count; f++) {
            const char *fnm = t->field_names[f];
            Type *ft = t->field_types[f];
            if (!fnm || !ft) continue;
            AstNode *tag = ast_number((int64_t)f + 1, loc);
            if (proto_repeated(ft)) {
                /* for e in 0..m.f.len { out = append_bytes(out, encode_*_elem(tag, m.f[e])) } */
                const char *elem = (ft->elem && ft->elem->kind == TY_STRING)
                                       ? "encode_string_elem"
                                       : "encode_int_elem";
                AstNode **fa = (AstNode **)malloc(2 * sizeof(AstNode *));
                fa[0] = tag;
                fa[1] = ast_index(ast_field(proto_id("m", loc), loam_dup(fnm), 0, loc),
                                  proto_id("e", loc), loc);
                AstNode **aa = (AstNode **)malloc(2 * sizeof(AstNode *));
                aa[0] = proto_id("out", loc);
                aa[1] = proto_call(elem, fa, 2, loc);
                AstNode *body = proto_block1(
                    ast_expr_stmt(proto_call("append_bytes", aa, 2, loc), loc), loc);
                AstNode *iter = ast_binary(
                    TOK_DOT_DOT, ast_number(0, loc),
                    ast_field(ast_field(proto_id("m", loc), loam_dup(fnm), 0, loc),
                              loam_dup("len"), 0, loc),
                    loc);
                proto_stmts_add(&stmts, &n, ast_for(loam_dup("e"), iter, body, loc));
                continue;
            }
            const char *enc = ft->kind == TY_STRING ? "encode_string_field" : "encode_int_field";
            AstNode **fa = (AstNode **)malloc(2 * sizeof(AstNode *));
            fa[0] = tag;
            fa[1] = ast_field(proto_id("m", loc), loam_dup(fnm), 0, loc);
            AstNode **aa = (AstNode **)malloc(2 * sizeof(AstNode *));
            aa[0] = proto_id("out", loc);
            aa[1] = proto_call(enc, fa, 2, loc);
            proto_stmts_add(&stmts, &n, ast_expr_stmt(proto_call("append_bytes", aa, 2, loc), loc));
        }
    }
    {
        AstNode **sa = (AstNode **)malloc(sizeof(AstNode *));
        sa[0] = proto_id("out", loc);
        proto_stmts_add(&stmts, &n, ast_expr_stmt(proto_call("string_of", sa, 1, loc), loc));
    }
    (void)st;
    return ast_block(stmts, n, loc);
}

static AstNode *proto_zero_lit(AstNode *st, Type *t, SourceLoc loc) {
    FieldInit *fi = NULL;
    size_t fc = 0;
    if (t) {
        fc = t->field_count;
        fi = (FieldInit *)calloc(fc, sizeof(FieldInit));
        for (size_t f = 0; f < fc; f++) {
            fi[f].name = loam_dup(t->field_names[f] ? t->field_names[f] : "_");
            Type *ft = t->field_types[f];
            if (proto_repeated(ft)) {
                /* An empty `[]T`, so a message that arrives with the field unset
                   decodes to an empty list rather than to null. */
                const char *en = (ft->elem && ft->elem->kind == TY_STRING) ? "string" : "int";
                fi[f].init = ast_array_lit(named_type_node(en, loc), -1, NULL, 0, loc);
            } else if (ft && ft->kind == TY_STRING) {
                fi[f].init = ast_string(loam_dup(""), loc);
            } else {
                fi[f].init = ast_number(0, loc);
            }
        }
    }
    return ast_struct_lit(loam_dup(st->as.strct.name), fi, fc, loc);
}

static AstNode *proto_decode_body(AstNode *st, Type *t, SourceLoc loc) {
    AstNode **stmts = NULL;
    size_t n = 0;
    AstNode **ba = (AstNode **)malloc(sizeof(AstNode *));
    ba[0] = proto_id("buf", loc);
    proto_stmts_add(&stmts, &n,
                    ast_var(loam_dup("b"), NULL, proto_call("bytes_of", ba, 1, loc), 0, loc));
    proto_stmts_add(&stmts, &n, ast_var(loam_dup("m"), NULL, proto_zero_lit(st, t, loc), 1, loc));
    proto_stmts_add(&stmts, &n, ast_var(loam_dup("i"), NULL, ast_number(0, loc), 1, loc));
    proto_stmts_add(&stmts, &n, ast_var(loam_dup("more"), NULL, ast_number(1, loc), 1, loc));

    AstNode **ga = (AstNode **)malloc(2 * sizeof(AstNode *));
    ga[0] = proto_id("b", loc);
    ga[1] = proto_id("i", loc);

    AstNode **got = NULL;
    size_t gn = 0;
    proto_stmts_add(&got, &gn,
                    ast_assign(TOK_EQ, proto_id("i", loc),
                               ast_field(proto_id("kv", loc), loam_dup("next"), 0, loc), loc));
    proto_stmts_add(&got, &gn,
                    ast_var(loam_dup("tag"), NULL,
                            ast_binary(TOK_SLASH, ast_field(proto_id("kv", loc), loam_dup("val"), 0, loc),
                                       ast_number(8, loc), loc),
                            0, loc));
    proto_stmts_add(&got, &gn,
                    ast_var(loam_dup("wt"), NULL,
                            ast_binary(TOK_PERCENT, ast_field(proto_id("kv", loc), loam_dup("val"), 0, loc),
                                       ast_number(8, loc), loc),
                            0, loc));
    proto_stmts_add(&got, &gn, proto_tag_chain(t, 0, loc));

    AstNode **core = NULL;
    size_t cn = 0;
    proto_stmts_add(&core, &cn,
                    ast_var(loam_dup("kv"), NULL, proto_call("get_varint", ga, 2, loc), 0, loc));
    proto_stmts_add(
        &core, &cn,
        ast_if(ast_binary(TOK_LT_EQ, ast_field(proto_id("kv", loc), loam_dup("next"), 0, loc),
                          proto_id("i", loc), loc),
               proto_block1(ast_assign(TOK_EQ, proto_id("more", loc), ast_number(0, loc), loc), loc),
               NULL, loc));
    proto_stmts_add(&core, &cn,
                    ast_if(ast_binary(TOK_EQ_EQ, proto_id("more", loc), ast_number(1, loc), loc),
                           ast_block(got, gn, loc), NULL, loc));

    AstNode **loopst = NULL;
    size_t ln = 0;
    proto_stmts_add(&loopst, &ln,
                    ast_if(ast_binary(TOK_GT_EQ, proto_id("i", loc),
                                      ast_field(proto_id("b", loc), loam_dup("len"), 0, loc), loc),
                           proto_block1(ast_assign(TOK_EQ, proto_id("more", loc), ast_number(0, loc), loc),
                                        loc),
                           NULL, loc));
    proto_stmts_add(&loopst, &ln,
                    ast_if(ast_binary(TOK_EQ_EQ, proto_id("more", loc), ast_number(1, loc), loc),
                           ast_block(core, cn, loc), NULL, loc));

    proto_stmts_add(
        &stmts, &n,
        ast_for(loam_dup("k"),
                ast_binary(TOK_DOT_DOT, ast_number(0, loc),
                           ast_field(proto_id("b", loc), loam_dup("len"), 0, loc), loc),
                ast_block(loopst, ln, loc), loc));
    proto_stmts_add(&stmts, &n, ast_expr_stmt(proto_id("m", loc), loc));
    return ast_block(stmts, n, loc);
}

static void inject_proto_fn(AstNode *prog, AstNode *st, int is_encode) {
    const char *sname = st->as.strct.name;
    char fnname[256];
    snprintf(fnname, sizeof fnname, "%s_%s", is_encode ? "encode" : "decode", sname);
    for (size_t i = 0; i < prog->as.program.decl_count; i++) {
        AstNode *d = prog->as.program.decls[i];
        if (d->kind == AST_FN_DECL && d->as.fn.name && strcmp(d->as.fn.name, fnname) == 0) {
            err(st->loc, "'%s' is reserved for #[proto] struct %s", fnname, sname);
            return;
        }
    }
    Param *ps = (Param *)calloc(1, sizeof(Param));
    if (!ps) loam_fatal("out of memory");
    SourceLoc loc = st->loc;
    AstNode *ret;
    Type *t = struct_type_of(st);
    if (is_encode) {
        ps[0].name = loam_dup("m");
        ps[0].type = named_type_node(sname, loc);
        ps[0].loc = loc;
        ret = named_type_node("string", loc);
    } else {
        ps[0].name = loam_dup("buf");
        ps[0].type = named_type_node("string", loc);
        ps[0].loc = loc;
        ret = named_type_node(sname, loc);
    }
    AstNode *body = is_encode ? proto_encode_body(st, t, loc) : proto_decode_body(st, t, loc);
    AstNode *fn = ast_fn(loam_dup(fnname), ps, 1, ret, body, loc);
    program_add_decl(prog, fn);
}

static void inject_proto_codecs(AstNode *prog) {
    if (!prog) return;
    size_t n = prog->as.program.decl_count;
    for (size_t i = 0; i < n; i++) {
        AstNode *d = prog->as.program.decls[i];
        if (d->kind != AST_STRUCT_DECL || !d->as.strct.is_proto) continue;
        if (d->as.strct.tparam_count) {
            err(d->loc, "#[proto] struct cannot have type parameters");
            continue;
        }
        Type *t = struct_type_of(d);
        for (size_t f = 0; f < t->field_count; f++) {
            if (!proto_field_ok(t->field_types[f])) {
                err(d->loc, "#[proto] field '%s' must be int, string, or a list of those",
                    t->field_names[f]);
            }
        }
        inject_proto_fn(prog, d, 1);
        inject_proto_fn(prog, d, 0);
    }
}

// --- #[json] derive: json_encode_<S> / json_decode_<S> ---

static int json_field_ok(Type *t) {
    return t && (t->kind == TY_INT || t->kind == TY_STRING);
}

static AstNode *jdent(const char *n, SourceLoc loc) {
    return ast_ident(loam_dup(n), loc);
}

static AstNode *jfield(AstNode *base, const char *n, SourceLoc loc) {
    return ast_field(base, loam_dup(n), 0, loc);
}

static AstNode *json_mod_fn(const char *fn, SourceLoc loc) {
    return jfield(jdent("json", loc), fn, loc);
}

static AstNode *json_call(const char *fn, AstNode **args, size_t n, SourceLoc loc) {
    return ast_call(json_mod_fn(fn, loc), args, n, loc);
}

static AstNode **one(AstNode *a) {
    AstNode **v = (AstNode **)malloc(sizeof(AstNode *));
    if (!v) loam_fatal("out of memory");
    v[0] = a;
    return v;
}

static AstNode *jstr(const char *s, SourceLoc loc) {
    return ast_string(loam_dup(s), loc);
}

/** `if (<ok> == 0) { json.set_err(<err>); return <zero struct>; }` */
static AstNode *json_guard_fail(AstNode *st, Type *t, AstNode *ok, AstNode *errv,
                                SourceLoc loc) {
    AstNode **ss = NULL;
    size_t n = 0;
    proto_stmts_add(&ss, &n, ast_expr_stmt(json_call("set_err", one(errv), 1, loc), loc));
    proto_stmts_add(&ss, &n, ast_return(proto_zero_lit(st, t, loc), loc));
    return ast_if(ast_binary(TOK_EQ_EQ, ok, ast_number(0, loc), loc), ast_block(ss, n, loc), NULL,
                  loc);
}

/** Block: `{ json.set_err(<px>); return <zero struct>; }` — used for an
    out-of-range integer so the narrowing conversion is never reached. */
static AstNode *json_range_fail(AstNode *st, Type *t, AstNode *px, SourceLoc loc) {
    AstNode **ss = NULL;
    size_t n = 0;
    proto_stmts_add(&ss, &n, ast_expr_stmt(json_call("set_err", one(px), 1, loc), loc));
    proto_stmts_add(&ss, &n, ast_return(proto_zero_lit(st, t, loc), loc));
    return ast_block(ss, n, loc);
}

static AstNode *json_encode_body(AstNode *st, Type *t, SourceLoc loc) {
    AstNode **stmts = NULL;
    size_t n = 0;
    proto_stmts_add(&stmts, &n, ast_expr_stmt(json_call("enc_obj_begin", NULL, 0, loc), loc));
    if (t) {
        for (size_t f = 0; f < t->field_count; f++) {
            const char *fnm = t->field_names[f];
            Type *ft = t->field_types[f];
            if (!fnm || !ft) continue;
            proto_stmts_add(&stmts, &n,
                            ast_expr_stmt(json_call("enc_key", one(jstr(fnm, loc)), 1, loc), loc));
            AstNode *val = jfield(jdent("m", loc), fnm, loc);
            const char *enc;
            if (ft->kind == TY_STRING) {
                enc = "enc_str";
            } else {
                enc = "enc_int";
                val = ast_call(jdent("i64", loc), one(val), 1, loc); // widen to i64
            }
            proto_stmts_add(&stmts, &n, ast_expr_stmt(json_call(enc, one(val), 1, loc), loc));
        }
    }
    proto_stmts_add(&stmts, &n, ast_expr_stmt(json_call("enc_obj_end", NULL, 0, loc), loc));
    proto_stmts_add(&stmts, &n, ast_return(json_call("enc_take", NULL, 0, loc), loc));
    (void)st;
    return ast_block(stmts, n, loc);
}

static AstNode *json_decode_body(AstNode *st, Type *t, SourceLoc loc) {
    AstNode **stmts = NULL;
    size_t n = 0;
    proto_stmts_add(&stmts, &n, ast_var(loam_dup("m"), NULL, proto_zero_lit(st, t, loc), 1, loc));
    if (t) {
        for (size_t f = 0; f < t->field_count; f++) {
            const char *fnm = t->field_names[f];
            Type *ft = t->field_types[f];
            if (!fnm || !ft) continue;
            /* Each field is its own block so `px` / `f` / `v` do not collide. */
            AstNode **fs = NULL;
            size_t fn_ = 0;
            AstNode **pa = (AstNode **)malloc(2 * sizeof(AstNode *));
            pa[0] = jdent("path", loc);
            pa[1] = jstr(fnm, loc);
            proto_stmts_add(&fs, &fn_,
                            ast_var(loam_dup("px"), NULL, json_call("child", pa, 2, loc), 0, loc));
            AstNode **fa = (AstNode **)malloc(3 * sizeof(AstNode *));
            fa[0] = jdent("id", loc);
            fa[1] = jstr(fnm, loc);
            fa[2] = jdent("px", loc);
            proto_stmts_add(&fs, &fn_,
                            ast_var(loam_dup("f"), NULL, json_call("field", fa, 3, loc), 0, loc));
            proto_stmts_add(&fs, &fn_,
                            json_guard_fail(st, t, jfield(jdent("f", loc), "ok", loc),
                                            jfield(jdent("f", loc), "err", loc), loc));
            AstNode **va = (AstNode **)malloc(2 * sizeof(AstNode *));
            va[0] = jfield(jdent("f", loc), "id", loc);
            va[1] = jdent("px", loc);
            const char *getter = ft->kind == TY_STRING ? "as_str" : "as_int";
            proto_stmts_add(&fs, &fn_,
                            ast_var(loam_dup("v"), NULL, json_call(getter, va, 2, loc), 0, loc));
            proto_stmts_add(&fs, &fn_,
                            json_guard_fail(st, t, jfield(jdent("v", loc), "ok", loc),
                                            jfield(jdent("v", loc), "err", loc), loc));
            AstNode *target = jfield(jdent("m", loc), fnm, loc);
            if (ft->kind == TY_STRING) {
                proto_stmts_add(
                    &fs, &fn_,
                    ast_assign(TOK_EQ, target, jfield(jdent("v", loc), "s", loc), loc));
            } else {
                // Range-check the i64 against i32 before the narrowing conversion,
                // so an out-of-range value is an error, not a trap.
                proto_stmts_add(
                    &fs, &fn_,
                    ast_if(ast_binary(TOK_GT, jfield(jdent("v", loc), "n", loc),
                                      ast_number(2147483647, loc), loc),
                           json_range_fail(st, t, jdent("px", loc), loc),
                           NULL, loc));
                proto_stmts_add(
                    &fs, &fn_,
                    ast_if(ast_binary(TOK_LT, jfield(jdent("v", loc), "n", loc),
                                      ast_number(-2147483648LL, loc), loc),
                           json_range_fail(st, t, jdent("px", loc), loc),
                           NULL, loc));
                proto_stmts_add(
                    &fs, &fn_,
                    ast_assign(TOK_EQ, target,
                               ast_call(jdent("int", loc), one(jfield(jdent("v", loc), "n", loc)),
                                        1, loc),
                               loc));
            }
            proto_stmts_add(&stmts, &n, ast_block(fs, fn_, loc));
        }
    }
    proto_stmts_add(&stmts, &n, ast_return(jdent("m", loc), loc));
    return ast_block(stmts, n, loc);
}

static void inject_json_fn(AstNode *prog, AstNode *st, int is_encode) {
    const char *sname = st->as.strct.name;
    char fnname[256];
    snprintf(fnname, sizeof fnname, "%s_%s", is_encode ? "json_encode" : "json_decode", sname);
    for (size_t i = 0; i < prog->as.program.decl_count; i++) {
        AstNode *d = prog->as.program.decls[i];
        if (d->kind == AST_FN_DECL && d->as.fn.name && strcmp(d->as.fn.name, fnname) == 0) {
            err(st->loc, "'%s' is reserved for #[json] struct %s", fnname, sname);
            return;
        }
    }
    SourceLoc loc = st->loc;
    Type *t = struct_type_of(st);
    if (is_encode) {
        Param *ps = (Param *)calloc(1, sizeof(Param));
        if (!ps) loam_fatal("out of memory");
        ps[0].name = loam_dup("m");
        ps[0].type = named_type_node(sname, loc);
        ps[0].loc = loc;
        AstNode *fn = ast_fn(loam_dup(fnname), ps, 1, named_type_node("string", loc),
                            json_encode_body(st, t, loc), loc);
        program_add_decl(prog, fn);
    } else {
        Param *ps = (Param *)calloc(2, sizeof(Param));
        if (!ps) loam_fatal("out of memory");
        ps[0].name = loam_dup("id");
        ps[0].type = named_type_node("int", loc);
        ps[0].loc = loc;
        ps[1].name = loam_dup("path");
        ps[1].type = named_type_node("string", loc);
        ps[1].loc = loc;
        AstNode *fn = ast_fn(loam_dup(fnname), ps, 2, named_type_node(sname, loc),
                            json_decode_body(st, t, loc), loc);
        program_add_decl(prog, fn);
    }
}

static void inject_json_codecs(AstNode *prog) {
    if (!prog) return;
    size_t n = prog->as.program.decl_count;
    for (size_t i = 0; i < n; i++) {
        AstNode *d = prog->as.program.decls[i];
        if (d->kind != AST_STRUCT_DECL || !d->as.strct.is_json) continue;
        if (d->as.strct.tparam_count) {
            err(d->loc, "#[json] struct cannot have type parameters");
            continue;
        }
        Type *t = struct_type_of(d);
        for (size_t f = 0; f < t->field_count; f++) {
            if (!json_field_ok(t->field_types[f]))
                err(d->loc, "#[json] field '%s' must be int or string", t->field_names[f]);
        }
        inject_json_fn(prog, d, 1);
        inject_json_fn(prog, d, 0);
    }
}

/** The name a top-level declaration introduces, or NULL. */
static const char *decl_name(AstNode *d) {
    if (!d) return NULL;
    if (d->kind == AST_FN_DECL) return d->as.fn.name;
    if (d->kind == AST_STRUCT_DECL) return d->as.strct.name;
    if (d->kind == AST_ENUM_DECL) return d->as.enm.name;
    if (d->kind == AST_VAR_DECL) return d->as.var.name;
    return NULL;
}

/** A name defined twice in one module is an error at the second definition,
 *  with the first site named. Imported names still shadow by depth. */
static void check_duplicate_decls(AstNode *prog) {
    if (!prog) return;
    for (size_t i = 0; i < prog->as.program.decl_count; i++) {
        const char *a = decl_name(prog->as.program.decls[i]);
        if (!a) continue;
        for (size_t j = 0; j < i; j++) {
            const char *b = decl_name(prog->as.program.decls[j]);
            if (b && strcmp(a, b) == 0) {
                err(prog->as.program.decls[i]->loc,
                    "'%s' is already defined in this module (first at line %d)", a,
                    prog->as.program.decls[j]->loc.line);
                break;
            }
        }
    }
}

/** The generic fn a call refers to, or NULL. */
static AstNode *call_generic_fn(AstNode *call) {
    if (!call || call->kind != AST_CALL) return NULL;
    AstNode *cal = call->as.call.callee;
    if (cal && cal->kind == AST_IDENT && cal->as.ident.resolved &&
        cal->as.ident.resolved->kind == AST_FN_DECL &&
        cal->as.ident.resolved->as.fn.tparam_count)
        return cal->as.ident.resolved;
    if (cal && cal->kind == AST_FIELD && cal->as.access.resolved &&
        cal->as.access.resolved->kind == AST_FN_DECL &&
        cal->as.access.resolved->as.fn.tparam_count)
        return cal->as.access.resolved;
    return NULL;
}

/** Instantiate a generic callee of `call` under the caller's type subst.
    Returns the monomorphized C name, or NULL if T cannot be bound yet. */
const char *typecheck_callee_cname(AstNode *call, const char **names, Type **args, size_t n) {
    if (!call || call->kind != AST_CALL) return NULL;
    AstNode *fn = call_generic_fn(call);
    if (!fn) return call->as.call.resolved_cname;
    if (!n || !names || !args) return call->as.call.resolved_cname;
    Type *ft = fn_type_of(fn);
    if (!ft) return NULL;
    size_t nt = fn->as.fn.tparam_count;
    Type **bound = calloc(nt, sizeof(Type *));
    if (!bound) loam_fatal("out of memory");
    for (size_t i = 0; i < call->as.call.arg_count && i < ft->param_count; i++) {
        if (!call->as.call.args[i]) continue;
        Type *at = subst_type(call->as.call.args[i]->ty, names, args, n);
        unify(ft->params[i], at, fn->as.fn.tparams, bound, nt);
    }
    for (size_t i = 0; i < nt; i++) {
        if (bound[i]) bound[i] = subst_type(bound[i], names, args, n);
        if (!bound[i] || type_has_param(bound[i])) {
            free(bound);
            return NULL;
        }
    }
    char *cn = mono_cname(fn, bound, nt);
    record_mono(fn, bound, nt, cn);
    const char *out = NULL;
    for (int i = 0; i < nmono; i++) {
        if (monos[i].fn == fn && strcmp(monos[i].cname, cn) == 0) {
            out = monos[i].cname;
            break;
        }
        if (monos[i].fn == fn && monos[i].n == nt) {
            int same = 1;
            for (size_t k = 0; k < nt; k++)
                if (!type_eq(monos[i].args[k], bound[k])) same = 0;
            if (same) {
                out = monos[i].cname;
                break;
            }
        }
    }
    free(bound);
    return out ? out : cn;
}

static void walk_instantiate(AstNode *n, const char **names, Type **args, size_t na, int *added);

static void walk_instantiate(AstNode *n, const char **names, Type **args, size_t na, int *added) {
    if (!n) return;
    if (n->kind == AST_CALL) {
        int before = nmono;
        typecheck_callee_cname(n, names, args, na);
        if (nmono > before) *added = 1;
        walk_instantiate(n->as.call.callee, names, args, na, added);
        for (size_t i = 0; i < n->as.call.arg_count; i++)
            walk_instantiate(n->as.call.args[i], names, args, na, added);
        return;
    }
    if (n->kind == AST_CLOSURE) {
        walk_instantiate(n->as.fn.body, names, args, na, added);
        return;
    }
    switch (n->kind) {
        case AST_FN_DECL:
            walk_instantiate(n->as.fn.body, names, args, na, added);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                walk_instantiate(n->as.block.stmts[i], names, args, na, added);
            break;
        case AST_IF:
            walk_instantiate(n->as.if_stmt.cond, names, args, na, added);
            walk_instantiate(n->as.if_stmt.then_block, names, args, na, added);
            walk_instantiate(n->as.if_stmt.else_block, names, args, na, added);
            break;
        case AST_FOR:
            walk_instantiate(n->as.for_stmt.iter, names, args, na, added);
            walk_instantiate(n->as.for_stmt.body, names, args, na, added);
            break;
        case AST_WHILE:
            walk_instantiate(n->as.if_stmt.cond, names, args, na, added);
            walk_instantiate(n->as.if_stmt.then_block, names, args, na, added);
            break;
        case AST_MATCH:
            walk_instantiate(n->as.match_stmt.scrut, names, args, na, added);
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++)
                walk_instantiate(n->as.match_stmt.arms[i], names, args, na, added);
            break;
        case AST_MATCH_ARM:
            walk_instantiate(n->as.match_arm.body, names, args, na, added);
            break;
        case AST_RETURN:
            walk_instantiate(n->as.ret.expr, names, args, na, added);
            break;
        case AST_EXPR_STMT:
            walk_instantiate(n->as.expr_stmt.expr, names, args, na, added);
            break;
        case AST_VAR_DECL:
            walk_instantiate(n->as.var.init, names, args, na, added);
            break;
        case AST_ASSIGN:
            walk_instantiate(n->as.assign.left, names, args, na, added);
            walk_instantiate(n->as.assign.right, names, args, na, added);
            break;
        case AST_INCDEC:
            walk_instantiate(n->as.incdec.operand, names, args, na, added);
            break;
        case AST_BINARY:
            walk_instantiate(n->as.binary.left, names, args, na, added);
            walk_instantiate(n->as.binary.right, names, args, na, added);
            break;
        case AST_UNARY:
            walk_instantiate(n->as.unary.operand, names, args, na, added);
            break;
        case AST_CAST:
            walk_instantiate(n->as.cast.expr, names, args, na, added);
            break;
        case AST_DEREF:
        case AST_ADDR:
            walk_instantiate(n->as.access.target, names, args, na, added);
            break;
        case AST_INDEX:
        case AST_FIELD:
            walk_instantiate(n->as.access.target, names, args, na, added);
            walk_instantiate(n->as.access.index, names, args, na, added);
            break;
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++)
                walk_instantiate(n->as.struct_lit.fields[i].init, names, args, na, added);
            break;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                walk_instantiate(n->as.array_lit.elems[i], names, args, na, added);
            break;
        default:
            break;
    }
}

/** Walk each monomorphized body and instantiate generic calls inside it
    (generic-calls-generic, including closures). Repeats until a fixpoint. */
static void instantiate_nested(void) {
    int added = 1;
    while (added) {
        added = 0;
        int n = nmono;
        for (int i = 0; i < n; i++) {
            AstNode *fn = monos[i].fn;
            if (!fn) continue;
            walk_instantiate(fn->as.fn.body, fn->as.fn.tparams, monos[i].args, monos[i].n,
                             &added);
        }
    }
}

/**
 * Pass 1: types and C names, mark std intrinsics.
 * Pass 2: check bodies. Returns 1 if any error.
 */
int typecheck_modules(LoamModule *mods, int nmods) {
    Gmods = mods;
    Gn = nmods;
    Gerr = 0;
    scope = NULL;
    nmono = 0;
    nstruct_insts = 0;
    ngvars = 0;
    next_clos_id = 1;
    cur_tparams = NULL;
    cur_ntparams = 0;
    cur_clos = NULL;
    clos_scope = NULL;
    clos_depth = 0;

    /* pass 1: structs then fn signatures */
    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind == AST_STRUCT_DECL) struct_type_of(d);
        }
        check_duplicate_decls(p);
        inject_proto_codecs(p);
        inject_json_codecs(p);
    }
    for (int m = 0; m < nmods; m++) {
        int is_main_mod = (m == 0);
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_FN_DECL) continue;
            fn_type_of(d);
            if (d->as.fn.is_test) {
                if (d->as.fn.param_count != 0)
                    err(d->loc, "#[test] fn '%s' must take no parameters", d->as.fn.name);
                if (d->as.fn.ret_type)
                    err(d->loc, "#[test] fn '%s' must not return a value", d->as.fn.name);
                if (d->as.fn.tparam_count)
                    err(d->loc, "#[test] fn '%s' must not be generic", d->as.fn.name);
            }
            if (is_ffi_mod(mods[m].name) || user_seam_mod(mods[m].path)) {
                AstNode *body = d->as.fn.body;
                if (!body || (body->kind == AST_BLOCK && body->as.block.stmt_count == 0)) {
                    if (strcmp(d->as.fn.name, "main") != 0)
                        d->as.fn.is_intrinsic = 1;
                }
            }
            {
                const char *fnn = d->as.fn.name;
                if (is_main_mod && strcmp(fnn, "main") == 0) {
                    d->as.fn.cname = loam_dup("main");
                } else if (is_main_mod) {
                    size_t ln = 5 + strlen(fnn) + 1;
                    char *cn = malloc(ln);
                    snprintf(cn, ln, "loam_%s", fnn);
                    d->as.fn.cname = cn;
                } else {
                    size_t ln = 5 + strlen(mods[m].name) + 1 + strlen(fnn) + 1;
                    char *cn = malloc(ln);
                    snprintf(cn, ln, "loam_%s_%s", mods[m].name, fnn);
                    d->as.fn.cname = cn;
                }
            }
        }
    }

    /* pass 2a: type every module `let` so `mod.global` can see imported state. */
    for (int m = 0; m < nmods; m++) {
        Gcur = m;
        cur_mod_name = mods[m].name;
        AstNode *p = mods[m].ast;
        if (!p) continue;
        scope_push();
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_VAR_DECL) continue;
            check_stmt(d);
            if (ngvars >= LOAM_MAX_GLOBALS) {
                err(d->loc, "too many module globals (max %d)", LOAM_MAX_GLOBALS);
            } else {
                char buf[256];
                if (m == 0)
                    snprintf(buf, sizeof buf, "loam_%s", d->as.var.name);
                else
                    snprintf(buf, sizeof buf, "loam_%s_%s", mods[m].name, d->as.var.name);
                gvars[ngvars].var = d;
                gvars[ngvars].cname = loam_dup(buf);
                gvars[ngvars].mod = m;
                ngvars++;
            }
        }
        scope_pop();
    }

    /* pass 2b: function bodies. Same-module lets are in scope; other modules
       are `mod.name`. */
    for (int m = 0; m < nmods; m++) {
        Gcur = m;
        cur_mod_name = mods[m].name;
        AstNode *p = mods[m].ast;
        if (!p) continue;
        scope_push();
        for (int g = 0; g < ngvars; g++) {
            if (gvars[g].mod != m || !gvars[g].var) continue;
            AstNode *d = gvars[g].var;
            scope_add(d->as.var.name, d->ty, d->as.var.is_mut, 0, d->loc, d);
        }
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind == AST_FN_DECL && !d->as.fn.is_intrinsic)
                check_fn(d);
        }
        scope_pop();
    }

    instantiate_nested();

    while (scope) scope_pop();
    return Gerr;
}

/** Accessors for codegen: each monomorphized generic instance. */
int typecheck_mono_count(void) { return nmono; }
AstNode *typecheck_mono_fn(int i) { return (i >= 0 && i < nmono) ? monos[i].fn : NULL; }
const char *typecheck_mono_cname(int i) { return (i >= 0 && i < nmono) ? monos[i].cname : NULL; }
Type **typecheck_mono_args(int i) { return (i >= 0 && i < nmono) ? monos[i].args : NULL; }
size_t typecheck_mono_nargs(int i) { return (i >= 0 && i < nmono) ? monos[i].n : 0; }

int typecheck_struct_inst_count(void) { return nstruct_insts; }
Type *typecheck_struct_inst(int i) {
    return (i >= 0 && i < nstruct_insts) ? struct_insts[i] : NULL;
}

Type *typecheck_subst(Type *t, const char **names, Type **args, size_t n) {
    return subst_type(t, names, args, n);
}

int typecheck_global_count(void) { return ngvars; }
AstNode *typecheck_global_var(int i) {
    return (i >= 0 && i < ngvars) ? gvars[i].var : NULL;
}
const char *typecheck_global_cname(int i) {
    return (i >= 0 && i < ngvars) ? gvars[i].cname : NULL;
}
int typecheck_global_mod(int i) { return (i >= 0 && i < ngvars) ? gvars[i].mod : -1; }

/** Free mono tables and the type pool. */
void typecheck_cleanup(void) {
    for (int i = 0; i < nmono; i++) {
        free(monos[i].args);
        free(monos[i].cname);
        monos[i].fn = NULL;
        monos[i].args = NULL;
        monos[i].cname = NULL;
    }
    nmono = 0;
    nstruct_insts = 0;
    for (int i = 0; i < ngvars; i++) {
        free(gvars[i].cname);
        gvars[i].var = NULL;
        gvars[i].cname = NULL;
    }
    ngvars = 0;
    type_pool_reset();
}
