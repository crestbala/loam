/**
 * borrowck.c — moves, shared vs exclusive borrows, and reference escape.
 *
 * Every reference binding records the deepest frame binding it may point
 * into (`ref_depth`, 0 = caller or global data). Then:
 *   - a returned reference may not point into the frame (depth > 0);
 *   - when a block ends, a surviving reference into it becomes dangling, and
 *     reading it afterwards is an error (assigning it again revives it);
 *   - a reference written through `*pp` or into a global may not point at
 *     anything shallower-lived than that place.
 * Assignment replaces what a reference points at; branches and loops join
 * by taking the deepest. See docs/reference-escape.md.
 *
 * Loans: a reference binding owns the borrows it was made from (`let p =
 * &v[0]`, `p = &x`, or every reference argument of `p = first(&v)`), and they
 * last until its last use. While one is live, the place cannot be assigned,
 * moved, or borrowed mutably. A `&mut` reference passed to a call reborrows
 * it mutably.
 *
 * Bindings are a stack. Temps from `&x` in a statement are cleared at
 * statement end unless a binding takes them as loans.
 */
#include "borrowck.h"
#include "typecheck.h"
#include "../diagnostics.h"
#include "type.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum { ST_OWNED, ST_MOVED } Own;

/** Where a binding lives: a global outlives every call, a parameter of
 *  reference type points into the caller, a local dies with the frame. */
typedef enum { BK_LOCAL, BK_PARAM, BK_GLOBAL } BindKind;

typedef struct Binding {
    char *name;
    Own own;
    int shared;
    int mutb;
    int is_copy;
    int is_box;
    int depth;
    BindKind kind;
    /* Reference binding: depth of the deepest frame binding it may point
       into (0 = caller or global data), and that binding's name (AST-owned,
       for messages). `dangling`: that binding's scope has ended. */
    int ref_depth;
    const char *ref_root;
    int dangling;
    /* `#[must_check]`: a value dropped without being read is an error. */
    int must_check;
    int checked;
    SourceLoc loc;
    /* Places under this binding that were consumed field-wise
       (`on_click = p.h`). Ownership is a static fact: no runtime zeroing. */
    char **moved_paths;
    int nmoved;
    struct Binding *next;
} Binding;

/**
 * A live borrow, keyed by the place it covers rather than by the variable it
 * roots in: "p.a" and "p.b" are two borrows, "p" and "p.a" are one.
 *
 * A dynamic index cannot be proven disjoint from another, so `p[i]` narrows to
 * "p[]" and conflicts with every other element of p.
 */
typedef struct Borrow {
    char *path;
    int is_mut;
    int stored; /* held by a live binding as a loan, not a statement temp */
    int depth;
    struct Binding *owner;  /* the binding holding this loan, if stored */
    struct Binding *target; /* the binding the borrowed place roots in */
    struct Borrow *next;
} Borrow;

static Binding *binds;
static Borrow *borrows;
static int depth;
static int errn;

/** Two places conflict when they are equal or one contains the other. */
static int paths_conflict(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la < lb ? la : lb;
    if (strncmp(a, b, n) != 0) return 0;
    if (la == lb) return 1;
    /* The shorter must end on a component boundary of the longer, so "p.ab"
       does not read as contained in "p.a". */
    char next = la < lb ? b[n] : a[n];
    return next == '.' || next == '[';
}

static char *path_cat(const char *base, const char *suffix) {
    size_t n = strlen(base) + strlen(suffix);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    strcpy(p, base);
    strcat(p, suffix);
    return p;
}

/** Textual place for an lvalue expression, or NULL if it is not one. */
static char *path_of(AstNode *n) {
    if (!n) return NULL;
    if (n->kind == AST_IDENT) return loam_dup(n->as.ident.name);
    if (n->kind == AST_FIELD) {
        char *base = path_of(n->as.access.target);
        if (!base) return NULL;
        char *dot = path_cat(base, ".");
        char *full = dot ? path_cat(dot, n->as.access.field) : NULL;
        free(base);
        free(dot);
        return full;
    }
    if (n->kind == AST_INDEX) {
        char *base = path_of(n->as.access.target);
        if (!base) return NULL;
        char *full = path_cat(base, "[]");
        free(base);
        return full;
    }
    if (n->kind == AST_DEREF) return path_of(n->as.access.target);
    return NULL;
}

static Binding *find_b(const char *name) {
    for (Binding *b = binds; b; b = b->next)
        if (strcmp(b->name, name) == 0) return b;
    return NULL;
}

static void push_b(const char *name, Type *ty) {
    Binding *b = calloc(1, sizeof(Binding));
    b->name = loam_dup(name);
    b->own = ST_OWNED;
    b->is_copy = type_is_copy(ty);
    b->is_box = ty && ty->kind == TY_BOX;
    b->depth = depth;
    b->next = binds;
    binds = b;
}

static void drop_borrows_deeper_than(int d);
static void release_owned(Binding *b);
static void drop_borrows_of(Binding *b);

static void pop_to_depth(int d) {
    drop_borrows_deeper_than(d);
    while (binds && binds->depth > d) {
        Binding *b = binds;
        binds = b->next;
        /* A `#[must_check]` value that was neither read nor moved out is a
           bug: the caller ignored a failure. */
        if (b->must_check && !b->checked && b->own != ST_MOVED) {
            loam_error(b->loc,
                       "unused `#[must_check]` value '%s' — read a field or use "
                       "`.or()` / `.or_trap()`",
                       b->name);
            errn = 1;
        }
        release_owned(b);
        drop_borrows_of(b);
        free(b->name);
        for (int i = 0; i < b->nmoved; i++) free(b->moved_paths[i]);
        free(b->moved_paths);
        free(b);
    }
    /* A reference that outlives the binding it points at is dangling until
       it is assigned again. */
    for (Binding *b = binds; b; b = b->next)
        if (b->ref_depth > d) b->dangling = 1;
}

/** Root variable of a place, for the move/ownership checks. */
static const char *root_of_path(const char *path, char *buf, size_t cap) {
    size_t i = 0;
    while (path[i] && path[i] != '.' && path[i] != '[' && i + 1 < cap) {
        buf[i] = path[i];
        i++;
    }
    buf[i] = '\0';
    return buf;
}

/** Record a shared or exclusive borrow of `path`. 1 if already an error. */
static int take_borrow_path(const char *path, int is_mut, SourceLoc loc) {
    char rbuf[256];
    const char *root = root_of_path(path, rbuf, sizeof rbuf);
    Binding *v = find_b(root);
    if (!v) return 0;
    if (v->own == ST_MOVED) {
        loam_error(loc, "borrow of moved value '%s'", root);
        errn = 1;
        return 1;
    }
    for (Borrow *b = borrows; b; b = b->next) {
        if (!paths_conflict(b->path, path)) continue;
        if (!b->is_mut && !is_mut) continue;
        if (is_mut)
            loam_error(loc,
                       "cannot borrow '%s' as mutable more than once, or while shared-borrowed",
                       path);
        else
            loam_error(loc, "cannot borrow '%s' as shared while mutably borrowed", path);
        errn = 1;
        return 1;
    }
    Borrow *nb = (Borrow *)calloc(1, sizeof(Borrow));
    nb->path = loam_dup(path);
    nb->is_mut = is_mut;
    nb->depth = depth;
    nb->target = v;
    nb->next = borrows;
    borrows = nb;
    return 0;
}

/** Writing `path` (assignment, `++`) while a loan covers it. */
static int check_write_path(const char *path, SourceLoc loc) {
    for (Borrow *b = borrows; b; b = b->next)
        if (b->stored && paths_conflict(b->path, path)) {
            loam_error(loc, "cannot assign to '%s' while it is borrowed", path);
            errn = 1;
            return 1;
        }
    return 0;
}

/** Any live borrow overlapping `path` — a value cannot move out from under one. */
static int borrow_covers(const char *path) {
    for (Borrow *b = borrows; b; b = b->next)
        if (paths_conflict(b->path, path)) return 1;
    return 0;
}

/** Is `path` readable, or does a live exclusive borrow cover it? */
/** 1 if `path`, or a place containing it, was already consumed. */
static int path_is_moved(Binding *v, const char *path) {
    for (int i = 0; i < v->nmoved; i++)
        if (paths_conflict(v->moved_paths[i], path)) return 1;
    return 0;
}

/** Record `path` as consumed on `v`. */
static void mark_path_moved(Binding *v, const char *path) {
    if (path_is_moved(v, path)) return;
    v->moved_paths = (char **)realloc(v->moved_paths, (size_t)(v->nmoved + 1) * sizeof(char *));
    if (!v->moved_paths) return;
    v->moved_paths[v->nmoved++] = loam_dup(path);
}

static int check_use_path(const char *path, SourceLoc loc) {
    for (Borrow *b = borrows; b; b = b->next)
        if (b->is_mut && paths_conflict(b->path, path)) {
            loam_error(loc, "cannot use '%s' while mutably borrowed", path);
            errn = 1;
            return 1;
        }
    return 0;
}

static void drop_borrows_deeper_than(int d) {
    Borrow **pp = &borrows;
    while (*pp) {
        Borrow *b = *pp;
        if (b->depth > d) {
            *pp = b->next;
            free(b->path);
            free(b);
        } else {
            pp = &b->next;
        }
    }
}

/** Drop statement-temp borrows; keep those stored in live `let` bindings. */
static void release_temps(void) {
    Borrow **pp = &borrows;
    while (*pp) {
        Borrow *b = *pp;
        if (!b->stored) {
            *pp = b->next;
            free(b->path);
            free(b);
        } else {
            pp = &b->next;
        }
    }
}

static int expr_uses(AstNode *n, const char *name);
static int stmt_uses(AstNode *n, const char *name);

/** Does expression `n` mention identifier `name`? */
static int expr_uses(AstNode *n, const char *name) {
    if (!n) return 0;
    switch (n->kind) {
        case AST_IDENT:
            return strcmp(n->as.ident.name, name) == 0;
        case AST_ADDR:
        case AST_DEREF:
            return expr_uses(n->as.access.target, name);
        case AST_FIELD:
            return expr_uses(n->as.access.target, name);
        case AST_INDEX:
            return expr_uses(n->as.access.target, name) || expr_uses(n->as.access.index, name);
        case AST_BINARY:
            return expr_uses(n->as.binary.left, name) || expr_uses(n->as.binary.right, name);
        case AST_UNARY:
            return expr_uses(n->as.unary.operand, name);
        case AST_INCDEC:
            return expr_uses(n->as.incdec.operand, name);
        case AST_CAST:
            return expr_uses(n->as.cast.expr, name);
        case AST_TRY:
            return expr_uses(n->as.try_expr.expr, name);
        case AST_CLOSURE: {
            for (size_t i = 0; i < n->as.fn.cap_count; i++)
                if (strcmp(n->as.fn.caps[i], name) == 0) return 1;
            return stmt_uses(n->as.fn.body, name);
        }
        case AST_CALL:
            if (expr_uses(n->as.call.callee, name)) return 1;
            for (size_t i = 0; i < n->as.call.arg_count; i++)
                if (expr_uses(n->as.call.args[i], name)) return 1;
            return 0;
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++)
                if (expr_uses(n->as.struct_lit.fields[i].init, name)) return 1;
            return 0;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                if (expr_uses(n->as.array_lit.elems[i], name)) return 1;
            return 0;
        default:
            return 0;
    }
}

/** Does statement `n` (or anything nested in it) mention `name`? */
static int stmt_uses(AstNode *n, const char *name) {
    if (!n) return 0;
    switch (n->kind) {
        case AST_VAR_DECL:
            return expr_uses(n->as.var.init, name);
        case AST_ASSIGN:
            return expr_uses(n->as.assign.left, name) || expr_uses(n->as.assign.right, name);
        case AST_EXPR_STMT:
            return expr_uses(n->as.expr_stmt.expr, name);
        case AST_RETURN:
            return expr_uses(n->as.ret.expr, name);
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                if (stmt_uses(n->as.block.stmts[i], name)) return 1;
            return 0;
        case AST_IF:
            return expr_uses(n->as.if_stmt.cond, name) ||
                   stmt_uses(n->as.if_stmt.then_block, name) ||
                   stmt_uses(n->as.if_stmt.else_block, name);
        case AST_FOR:
            return expr_uses(n->as.for_stmt.iter, name) || stmt_uses(n->as.for_stmt.body, name);
        case AST_WHILE:
            return expr_uses(n->as.if_stmt.cond, name) || stmt_uses(n->as.if_stmt.then_block, name);
        case AST_SWITCH:
            if (expr_uses(n->as.switch_stmt.scrut, name)) return 1;
            for (size_t i = 0; i < n->as.switch_stmt.case_count; i++)
                if (stmt_uses(n->as.switch_stmt.cases[i], name)) return 1;
            return 0;
        case AST_CASE:
            return stmt_uses(n->as.switch_case.body, name);
        default:
            return 0;
    }
}

static void unlink_borrows_if(int (*pred)(Borrow *, Binding *), Binding *who) {
    Borrow **pp = &borrows;
    while (*pp) {
        Borrow *b = *pp;
        if (pred(b, who)) {
            *pp = b->next;
            free(b->path);
            free(b);
        } else {
            pp = &b->next;
        }
    }
}

static int owned_by(Borrow *b, Binding *who) { return b->stored && b->owner == who; }
static int rooted_in(Borrow *b, Binding *who) { return b->target == who; }

/** End every loan `b` holds. */
static void release_owned(Binding *b) { unlink_borrows_if(owned_by, b); }

/** `b` is gone: no place rooted in it can still be borrowed. */
static void drop_borrows_of(Binding *b) { unlink_borrows_if(rooted_in, b); }

static int holds_loans(Binding *b) {
    for (Borrow *l = borrows; l; l = l->next)
        if (l->stored && l->owner == b) return 1;
    return 0;
}

/* --- Loan sets, saved across branches --- */

static Borrow *dup_borrows(const Borrow *l) {
    Borrow *head = NULL, **tail = &head;
    for (; l; l = l->next) {
        Borrow *c = (Borrow *)malloc(sizeof(Borrow));
        *c = *l;
        c->path = loam_dup(l->path);
        c->next = NULL;
        *tail = c;
        tail = &c->next;
    }
    return head;
}

static void free_borrows(Borrow *l) {
    while (l) {
        Borrow *n = l->next;
        free(l->path);
        free(l);
        l = n;
    }
}

/** Add to `*dst` every loan of `other` it lacks (join after a branch).
 *  `temps`: statement temps too (an `if` expression's arms are mid-statement). */
static void union_into_ex(Borrow **dst, const Borrow *other, int temps) {
    for (; other; other = other->next) {
        if (!other->stored && !temps) continue;
        int have = 0;
        for (Borrow *b = *dst; b && !have; b = b->next)
            have = b->stored && b->owner == other->owner && b->is_mut == other->is_mut &&
                   strcmp(b->path, other->path) == 0;
        if (have) continue;
        Borrow *c = (Borrow *)malloc(sizeof(Borrow));
        *c = *other;
        c->path = loam_dup(other->path);
        c->next = *dst;
        *dst = c;
    }
}

static void union_into(Borrow **dst, const Borrow *other) { union_into_ex(dst, other, 0); }
static void union_borrows(const Borrow *other) { union_into(&borrows, other); }

static void set_borrows(const Borrow *l) {
    free_borrows(borrows);
    borrows = dup_borrows(l);
}

/**
 * NLL: end the borrow held by any binding of this block whose last use is
 * before statement `at`. The borrow is dead after its holder's final read, so
 * a later use of the borrowed place is legal (unlike a scope-length borrow).
 */
static void end_dead_borrows(AstNode **stmts, size_t count, size_t at, int block_depth) {
    for (Binding *b = binds; b; b = b->next) {
        if (b->depth != block_depth || !holds_loans(b)) continue;
        int used = 0;
        for (size_t j = at; j < count && !used; j++)
            if (stmt_uses(stmts[j], b->name)) used = 1;
        if (used) continue;
        release_owned(b);
    }
}

static int check_expr(AstNode *n, int as_move);
static int check_stmt(AstNode *n);

static const char *TEMP_ROOT = "<temporary>";

static int is_ref_ty(const Type *t) { return t && t->kind == TY_PTR; }

/** The deepest frame binding a place or reference may live in or point at.
 *  depth 0 = caller or global data; `name` is AST-owned or TEMP_ROOT. */
typedef struct {
    int depth;
    const char *name;
} Root;

static const Root NO_ROOT = { 0, NULL };

static Root deeper(Root a, Root b) { return b.depth > a.depth ? b : a; }

/* What an `if` expression's value points into, recorded while its arms'
   bindings were live (so `if c { let y = 1; &y } else { a }` sees `y`). */
typedef struct {
    AstNode *n;
    Root r;
} IfRoot;
static IfRoot *if_roots;
static int n_if_roots, cap_if_roots;

static void set_if_root(AstNode *n, Root r) {
    for (int i = 0; i < n_if_roots; i++)
        if (if_roots[i].n == n) {
            if_roots[i].r = r;
            return;
        }
    if (n_if_roots == cap_if_roots) {
        cap_if_roots = cap_if_roots ? cap_if_roots * 2 : 16;
        if_roots = (IfRoot *)realloc(if_roots, (size_t)cap_if_roots * sizeof(IfRoot));
        if (!if_roots) loam_fatal("out of memory");
    }
    if_roots[n_if_roots].n = n;
    if_roots[n_if_roots].r = r;
    n_if_roots++;
}

static Root get_if_root(AstNode *n) {
    for (int i = 0; i < n_if_roots; i++)
        if (if_roots[i].n == n) return if_roots[i].r;
    return NO_ROOT;
}

/** The value expression an `if` arm ends in, or NULL. */
static AstNode *arm_tail(AstNode *blk) {
    if (!blk || blk->kind != AST_BLOCK || !blk->as.block.stmt_count) return NULL;
    AstNode *last = blk->as.block.stmts[blk->as.block.stmt_count - 1];
    return last && last->kind == AST_EXPR_STMT ? last->as.expr_stmt.expr : NULL;
}

static Root ref_root(AstNode *e);

/**
 * Where place `n` lives. A place reached through a reference lives wherever
 * that reference points; a place that is not a binding at all (`f().x`, a
 * struct literal) is a temporary of the current scope.
 */
static Root place_root(AstNode *n) {
    if (!n) return NO_ROOT;
    switch (n->kind) {
        case AST_IDENT: {
            Binding *b = find_b(n->as.ident.name);
            if (!b || b->kind == BK_GLOBAL) return NO_ROOT;
            Root r = { b->depth, n->as.ident.name };
            return r;
        }
        case AST_FIELD:
        case AST_INDEX: {
            AstNode *t = n->as.access.target;
            /* `p.f` / `xs[i]` through a reference auto-derefs. */
            if (t && is_ref_ty(t->ty)) return ref_root(t);
            /* `mod.g`: a module-qualified global, never a binding. */
            if (n->kind == AST_FIELD && t && t->kind == AST_IDENT && !find_b(t->as.ident.name))
                return NO_ROOT;
            return place_root(t);
        }
        case AST_DEREF: {
            AstNode *t = n->as.access.target;
            if (t && is_ref_ty(t->ty)) return ref_root(t);
            /* `*b` of a Box: the heap value dies with b's owner. */
            return place_root(t);
        }
        default: {
            Root r = { depth, TEMP_ROOT };
            return r;
        }
    }
}

/**
 * What an expression of reference type points into. A call returning a
 * reference borrows from every reference argument, so `first(&local)` points
 * into `local`.
 */
static Root ref_root(AstNode *e) {
    if (!e) return NO_ROOT;
    switch (e->kind) {
        case AST_ADDR:
            return place_root(e->as.access.target);
        case AST_IDENT: {
            Binding *b = find_b(e->as.ident.name);
            if (!b || b->kind == BK_GLOBAL || !b->ref_depth) return NO_ROOT;
            Root r = { b->ref_depth, b->ref_root };
            return r;
        }
        case AST_CALL: {
            Root r = NO_ROOT;
            for (size_t i = 0; i < e->as.call.arg_count; i++) {
                AstNode *a = e->as.call.args[i];
                if (a && is_ref_ty(a->ty)) r = deeper(r, ref_root(a));
            }
            return r;
        }
        case AST_IF:
            return get_if_root(e);
        default:
            return NO_ROOT;
    }
}

static void set_ref(Binding *b, Root r) {
    b->ref_depth = r.depth;
    b->ref_root = r.name;
    b->dangling = 0;
}

/** Reading a reference whose target's scope has ended. */
static int check_dangling(Binding *b, SourceLoc loc) {
    if (!b || !b->dangling) return 0;
    if (b->ref_root == TEMP_ROOT)
        loam_error(loc, "'%s' is used after the temporary it borrows was dropped", b->name);
    else
        loam_error(loc, "'%s' is used after '%s', which it borrows, went out of scope", b->name,
                   b->ref_root ? b->ref_root : "?");
    errn = 1;
    return 1;
}

/* --- Loans a reference takes on --- */

typedef struct {
    Borrow *temp;   /* a statement temp to keep, or NULL */
    char *path;     /* otherwise: a copy of another binding's loan */
    int is_mut;
    Binding *target;
} Loan;

typedef struct {
    Loan *v;
    int n, cap;
} Loans;

static void loans_add(Loans *ls, Loan l) {
    if (ls->n == ls->cap) {
        ls->cap = ls->cap ? ls->cap * 2 : 4;
        ls->v = (Loan *)realloc(ls->v, (size_t)ls->cap * sizeof(Loan));
        if (!ls->v) loam_fatal("out of memory");
    }
    ls->v[ls->n++] = l;
}

/** The unkept temp borrow of `path` made by this statement, or NULL. */
static Borrow *find_temp(const char *path) {
    for (Borrow *b = borrows; b; b = b->next)
        if (!b->stored && strcmp(b->path, path) == 0) return b;
    return NULL;
}

/**
 * The loans a reference made from `e` must hold: the place of `&place`; every
 * loan of a reference it copies, plus a reborrow of that reference itself;
 * and, for a call returning a reference, those of each reference argument.
 * `result_mut`: the reference is `&mut` (a `&T` keeps only shared loans).
 */
static int gather_loans(AstNode *e, int result_mut, Loans *ls, SourceLoc loc) {
    if (!e) return 0;
    switch (e->kind) {
        case AST_ADDR: {
            char *p = path_of(e->as.access.target);
            if (!p) return 0;
            Borrow *t = find_temp(p);
            free(p);
            if (t) {
                Loan l = { t, NULL, t->is_mut && result_mut, NULL };
                loans_add(ls, l);
            }
            return 0;
        }
        case AST_IDENT: {
            Binding *q = find_b(e->as.ident.name);
            if (!q || q->kind == BK_GLOBAL || !is_ref_ty(e->ty)) return 0;
            for (Borrow *b = borrows; b; b = b->next) {
                if (!b->stored || b->owner != q) continue;
                Loan l = { NULL, loam_dup(b->path), b->is_mut && result_mut, b->target };
                loans_add(ls, l);
            }
            /* Through `q` itself: while the result lives, `q` may not be
               used to write what it points at. */
            int m = result_mut && e->ty->is_mut;
            if (take_borrow_path(e->as.ident.name, m, loc)) return 1;
            Loan l = { borrows, NULL, m, NULL };
            loans_add(ls, l);
            return 0;
        }
        case AST_CALL:
            if (!is_ref_ty(e->ty)) return 0;
            for (size_t i = 0; i < e->as.call.arg_count; i++) {
                AstNode *a = e->as.call.args[i];
                if (a && is_ref_ty(a->ty) && gather_loans(a, result_mut, ls, loc)) return 1;
            }
            return 0;
        case AST_IF:
            /* Either arm's value may be the result. */
            if (gather_loans(arm_tail(e->as.if_stmt.then_block), result_mut, ls, loc)) return 1;
            return gather_loans(arm_tail(e->as.if_stmt.else_block), result_mut, ls, loc);
        default:
            return 0;
    }
}

static void loans_free(Loans *ls) {
    for (int i = 0; i < ls->n; i++) free(ls->v[i].path);
    free(ls->v);
    ls->v = NULL;
    ls->n = ls->cap = 0;
}

/** Hand `ls` to `b`. `replace`: drop the loans `b` held before. */
static void attach_loans(Binding *b, Loans *ls, int replace) {
    if (replace) release_owned(b);
    for (int i = 0; i < ls->n; i++) {
        Loan *l = &ls->v[i];
        if (l->temp) {
            l->temp->stored = 1;
            l->temp->owner = b;
            l->temp->depth = b->depth;
            l->temp->is_mut = l->is_mut;
            continue;
        }
        Borrow *c = (Borrow *)calloc(1, sizeof(Borrow));
        c->path = l->path;
        l->path = NULL;
        c->is_mut = l->is_mut;
        c->stored = 1;
        c->depth = b->depth;
        c->owner = b;
        c->target = l->target;
        c->next = borrows;
        borrows = c;
    }
    loans_free(ls);
}

/**
 * Ownership state of every live binding, so a branch can be checked from the
 * state its sibling started in rather than the one it left behind.
 *
 * Binding pointers stay valid across a snapshot: a block pushes its own
 * bindings at a deeper depth and pops them before returning, so everything
 * recorded here outlives the branch that was checked.
 */
typedef struct {
    Binding *b;
    Own own;
    int shared;
    int mutb;
    int ref_depth;
    const char *ref_root;
    int dangling;
} BSnap;

static int snapshot(BSnap **out) {
    int n = 0;
    for (Binding *b = binds; b; b = b->next) n++;
    BSnap *s = n ? (BSnap *)calloc((size_t)n, sizeof(BSnap)) : NULL;
    int i = 0;
    for (Binding *b = binds; b; b = b->next) {
        s[i].b = b;
        s[i].own = b->own;
        s[i].shared = b->shared;
        s[i].mutb = b->mutb;
        s[i].ref_depth = b->ref_depth;
        s[i].ref_root = b->ref_root;
        s[i].dangling = b->dangling;
        i++;
    }
    *out = s;
    return n;
}

static void restore(BSnap *s, int n) {
    for (int i = 0; i < n; i++) {
        s[i].b->own = s[i].own;
        s[i].b->shared = s[i].shared;
        s[i].b->mutb = s[i].mutb;
        s[i].b->ref_depth = s[i].ref_depth;
        s[i].b->ref_root = s[i].ref_root;
        s[i].b->dangling = s[i].dangling;
    }
}

/** A reference may point at what it pointed at on either path. */
static void join_refs(BSnap *s, int n) {
    for (int i = 0; i < n; i++) {
        Binding *b = s[i].b;
        if (s[i].ref_depth > b->ref_depth) {
            b->ref_depth = s[i].ref_depth;
            b->ref_root = s[i].ref_root;
        }
        if (s[i].dangling) b->dangling = 1;
    }
}

/** Moved on either path is moved after the join. */
static void join_moved(BSnap *s, int n) {
    for (int i = 0; i < n; i++)
        if (s[i].own == ST_MOVED) s[i].b->own = ST_MOVED;
}

/** 1 unless `n` always leaves its block. Conservative: a `panic` call counts
 *  as continuing, which can only report a move, never hide one. */
static int stmt_continues(const AstNode *n) {
    if (!n) return 1;
    switch (n->kind) {
        case AST_RETURN:
        case AST_BREAK:
        case AST_CONTINUE:
            return 0;
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                if (!stmt_continues(n->as.block.stmts[i])) return 0;
            return 1;
        case AST_IF:
            return !n->as.if_stmt.else_block || stmt_continues(n->as.if_stmt.then_block) ||
                   stmt_continues(n->as.if_stmt.else_block);
        default:
            return 1;
    }
}

/** 1 if running case `cs` can reach the next case. */
static int case_falls_through(const AstNode *cs) {
    return !cs || cs->kind != AST_CASE || stmt_continues(cs->as.switch_case.body);
}

/**
 * One arm of an `if` expression: its statements, then its value. What the
 * value points into is folded into `acc` while the arm's bindings are live.
 */
static int check_value_arm(AstNode *blk, int as_move, Root *acc) {
    if (!blk) return 0;
    if (blk->kind != AST_BLOCK) return check_expr(blk, as_move);
    depth++;
    size_t cnt = blk->as.block.stmt_count;
    int rc = 0;
    for (size_t i = 0; i < cnt && !rc; i++) {
        end_dead_borrows(blk->as.block.stmts, cnt, i, depth);
        AstNode *st = blk->as.block.stmts[i];
        if (i + 1 == cnt && st && st->kind == AST_EXPR_STMT) {
            AstNode *e = st->as.expr_stmt.expr;
            Borrow *before = borrows;
            rc = check_expr(e, as_move);
            /* Borrows the value makes belong to the enclosing statement and
               outlive the arm (unless they root in the arm's own locals,
               which drop_borrows_of removes at the pop). */
            for (Borrow *b = borrows; b && b != before; b = b->next)
                if (!b->stored) b->depth = depth - 1;
            if (!rc && e && is_ref_ty(e->ty)) *acc = deeper(*acc, ref_root(e));
        } else {
            rc = check_stmt(st);
        }
    }
    pop_to_depth(depth - 1);
    depth--;
    return rc;
}

/** Walk an expression. `as_move` if this use consumes a non-Copy value. */
static int check_expr(AstNode *n, int as_move) {
    if (!n) return 0;
    switch (n->kind) {
        case AST_IDENT: {
            Binding *v = find_b(n->as.ident.name);
            if (!v) return 0;
            if (check_dangling(v, n->loc)) return 1;
            if (v->must_check) v->checked = 1;
            if (v->own == ST_MOVED) {
                loam_error(n->loc, "use of moved value '%s'", n->as.ident.name);
                errn = 1;
                return 1;
            }
            if (check_use_path(n->as.ident.name, n->loc)) return 1;
            if (as_move && !v->is_copy) {
                if (borrow_covers(n->as.ident.name)) {
                    loam_error(n->loc, "cannot move '%s' while borrowed", n->as.ident.name);
                    errn = 1;
                    return 1;
                }
                v->own = ST_MOVED;
                n->flags |= ASTF_MOVED;
            }
            return 0;
        }
        case AST_ADDR: {
            char *p = path_of(n->as.access.target);
            if (p) {
                char rb[256];
                if (check_dangling(find_b(root_of_path(p, rb, sizeof rb)), n->loc)) {
                    free(p);
                    return 1;
                }
                int rc = take_borrow_path(p, n->as.access.is_mut, n->loc);
                free(p);
                return rc;
            }
            return check_expr(n->as.access.target, 0);
        }
        case AST_DEREF:
            return check_expr(n->as.access.target, 0);
        case AST_FIELD: {
            /* Checked as a whole place, not by recursing to the root: reading
               p.b must stay legal while p.a is mutably borrowed. */
            char *p = path_of(n);
            if (!p) return check_expr(n->as.access.target, 0);
            char rbuf[256];
            Binding *v = find_b(root_of_path(p, rbuf, sizeof rbuf));
            if (v && v->must_check) v->checked = 1;
            int rc = 0;
            if (check_dangling(v, n->loc)) {
                rc = 1;
            } else if (v && v->own == ST_MOVED) {
                loam_error(n->loc, "use of moved value '%s'", rbuf);
                errn = 1;
                rc = 1;
            } else if (v && path_is_moved(v, p)) {
                loam_error(n->loc, "use of moved value '%s'", p);
                errn = 1;
                rc = 1;
            } else {
                rc = check_use_path(p, n->loc);
                if (!rc && v && as_move && n->ty && !type_is_copy(n->ty))
                    mark_path_moved(v, p);
            }
            free(p);
            return rc;
        }
        case AST_INDEX:
            if (check_expr(n->as.access.target, 0)) return 1;
            return check_expr(n->as.access.index, 0);
        case AST_IF: {
            /* An `if` expression: arms checked like a statement `if`, each
               from the state before it, joined after. Temps made in an arm's
               value belong to the enclosing statement, so they join too. */
            if (check_expr(n->as.if_stmt.cond, 0)) return 1;
            BSnap *pre = NULL, *post_then = NULL;
            int npre = snapshot(&pre), nthen = 0;
            Borrow *pre_loans = dup_borrows(borrows), *then_loans = NULL;
            Root acc = NO_ROOT;
            int rc = check_value_arm(n->as.if_stmt.then_block, as_move, &acc);
            if (!rc) {
                nthen = snapshot(&post_then);
                then_loans = dup_borrows(borrows);
                restore(pre, npre);
                set_borrows(pre_loans);
                rc = check_value_arm(n->as.if_stmt.else_block, as_move, &acc);
                if (!rc) {
                    join_moved(post_then, nthen);
                    join_refs(post_then, nthen);
                    union_into_ex(&borrows, then_loans, 1);
                }
            }
            free_borrows(pre_loans);
            free_borrows(then_loans);
            free(pre);
            free(post_then);
            set_if_root(n, acc);
            return rc;
        }
        case AST_BINARY:
            if (check_expr(n->as.binary.left, 0)) return 1;
            return check_expr(n->as.binary.right, 0);
        case AST_UNARY:
            return check_expr(n->as.unary.operand, 0);
        case AST_INCDEC: {
            /* Reads and writes the operand place (like an assignment). */
            if (check_expr(n->as.incdec.operand, 0)) return 1;
            char *wp = path_of(n->as.incdec.operand);
            int rc = wp ? check_write_path(wp, n->loc) : 0;
            free(wp);
            return rc;
        }
        case AST_CAST:
            return check_expr(n->as.cast.expr, 0);
        case AST_TRY:
            /* `expr?` reads the container and moves its payload out on the
               success path, but the failure path returns it whole. Treat it as
               a read of the operand (the container is never moved by `?`). */
            return check_expr(n->as.try_expr.expr, 0);
        case AST_CLOSURE: {
            for (size_t i = 0; i < n->as.fn.cap_count; i++) {
                Binding *v = find_b(n->as.fn.caps[i]);
                if (v && v->own == ST_MOVED) {
                    loam_error(n->loc, "use of moved value '%s'", n->as.fn.caps[i]);
                    errn = 1;
                    return 1;
                }
                /* A non-Copy capture is *moved* into the closure env: the
                   source is consumed here, so a later read of it is a
                   use-after-move and the codegen steals the value in. */
                if (v && !v->is_copy) {
                    if (borrow_covers(n->as.fn.caps[i])) {
                        loam_error(n->loc, "cannot move '%s' into a closure while borrowed",
                                   n->as.fn.caps[i]);
                        errn = 1;
                        return 1;
                    }
                    v->own = ST_MOVED;
                }
            }
            depth++;
            Type *ft = n->ty;
            for (size_t k = 0; k < n->as.fn.param_count; k++) {
                Type *pt = (ft && k < ft->param_count) ? ft->params[k] : NULL;
                push_b(n->as.fn.params[k].name, pt);
                binds->kind = BK_PARAM;
            }
            /* The closure body reads its *captures*, not the outer bindings
               (a moved capture's source is consumed here). Shadow each capture
               with its own binding so a body read is not a use-after-move of
               the source. */
            for (size_t k = 0; k < n->as.fn.cap_count; k++) {
                Type *ct = n->as.fn.cap_types ? n->as.fn.cap_types[k] : NULL;
                push_b(n->as.fn.caps[k], ct);
            }
            if (n->as.fn.body && check_stmt(n->as.fn.body)) {
                pop_to_depth(depth - 1);
                depth--;
                return 1;
            }
            pop_to_depth(depth - 1);
            depth--;
            return 0;
        }
        case AST_CALL: {
            int mv = n->as.call.is_box_new ? 0 : 0;
            (void)mv;
            for (size_t i = 0; i < n->as.call.arg_count; i++) {
                AstNode *a = n->as.call.args[i];
                int move_arg = 0;
                if (a && a->kind != AST_ADDR && a->ty && !type_is_copy(a->ty) &&
                    a->ty->kind != TY_PTR)
                    move_arg = 1;
                if (n->as.call.is_println || n->as.call.is_box_new) move_arg = 0;
                if (n->as.call.value_key) move_arg = 0;
                if (n->as.call.is_box_new) {
                    /* move non-copy into the box */
                    if (a->ty && !type_is_copy(a->ty)) move_arg = 1;
                }
                if (check_expr(a, move_arg)) return 1;
            }
            /* A `&mut` reference handed on is reborrowed mutably at the call,
               after every argument is read (`grow(m, m.cap)` is fine): it
               conflicts with a live loan through it, or a second `&mut`. */
            Type *cft = n->as.call.callee ? n->as.call.callee->ty : NULL;
            if (cft && cft->kind != TY_PROC) cft = NULL;
            Borrow *before_reborrows = borrows;
            for (size_t i = 0; i < n->as.call.arg_count; i++) {
                AstNode *a = n->as.call.args[i];
                /* Only into a `&mut` parameter: a `&` one reborrows shared. */
                Type *pt = cft && i < cft->param_count ? cft->params[i] : NULL;
                if (pt && !(is_ref_ty(pt) && pt->is_mut)) continue;
                if (a && a->kind == AST_IDENT && is_ref_ty(a->ty) && a->ty->is_mut &&
                    find_b(a->as.ident.name) &&
                    take_borrow_path(a->as.ident.name, 1, a->loc))
                    return 1;
            }
            /* They last for the call only: a result that keeps one gets its
               own loan from gather_loans. */
            while (borrows && borrows != before_reborrows) {
                Borrow *b = borrows;
                borrows = b->next;
                free(b->path);
                free(b);
            }
            if (n->as.call.callee && n->as.call.callee->kind != AST_FIELD)
                check_expr(n->as.call.callee, 0);
            /* `aim(&mut p, &v)` with `p` a reference: the callee may store any
               of its other reference arguments in `p`. */
            for (size_t i = 0; i < n->as.call.arg_count; i++) {
                AstNode *a = n->as.call.args[i];
                if (!a || a->kind != AST_ADDR || !a->as.access.is_mut) continue;
                AstNode *t = a->as.access.target;
                if (!t || t->kind != AST_IDENT || !is_ref_ty(t->ty)) continue;
                Binding *b = find_b(t->as.ident.name);
                if (!b || b->kind == BK_GLOBAL) continue;
                Root r = { b->ref_depth, b->ref_root };
                Loans ls = { 0 };
                for (size_t k = 0; k < n->as.call.arg_count; k++) {
                    AstNode *o = n->as.call.args[k];
                    if (k == i || !o || !is_ref_ty(o->ty)) continue;
                    r = deeper(r, ref_root(o));
                    if (gather_loans(o, t->ty->is_mut, &ls, o->loc)) {
                        loans_free(&ls);
                        return 1;
                    }
                }
                b->ref_depth = r.depth;
                b->ref_root = r.name;
                /* What `p` may now point at stays borrowed while `p` lives. */
                attach_loans(b, &ls, 0);
            }
            return 0;
        }
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++) {
                AstNode *init = n->as.struct_lit.fields[i].init;
                int mv = init && init->ty && !type_is_copy(init->ty);
                if (check_expr(init, mv)) return 1;
            }
            return 0;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                if (check_expr(n->as.array_lit.elems[i], 0)) return 1;
            return 0;
        default:
            return 0;
    }
}

static int check_stmt(AstNode *n) {
    if (!n) return 0;
    switch (n->kind) {
        case AST_VAR_DECL: {
            int mv = 0;
            if (n->as.var.init && n->as.var.init->ty && !type_is_copy(n->as.var.init->ty) &&
                n->as.var.init->kind != AST_ADDR && n->as.var.init->kind != AST_CALL)
                mv = 1;
            /* Box::new constructs a new owner; struct lit is a new value */
            if (n->as.var.init && n->as.var.init->kind == AST_CALL &&
                n->as.var.init->as.call.is_box_new)
                mv = 0;
            if (n->as.var.init && n->as.var.init->kind == AST_STRUCT_LIT) mv = 0;
            if (n->as.var.init && n->as.var.init->kind == AST_ARRAY_LIT) mv = 0;
            if (n->as.var.init && n->as.var.init->kind == AST_CLOSURE) mv = 0;
            if (n->as.var.init && check_expr(n->as.var.init, mv)) return 1;
            /* Resolved before the push, so `let x = &x` sees the outer x. */
            int is_ref = is_ref_ty(n->ty) && n->as.var.init;
            Root rr = is_ref ? ref_root(n->as.var.init) : NO_ROOT;
            Loans ls = { 0 };
            if (is_ref && gather_loans(n->as.var.init, n->ty->is_mut, &ls, n->loc)) {
                loans_free(&ls);
                return 1;
            }
            push_b(n->as.var.name, n->ty);
            set_ref(binds, rr);
            /* Aimed at a binding already out of scope (an `if` arm's local). */
            if (rr.depth > depth) binds->dangling = 1;
            /* The loans outlive the statement and end at the binding's last
               use (end_dead_borrows) or its scope. */
            attach_loans(binds, &ls, 1);
            if (n->ty && n->ty->kind == TY_STRUCT && n->ty->must_check) {
                binds->must_check = 1;
                binds->loc = n->loc;
            }
            if (type_needs_drop(n->ty)) n->flags |= ASTF_NEEDS_DROP;
            release_temps();
            return 0;
        }
        case AST_ASSIGN: {
            AstNode *L = n->as.assign.left;
            AstNode *R = n->as.assign.right;
            int stores_ref = L && is_ref_ty(L->ty);
            Binding *lb = L && L->kind == AST_IDENT ? find_b(L->as.ident.name) : NULL;
            /* Rebinding a local reference: its old target no longer matters. */
            int rebind = stores_ref && lb && lb->kind != BK_GLOBAL;
            /* `p = &mut x` again: the old loan ends before the new one starts,
               unless the right side reads `p`. */
            if (rebind && !expr_uses(R, lb->name)) release_owned(lb);
            if (check_expr(R, R->ty && !type_is_copy(R->ty) && R->kind != AST_ADDR)) return 1;
            Root rr = stores_ref ? ref_root(R) : NO_ROOT;
            Loans ls = { 0 };
            if (stores_ref && gather_loans(R, L->ty->is_mut, &ls, n->loc)) {
                loans_free(&ls);
                return 1;
            }
            if (rebind) lb->dangling = 0;
            if (check_expr(L, 0)) {
                loans_free(&ls);
                return 1;
            }
            if (!rebind) {
                char *wp = path_of(L);
                int bad = wp && check_write_path(wp, n->loc);
                free(wp);
                if (bad) {
                    loans_free(&ls);
                    return 1;
                }
            }
            if (rebind) {
                set_ref(lb, rr);
                if (rr.depth > depth) lb->dangling = 1;
                attach_loans(lb, &ls, 1);
            } else if (stores_ref) {
                /* `*pp = &x`, or a global: the place is not tracked, so what
                   it receives must outlive it now. */
                Root holder = place_root(L);
                if (rr.depth > holder.depth) {
                    if (rr.name == TEMP_ROOT)
                        loam_error(n->loc, "a borrow of a temporary cannot be stored here");
                    else
                        loam_error(n->loc,
                                   "'%s' does not live long enough to be stored here",
                                   rr.name ? rr.name : "?");
                    errn = 1;
                    return 1;
                }
                /* `*pp = &x` with `pp` aimed at a local reference `q`: `q`
                   holds the loan now. */
                Root h = place_root(L);
                Binding *q = h.name && h.name != TEMP_ROOT ? find_b(h.name) : NULL;
                if (q && q->depth == h.depth && q->kind != BK_GLOBAL) {
                    if (rr.depth > q->ref_depth) {
                        q->ref_depth = rr.depth;
                        q->ref_root = rr.name;
                    }
                    attach_loans(q, &ls, 0);
                }
            }
            loans_free(&ls);
            release_temps();
            return 0;
        }
        case AST_EXPR_STMT:
            if (n->as.expr_stmt.expr) {
                Type *et = n->as.expr_stmt.expr->ty;
                if (et && et->kind == TY_STRUCT && et->must_check) {
                    loam_error(n->loc,
                               "unused `#[must_check]` value of type '%s' — read a "
                               "field or use `.or()` / `.or_trap()`",
                               et->name);
                    errn = 1;
                }
            }
            if (check_expr(n->as.expr_stmt.expr, 0)) return 1;
            release_temps();
            return 0;
        case AST_RETURN:
            if (n->as.ret.expr && check_expr(n->as.ret.expr,
                    n->as.ret.expr->ty && !type_is_copy(n->as.ret.expr->ty) &&
                    n->as.ret.expr->kind != AST_ADDR))
                return 1;
            if (n->as.ret.expr && is_ref_ty(n->as.ret.expr->ty)) {
                Root r = ref_root(n->as.ret.expr);
                if (r.depth > 0 && r.name == TEMP_ROOT) {
                    loam_error(n->loc, "cannot return borrow of a temporary");
                    errn = 1;
                    return 1;
                }
                if (r.depth > 0) {
                    loam_error(n->loc, "cannot return borrow of local '%s'",
                               r.name ? r.name : "?");
                    errn = 1;
                    return 1;
                }
            }
            release_temps();
            return 0;
        case AST_BLOCK: {
            depth++;
            for (size_t i = 0; i < n->as.block.stmt_count; i++) {
                end_dead_borrows(n->as.block.stmts, n->as.block.stmt_count, i, depth);
                if (check_stmt(n->as.block.stmts[i])) {
                    pop_to_depth(depth - 1);
                    depth--;
                    return 1;
                }
            }
            pop_to_depth(depth - 1);
            depth--;
            return 0;
        }
        case AST_IF: {
            if (check_expr(n->as.if_stmt.cond, 0)) return 1;
            release_temps();
            /* Each arm starts from the state before the branch: consuming the
               same value in both is one move, not two. */
            BSnap *pre = NULL, *post_then = NULL;
            int npre = snapshot(&pre), nthen = 0;
            Borrow *pre_loans = dup_borrows(borrows), *then_loans = NULL;
            int rc = check_stmt(n->as.if_stmt.then_block);
            if (!rc) {
                nthen = snapshot(&post_then);
                then_loans = dup_borrows(borrows);
                restore(pre, npre);
                set_borrows(pre_loans);
                if (n->as.if_stmt.else_block) rc = check_stmt(n->as.if_stmt.else_block);
                if (!rc) {
                    join_moved(post_then, nthen);
                    join_refs(post_then, nthen);
                    union_borrows(then_loans);
                }
            }
            free_borrows(pre_loans);
            free_borrows(then_loans);
            free(pre);
            free(post_then);
            return rc;
        }
        case AST_FOR: {
            if (check_expr(n->as.for_stmt.iter, 0)) return 1;
            release_temps();
            depth++;
            push_b(n->as.for_stmt.var, ty_int());
            /* Twice: the second pass sees what the first left behind, so a move
               in the body is caught as a use-after-move on the next iteration.
               Two passes suffice — Own is a two-state monotone lattice, so a
               third could not change anything. */
            /* The body may run zero times: references join with the state
               before the loop, before the second pass and after it. */
            BSnap *pre = NULL;
            int npre = snapshot(&pre);
            int rc = check_stmt(n->as.for_stmt.body);
            if (!rc) {
                join_refs(pre, npre);
                rc = check_stmt(n->as.for_stmt.body);
            }
            if (!rc) join_refs(pre, npre);
            free(pre);
            pop_to_depth(depth - 1);
            depth--;
            return rc;
        }
        case AST_WHILE: {
            if (check_expr(n->as.if_stmt.cond, 0)) return 1;
            release_temps();
            BSnap *pre = NULL;
            int npre = snapshot(&pre);
            int rc = check_stmt(n->as.if_stmt.then_block);
            if (!rc) {
                join_refs(pre, npre);
                rc = check_stmt(n->as.if_stmt.then_block);
            }
            if (!rc) join_refs(pre, npre);
            free(pre);
            return rc;
        }
        case AST_SWITCH: {
            if (check_expr(n->as.switch_stmt.scrut, 0)) return 1;
            release_temps();
            BSnap *pre = NULL;
            int npre = snapshot(&pre);
            /* Loans: each case starts from those before the switch, plus the
               case above's by fallthrough; after it, any case's (or none). */
            Borrow *pre_loans = dup_borrows(borrows), *all_loans = dup_borrows(borrows);
            int rc = 0;
            BSnap **snaps = NULL;
            int *nsn = NULL;
            size_t na = n->as.switch_stmt.case_count;
            if (na) {
                snaps = (BSnap **)calloc(na, sizeof(BSnap *));
                nsn = (int *)calloc(na, sizeof(int));
            }
            for (size_t i = 0; i < na; i++) {
                /* A case is entered from the dispatch, and by falling out of
                   the case above: what that one moved is moved here too. */
                restore(pre, npre);
                int fell = i && case_falls_through(n->as.switch_stmt.cases[i - 1]);
                Borrow *above = fell ? dup_borrows(borrows) : NULL;
                set_borrows(pre_loans);
                union_borrows(above);
                free_borrows(above);
                if (fell) {
                    join_moved(snaps[i - 1], nsn[i - 1]);
                    join_refs(snaps[i - 1], nsn[i - 1]);
                }
                AstNode *cs = n->as.switch_stmt.cases[i];
                if (cs && cs->kind == AST_CASE)
                    rc = check_stmt(cs->as.switch_case.body);
                if (rc) break;
                union_into(&all_loans, borrows);
                nsn[i] = snapshot(&snaps[i]);
            }
            if (!rc) {
                for (size_t i = 0; i < na; i++) {
                    join_moved(snaps[i], nsn[i]);
                    join_refs(snaps[i], nsn[i]);
                }
                /* No case may match. */
                join_refs(pre, npre);
                set_borrows(all_loans);
            }
            free_borrows(pre_loans);
            free_borrows(all_loans);
            for (size_t i = 0; i < na; i++)
                if (snaps) free(snaps[i]);
            free(snaps);
            free(nsn);
            free(pre);
            return rc;
        }
        case AST_BREAK:
        case AST_CONTINUE:
            return 0;
        default:
            return 0;
    }
}

/** Check all non-intrinsic functions. Returns 1 if any error. */
int borrowck_modules(LoamModule *mods, int nmods) {
    errn = 0;
    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_FN_DECL || d->as.fn.is_intrinsic)
                continue;
            binds = NULL;
            borrows = NULL;
            depth = 0;
            depth = 1;
            for (int g = 0; g < typecheck_global_count(); g++) {
                if (typecheck_global_mod(g) != m) continue;
                AstNode *gv = typecheck_global_var(g);
                if (gv) {
                    push_b(gv->as.var.name, gv->ty);
                    binds->kind = BK_GLOBAL;
                }
            }
            Type *ft = d->ty;
            for (size_t k = 0; k < d->as.fn.param_count; k++) {
                Type *pt = (ft && k < ft->param_count) ? ft->params[k] : NULL;
                push_b(d->as.fn.params[k].name, pt);
                binds->kind = BK_PARAM;
            }
            if (d->as.fn.body) check_stmt(d->as.fn.body);
            pop_to_depth(0);
        }
    }
    return errn;
}
