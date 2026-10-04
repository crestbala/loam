/**
 * boundscheck.c — mark `a[k]` safe when the compiler can prove k in range.
 *
 * Two proofs today:
 *   - `a[LITERAL]` with `0 <= LITERAL < N` for `[N]T` (and the vec/string
 *     equivalent, where the length is not a compile-time constant).
 *   - `for i in 0..C.len { ... C[i] ... }` — the loop variable is the index of
 *     the *same* container the range bounds, so `i` is in `[0, C.len)` on every
 *     iteration. This is the hot pattern (every elementwise loop), and it is
 *     the one a whole-program compile emits a `loam_idx` per iteration for.
 *
 * Everything else keeps a run-time trap. The pass never fails the compile: a
 * miss is a redundant check, never a wrong answer.
 */
#include "boundscheck.h"
#include "type.h"
#include "../diagnostics.h"
#include <string.h>

/** The identifier node a `<expr>.len` is rooted at, or NULL. */
static AstNode *len_container_ident(AstNode *n) {
    if (!n || n->kind != AST_FIELD) return NULL;
    if (!n->as.access.field || strcmp(n->as.access.field, "len") != 0) return NULL;
    AstNode *b = n->as.access.target;
    while (b && b->kind != AST_IDENT) {
        if (b->kind == AST_FIELD || b->kind == AST_INDEX || b->kind == AST_DEREF)
            b = b->as.access.target;
        else
            return NULL;
    }
    return b;
}

/** The identifier node a place expression is rooted at, or NULL. */
static AstNode *root_ident_node(AstNode *n) {
    while (n) {
        if (n->kind == AST_IDENT) return n;
        if (n->kind == AST_FIELD || n->kind == AST_INDEX || n->kind == AST_DEREF)
            n = n->as.access.target;
        else
            return NULL;
    }
    return NULL;
}

/** A `for`-loop in-range proof in force for the current subtree. `loop` is the
    `AST_FOR` node; `container` is the identifier whose `.len` bounds the loop.
    The index must *resolve* to `loop` (not a shadowing inner `i`), so the
    proof cannot be fooled by a `let i` inside the body. */
typedef struct {
    AstNode *loop;      /* the AST_FOR node the loop variable resolves to */
    AstNode *container; /* the container identifier the range's `.len` resolves from */
    struct BoundCtx *next;
} BoundCtx;

static BoundCtx *ctx_top;

/** 1 if `n` is exactly the loop variable of a proof in force for the
    container `target` names — both matched by resolution, so a shadowed `i` or
    a rebound `xs` does not match. */
static int is_proven_index(AstNode *n, AstNode *target) {
    if (!n || n->kind != AST_IDENT || !n->as.ident.resolved) return 0;
    AstNode *tn = root_ident_node(target);
    if (!tn || !tn->as.ident.resolved) return 0;
    for (BoundCtx *c = ctx_top; c; c = c->next) {
        if (c->loop && c->container && c->container->as.ident.resolved &&
            c->container->as.ident.resolved == tn->as.ident.resolved &&
            n->as.ident.resolved == c->loop)
            return 1;
    }
    return 0;
}

/** Depth-first walk; sets ASTF_INDEX_SAFE on proven-in-range indexes. */
static void walk(AstNode *n) {
    if (!n) return;
    switch (n->kind) {
        case AST_PROGRAM:
            for (size_t i = 0; i < n->as.program.decl_count; i++) walk(n->as.program.decls[i]);
            break;
        case AST_FN_DECL:
            walk(n->as.fn.body);
            break;
        case AST_CLOSURE:
            walk(n->as.fn.body);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++) walk(n->as.block.stmts[i]);
            break;
        case AST_IF:
            walk(n->as.if_stmt.cond);
            walk(n->as.if_stmt.then_block);
            walk(n->as.if_stmt.else_block);
            break;
        case AST_FOR: {
            walk(n->as.for_stmt.iter);
            /* `for i in 0..C.len` proves every `C[i]` in the body in range. */
            BoundCtx ctx;
            AstNode *it = n->as.for_stmt.iter;
            ctx.loop = n;
            ctx.container = NULL;
            ctx.next = ctx_top;
            if (it && it->kind == AST_BINARY && it->as.binary.op == TOK_DOT_DOT &&
                it->as.binary.left && it->as.binary.left->kind == AST_NUMBER &&
                it->as.binary.left->as.lit.value == 0) {
                AstNode *cont = len_container_ident(it->as.binary.right);
                if (cont && cont->as.ident.resolved) ctx.container = cont;
            }
            BoundCtx *saved = ctx_top;
            ctx_top = &ctx;
            walk(n->as.for_stmt.body);
            ctx_top = saved;
            break;
        }
        case AST_WHILE:
            walk(n->as.if_stmt.cond);
            walk(n->as.if_stmt.then_block);
            break;
        case AST_MATCH:
            walk(n->as.match_stmt.scrut);
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++)
                walk(n->as.match_stmt.arms[i]);
            break;
        case AST_MATCH_ARM:
            for (size_t i = 0; i < n->as.match_arm.pat_count; i++)
                walk(n->as.match_arm.pats[i]);
            walk(n->as.match_arm.body);
            break;
        case AST_RETURN:
            walk(n->as.ret.expr);
            break;
        case AST_EXPR_STMT:
            walk(n->as.expr_stmt.expr);
            break;
        case AST_VAR_DECL:
            walk(n->as.var.init);
            break;
        case AST_ASSIGN:
            walk(n->as.assign.left);
            walk(n->as.assign.right);
            break;
        case AST_BINARY:
            walk(n->as.binary.left);
            walk(n->as.binary.right);
            break;
        case AST_UNARY:
            walk(n->as.unary.operand);
            break;
        case AST_CAST:
            walk(n->as.cast.expr);
            break;
        case AST_TRY:
            walk(n->as.try_expr.expr);
            break;
        case AST_CALL:
            walk(n->as.call.callee);
            for (size_t i = 0; i < n->as.call.arg_count; i++) walk(n->as.call.args[i]);
            break;
        case AST_INDEX: {
            walk(n->as.access.target);
            walk(n->as.access.index);
            Type *t = n->as.access.target ? n->as.access.target->ty : NULL;
            if (t && (t->kind == TY_PTR || t->kind == TY_BOX)) t = t->elem;
            if (t && t->kind == TY_ARRAY && n->as.access.index &&
                n->as.access.index->kind == AST_NUMBER) {
                int64_t i = n->as.access.index->as.lit.value;
                if (i >= 0 && i < t->array_len) n->flags |= ASTF_INDEX_SAFE;
            }
            /* `for i in 0..C.len { ... C[i] ... }`: the index is the loop
               variable and the target is the same container C the range
               bounds, so `i` is in `[0, C.len)`. */
            if (!(n->flags & ASTF_INDEX_SAFE) && t &&
                (t->kind == TY_VEC || t->kind == TY_ARRAY || t->kind == TY_STRING)) {
                if (is_proven_index(n->as.access.index, n->as.access.target))
                    n->flags |= ASTF_INDEX_SAFE;
            }
            break;
        }
        case AST_FIELD:
        case AST_DEREF:
        case AST_ADDR:
            walk(n->as.access.target);
            walk(n->as.access.index);
            break;
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++)
                walk(n->as.struct_lit.fields[i].init);
            break;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++) walk(n->as.array_lit.elems[i]);
            break;
        default:
            break;
    }
}

/** Mark safe constant indexes. Always returns 0. */
int boundscheck_modules(LoamModule *mods, int nmods) {
    for (int i = 0; i < nmods; i++)
        if (mods[i].ast) walk(mods[i].ast);
    return 0;
}
