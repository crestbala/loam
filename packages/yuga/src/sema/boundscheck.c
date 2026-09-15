/**
 * boundscheck.c — mark `a[k]` safe when k is a literal in 0..N for `[N]T`.
 *
 * Other indexes keep a run-time yuga_idx trap. Never fails the compile.
 */
#include "boundscheck.h"
#include "type.h"
#include "../diagnostics.h"

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
        case AST_FOR:
            walk(n->as.for_stmt.iter);
            walk(n->as.for_stmt.body);
            break;
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
int boundscheck_modules(YugaModule *mods, int nmods) {
    for (int i = 0; i < nmods; i++)
        if (mods[i].ast) walk(mods[i].ast);
    return 0;
}
