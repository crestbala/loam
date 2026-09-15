/**
 * borrowck.c — moves, shared vs exclusive borrows, no return of &local
 * (that last one is also rejected in typecheck for `return &ident`).
 *
 * Bindings are a stack. Temps from `&x` in a statement are cleared at
 * statement end unless stored in a `let`.
 */
#include "borrowck.h"
#include "typecheck.h"
#include "../diagnostics.h"
#include "type.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum { ST_OWNED, ST_MOVED } Own;

typedef struct Binding {
    char *name;
    Own own;
    int shared;
    int mutb;
    int is_copy;
    int is_box;
    int depth;
    char *borrow_of; /* place this binding holds a stored borrow of, or NULL */
    int borrow_mut;
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
    int stored; /* held by a live let binding, not a statement temp */
    int depth;
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
        free(b->name);
        free(b->borrow_of);
        for (int i = 0; i < b->nmoved; i++) free(b->moved_paths[i]);
        free(b->moved_paths);
        free(b);
    }
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
    nb->next = borrows;
    borrows = nb;
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
        case AST_MATCH:
            if (expr_uses(n->as.match_stmt.scrut, name)) return 1;
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++)
                if (stmt_uses(n->as.match_stmt.arms[i], name)) return 1;
            return 0;
        case AST_MATCH_ARM:
            return stmt_uses(n->as.match_arm.body, name);
        default:
            return 0;
    }
}

/** Drop the stored borrow of `path`, if still live. */
static void release_borrow_path(const char *path) {
    Borrow **pp = &borrows;
    while (*pp) {
        Borrow *b = *pp;
        if (b->stored && strcmp(b->path, path) == 0) {
            *pp = b->next;
            free(b->path);
            free(b);
        } else {
            pp = &b->next;
        }
    }
}

/**
 * NLL: end the borrow held by any binding of this block whose last use is
 * before statement `at`. The borrow is dead after its holder's final read, so
 * a later use of the borrowed place is legal (unlike a scope-length borrow).
 */
static void end_dead_borrows(AstNode **stmts, size_t count, size_t at, int block_depth) {
    for (Binding *b = binds; b; b = b->next) {
        if (b->depth != block_depth || !b->borrow_of) continue;
        int used = 0;
        for (size_t j = at; j < count && !used; j++)
            if (stmt_uses(stmts[j], b->name)) used = 1;
        if (used) continue;
        release_borrow_path(b->borrow_of);
        free(b->borrow_of);
        b->borrow_of = NULL;
    }
}

static int check_expr(AstNode *n, int as_move);
static int check_stmt(AstNode *n);

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
    }
}

/** Moved on either path is moved after the join. */
static void join_moved(BSnap *s, int n) {
    for (int i = 0; i < n; i++)
        if (s[i].own == ST_MOVED) s[i].b->own = ST_MOVED;
}

/** Walk an expression. `as_move` if this use consumes a non-Copy value. */
static int check_expr(AstNode *n, int as_move) {
    if (!n) return 0;
    switch (n->kind) {
        case AST_IDENT: {
            Binding *v = find_b(n->as.ident.name);
            if (!v) return 0;
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
            if (v && v->own == ST_MOVED) {
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
        case AST_BINARY:
            if (check_expr(n->as.binary.left, 0)) return 1;
            return check_expr(n->as.binary.right, 0);
        case AST_UNARY:
            return check_expr(n->as.unary.operand, 0);
        case AST_INCDEC:
            /* Reads and writes the operand place (like an assignment). */
            return check_expr(n->as.incdec.operand, 0);
        case AST_CAST:
            return check_expr(n->as.cast.expr, 0);
        case AST_CLOSURE: {
            for (size_t i = 0; i < n->as.fn.cap_count; i++) {
                Binding *v = find_b(n->as.fn.caps[i]);
                if (v && v->own == ST_MOVED) {
                    loam_error(n->loc, "use of moved value '%s'", n->as.fn.caps[i]);
                    errn = 1;
                    return 1;
                }
            }
            depth++;
            Type *ft = n->ty;
            for (size_t k = 0; k < n->as.fn.param_count; k++) {
                Type *pt = (ft && k < ft->param_count) ? ft->params[k] : NULL;
                push_b(n->as.fn.params[k].name, pt);
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
                if (n->as.call.is_box_new) {
                    /* move non-copy into the box */
                    if (a->ty && !type_is_copy(a->ty)) move_arg = 1;
                }
                if (check_expr(a, move_arg)) return 1;
            }
            if (n->as.call.callee && n->as.call.callee->kind != AST_FIELD)
                check_expr(n->as.call.callee, 0);
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
            push_b(n->as.var.name, n->ty);
            if (n->ty && n->ty->kind == TY_STRUCT && n->ty->must_check) {
                binds->must_check = 1;
                binds->loc = n->loc;
            }
            /* A borrow stored in a binding outlives the statement that made it,
               so it survives release_temps and dies with its scope instead. */
            if (n->as.var.init && n->as.var.init->kind == AST_ADDR && borrows) {
                borrows->stored = 1;
                borrows->depth = depth;
                char *bp = path_of(n->as.var.init->as.access.target);
                if (bp) {
                    free(binds->borrow_of);
                    binds->borrow_of = bp;
                }
            }
            if (type_needs_drop(n->ty)) n->flags |= ASTF_NEEDS_DROP;
            release_temps();
            return 0;
        }
        case AST_ASSIGN:
            if (check_expr(n->as.assign.right,
                           n->as.assign.right->ty && !type_is_copy(n->as.assign.right->ty) &&
                               n->as.assign.right->kind != AST_ADDR))
                return 1;
            if (check_expr(n->as.assign.left, 0)) return 1;
            release_temps();
            return 0;
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
            int rc = check_stmt(n->as.if_stmt.then_block);
            if (!rc) {
                nthen = snapshot(&post_then);
                restore(pre, npre);
                if (n->as.if_stmt.else_block) rc = check_stmt(n->as.if_stmt.else_block);
                if (!rc) join_moved(post_then, nthen);
            }
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
            int rc = check_stmt(n->as.for_stmt.body);
            if (!rc) rc = check_stmt(n->as.for_stmt.body);
            pop_to_depth(depth - 1);
            depth--;
            return rc;
        }
        case AST_WHILE: {
            if (check_expr(n->as.if_stmt.cond, 0)) return 1;
            release_temps();
            int rc = check_stmt(n->as.if_stmt.then_block);
            if (!rc) rc = check_stmt(n->as.if_stmt.then_block);
            return rc;
        }
        case AST_MATCH: {
            if (check_expr(n->as.match_stmt.scrut, 0)) return 1;
            release_temps();
            BSnap *pre = NULL;
            int npre = snapshot(&pre);
            int rc = 0;
            BSnap **snaps = NULL;
            int *nsn = NULL;
            size_t na = n->as.match_stmt.arm_count;
            if (na) {
                snaps = (BSnap **)calloc(na, sizeof(BSnap *));
                nsn = (int *)calloc(na, sizeof(int));
            }
            for (size_t i = 0; i < na; i++) {
                restore(pre, npre);
                AstNode *arm = n->as.match_stmt.arms[i];
                if (arm && arm->kind == AST_MATCH_ARM)
                    rc = check_stmt(arm->as.match_arm.body);
                if (rc) break;
                if (snaps) nsn[i] = snapshot(&snaps[i]);
            }
            if (!rc && na > 0) {
                for (size_t i = 0; i + 1 < na; i++)
                    if (snaps) join_moved(snaps[i], nsn[i]);
            }
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
                if (gv) push_b(gv->as.var.name, gv->ty);
            }
            Type *ft = d->ty;
            for (size_t k = 0; k < d->as.fn.param_count; k++) {
                Type *pt = (ft && k < ft->param_count) ? ft->params[k] : NULL;
                push_b(d->as.fn.params[k].name, pt);
            }
            if (d->as.fn.body) check_stmt(d->as.fn.body);
            pop_to_depth(0);
        }
    }
    return errn;
}
