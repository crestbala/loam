/**
 * ast.c — allocate and free AST nodes.
 *
 * Constructors take ownership of names and child pointers. ast_free walks
 * the tree; Type* on nodes is interned in the type pool and not freed here.
 */
#include "ast.h"
#include <stdlib.h>
#include <string.h>

/** Zeroed node with kind and location. */
AstNode *ast_new(AstKind kind, SourceLoc loc) {
    AstNode *n = (AstNode *)calloc(1, sizeof(AstNode));
    if (!n) loam_fatal("out of memory");
    n->kind = kind;
    n->loc = loc;
    return n;
}

/** Recursively free `node` and owned strings. Types are pool-allocated. */
void ast_free(AstNode *node) {
    if (!node) return;
    switch (node->kind) {
        case AST_PROGRAM:
            for (size_t i = 0; i < node->as.program.import_count; i++)
                ast_free(node->as.program.imports[i]);
            free(node->as.program.imports);
            for (size_t i = 0; i < node->as.program.decl_count; i++)
                ast_free(node->as.program.decls[i]);
            free(node->as.program.decls);
            free((void *)node->as.program.mod_name);
            free((void *)node->as.program.mod_doc);
            break;
        case AST_IMPORT:
            free((void *)node->as.import.alias);
            free((void *)node->as.import.path);
            free((void *)node->as.import.resolved);
            break;
        case AST_FN_DECL:
        case AST_CLOSURE:
            free((void *)node->as.fn.name);
            free((void *)node->as.fn.cname);
            for (size_t i = 0; i < node->as.fn.param_count; i++) {
                free((void *)node->as.fn.params[i].name);
                ast_free(node->as.fn.params[i].type);
            }
            free(node->as.fn.params);
            ast_free(node->as.fn.ret_type);
            ast_free(node->as.fn.body);
            for (size_t i = 0; i < node->as.fn.tparam_count; i++)
                free((void *)node->as.fn.tparams[i]);
            free(node->as.fn.tparams);
            for (size_t i = 0; i < node->as.fn.cap_count; i++)
                free((void *)node->as.fn.caps[i]);
            free(node->as.fn.caps);
            free(node->as.fn.cap_types);
            break;
        case AST_STRUCT_DECL:
            free((void *)node->as.strct.name);
            for (size_t i = 0; i < node->as.strct.field_count; i++) {
                free((void *)node->as.strct.fields[i].name);
                free((void *)node->as.strct.fields[i].doc);
                ast_free(node->as.strct.fields[i].type);
                ast_free(node->as.strct.fields[i].def);
            }
            free(node->as.strct.fields);
            for (size_t i = 0; i < node->as.strct.tparam_count; i++)
                free((void *)node->as.strct.tparams[i]);
            free(node->as.strct.tparams);
            break;
        case AST_ENUM_DECL:
            free((void *)node->as.enm.name);
            for (size_t i = 0; i < node->as.enm.count; i++)
                free((void *)node->as.enm.vnames[i]);
            free(node->as.enm.vnames);
            free(node->as.enm.vals);
            break;
        case AST_VAR_DECL:
            free((void *)node->as.var.name);
            ast_free(node->as.var.type);
            ast_free(node->as.var.init);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < node->as.block.stmt_count; i++)
                ast_free(node->as.block.stmts[i]);
            free(node->as.block.stmts);
            break;
        case AST_IF:
            ast_free(node->as.if_stmt.cond);
            ast_free(node->as.if_stmt.then_block);
            ast_free(node->as.if_stmt.else_block);
            break;
        case AST_WHILE:
            ast_free(node->as.if_stmt.cond);
            ast_free(node->as.if_stmt.then_block);
            break;
        case AST_FOR:
            free((void *)node->as.for_stmt.var);
            ast_free(node->as.for_stmt.iter);
            ast_free(node->as.for_stmt.body);
            break;
        case AST_MATCH:
            ast_free(node->as.match_stmt.scrut);
            for (size_t i = 0; i < node->as.match_stmt.arm_count; i++)
                ast_free(node->as.match_stmt.arms[i]);
            free(node->as.match_stmt.arms);
            break;
        case AST_MATCH_ARM:
            for (size_t i = 0; i < node->as.match_arm.pat_count; i++)
                ast_free(node->as.match_arm.pats[i]);
            free(node->as.match_arm.pats);
            ast_free(node->as.match_arm.body);
            break;
        case AST_RETURN:
            ast_free(node->as.ret.expr);
            break;
        case AST_BREAK:
        case AST_CONTINUE:
            break;
        case AST_EXPR_STMT:
            ast_free(node->as.expr_stmt.expr);
            break;
        case AST_ASSIGN:
            ast_free(node->as.assign.left);
            ast_free(node->as.assign.right);
            break;
        case AST_INCDEC:
            ast_free(node->as.incdec.operand);
            break;
        case AST_BINARY:
            ast_free(node->as.binary.left);
            ast_free(node->as.binary.right);
            break;
        case AST_UNARY:
            ast_free(node->as.unary.operand);
            break;
        case AST_CAST:
            ast_free(node->as.cast.expr);
            ast_free(node->as.cast.type);
            break;
        case AST_CALL:
            ast_free(node->as.call.callee);
            for (size_t i = 0; i < node->as.call.arg_count; i++)
                ast_free(node->as.call.args[i]);
            free(node->as.call.args);
            break;
        case AST_INDEX:
        case AST_FIELD:
        case AST_DEREF:
        case AST_ADDR:
            ast_free(node->as.access.target);
            ast_free(node->as.access.index);
            free((void *)node->as.access.field);
            break;
        case AST_IDENT:
            free((void *)node->as.ident.name);
            break;
        case AST_NUMBER:
        case AST_FLOAT:
        case AST_BOOL:
            break;
        case AST_STRING:
            free((void *)node->as.lit.str);
            break;
        case AST_STRUCT_LIT:
            free((void *)node->as.struct_lit.type_name);
            for (size_t i = 0; i < node->as.struct_lit.field_count; i++) {
                free((void *)node->as.struct_lit.fields[i].name);
                ast_free(node->as.struct_lit.fields[i].init);
            }
            free(node->as.struct_lit.fields);
            break;
        case AST_ARRAY_LIT:
            ast_free(node->as.array_lit.elem_type);
            for (size_t i = 0; i < node->as.array_lit.count; i++)
                ast_free(node->as.array_lit.elems[i]);
            free(node->as.array_lit.elems);
            break;
        case AST_TUPLE:
            for (size_t i = 0; i < node->as.array_lit.count; i++)
                ast_free(node->as.array_lit.elems[i]);
            free(node->as.array_lit.elems);
            break;
        case AST_TYPE:
            free((void *)node->as.type.name);
            ast_free(node->as.type.elem);
            for (size_t i = 0; i < node->as.type.fn_param_count; i++)
                ast_free(node->as.type.fn_params[i]);
            free(node->as.type.fn_params);
            for (size_t i = 0; i < node->as.type.targ_count; i++)
                ast_free(node->as.type.targs[i]);
            free(node->as.type.targs);
            break;
    }
    free((void *)node->doc);
    free(node);
}

/* --- constructors (see ast.h) --- */

AstNode *ast_program(AstNode **imps, size_t ni, AstNode **decls, size_t nd, SourceLoc loc) {
    AstNode *n = ast_new(AST_PROGRAM, loc);
    n->as.program.imports = imps;
    n->as.program.import_count = ni;
    n->as.program.decls = decls;
    n->as.program.decl_count = nd;
    return n;
}

AstNode *ast_import(const char *alias, const char *path, SourceLoc loc) {
    AstNode *n = ast_new(AST_IMPORT, loc);
    n->as.import.alias = alias;
    n->as.import.path = path;
    return n;
}

AstNode *ast_fn(const char *name, Param *params, size_t pc, AstNode *ret, AstNode *body, SourceLoc loc) {
    AstNode *n = ast_new(AST_FN_DECL, loc);
    n->as.fn.name = name;
    n->as.fn.params = params;
    n->as.fn.param_count = pc;
    n->as.fn.ret_type = ret;
    n->as.fn.body = body;
    return n;
}

AstNode *ast_enum(const char *name, const char **vnames, int64_t *vals, size_t n, SourceLoc loc) {
    AstNode *e = ast_new(AST_ENUM_DECL, loc);
    e->as.enm.name = name;
    e->as.enm.vnames = vnames;
    e->as.enm.vals = vals;
    e->as.enm.count = n;
    return e;
}

AstNode *ast_struct(const char *name, Field *fields, size_t fc, SourceLoc loc) {
    AstNode *n = ast_new(AST_STRUCT_DECL, loc);
    n->as.strct.name = name;
    n->as.strct.fields = fields;
    n->as.strct.field_count = fc;
    return n;
}

AstNode *ast_var(const char *name, AstNode *type, AstNode *init, int is_mut, SourceLoc loc) {
    AstNode *n = ast_new(AST_VAR_DECL, loc);
    n->as.var.name = name;
    n->as.var.type = type;
    n->as.var.init = init;
    n->as.var.is_mut = is_mut;
    return n;
}

AstNode *ast_block(AstNode **stmts, size_t n, SourceLoc loc) {
    AstNode *node = ast_new(AST_BLOCK, loc);
    node->as.block.stmts = stmts;
    node->as.block.stmt_count = n;
    return node;
}

AstNode *ast_if(AstNode *cond, AstNode *thenb, AstNode *elseb, SourceLoc loc) {
    AstNode *n = ast_new(AST_IF, loc);
    n->as.if_stmt.cond = cond;
    n->as.if_stmt.then_block = thenb;
    n->as.if_stmt.else_block = elseb;
    return n;
}

AstNode *ast_for(const char *var, AstNode *iter, AstNode *body, SourceLoc loc) {
    AstNode *n = ast_new(AST_FOR, loc);
    n->as.for_stmt.var = var;
    n->as.for_stmt.iter = iter;
    n->as.for_stmt.body = body;
    return n;
}

AstNode *ast_while(AstNode *cond, AstNode *body, SourceLoc loc) {
    AstNode *n = ast_new(AST_WHILE, loc);
    n->as.if_stmt.cond = cond;
    n->as.if_stmt.then_block = body;
    n->as.if_stmt.else_block = NULL;
    return n;
}

AstNode *ast_match(AstNode *scrut, AstNode **arms, size_t n, SourceLoc loc) {
    AstNode *node = ast_new(AST_MATCH, loc);
    node->as.match_stmt.scrut = scrut;
    node->as.match_stmt.arms = arms;
    node->as.match_stmt.arm_count = n;
    return node;
}

AstNode *ast_match_arm(AstNode **pats, size_t np, int wild, AstNode *body, SourceLoc loc) {
    AstNode *n = ast_new(AST_MATCH_ARM, loc);
    n->as.match_arm.pats = pats;
    n->as.match_arm.pat_count = np;
    n->as.match_arm.is_wild = wild;
    n->as.match_arm.body = body;
    return n;
}

AstNode *ast_break(SourceLoc loc) {
    return ast_new(AST_BREAK, loc);
}

AstNode *ast_continue(SourceLoc loc) {
    return ast_new(AST_CONTINUE, loc);
}

AstNode *ast_return(AstNode *expr, SourceLoc loc) {
    AstNode *n = ast_new(AST_RETURN, loc);
    n->as.ret.expr = expr;
    return n;
}

AstNode *ast_expr_stmt(AstNode *expr, SourceLoc loc) {
    AstNode *n = ast_new(AST_EXPR_STMT, loc);
    n->as.expr_stmt.expr = expr;
    return n;
}

AstNode *ast_assign(TokenKind op, AstNode *l, AstNode *r, SourceLoc loc) {
    AstNode *n = ast_new(AST_ASSIGN, loc);
    n->as.assign.op = op;
    n->as.assign.left = l;
    n->as.assign.right = r;
    return n;
}

AstNode *ast_binary(TokenKind op, AstNode *l, AstNode *r, SourceLoc loc) {
    AstNode *n = ast_new(AST_BINARY, loc);
    n->as.binary.op = op;
    n->as.binary.left = l;
    n->as.binary.right = r;
    return n;
}

AstNode *ast_unary(TokenKind op, AstNode *opnd, SourceLoc loc) {
    AstNode *n = ast_new(AST_UNARY, loc);
    n->as.unary.op = op;
    n->as.unary.operand = opnd;
    return n;
}

AstNode *ast_incdec(AstNode *opnd, int is_dec, int is_post, SourceLoc loc) {
    AstNode *n = ast_new(AST_INCDEC, loc);
    n->as.incdec.operand = opnd;
    n->as.incdec.is_dec = is_dec;
    n->as.incdec.is_post = is_post;
    return n;
}

AstNode *ast_cast(AstNode *expr, AstNode *type, SourceLoc loc) {
    AstNode *n = ast_new(AST_CAST, loc);
    n->as.cast.expr = expr;
    n->as.cast.type = type;
    return n;
}

AstNode *ast_call(AstNode *callee, AstNode **args, size_t n, SourceLoc loc) {
    AstNode *node = ast_new(AST_CALL, loc);
    node->as.call.callee = callee;
    node->as.call.args = args;
    node->as.call.arg_count = n;
    return node;
}

AstNode *ast_index(AstNode *target, AstNode *idx, SourceLoc loc) {
    AstNode *n = ast_new(AST_INDEX, loc);
    n->as.access.target = target;
    n->as.access.index = idx;
    return n;
}

AstNode *ast_field(AstNode *target, const char *field, int via_colon, SourceLoc loc) {
    AstNode *n = ast_new(AST_FIELD, loc);
    n->as.access.target = target;
    n->as.access.field = field;
    n->as.access.via_colon = via_colon;
    return n;
}

AstNode *ast_ident(const char *name, SourceLoc loc) {
    AstNode *n = ast_new(AST_IDENT, loc);
    n->as.ident.name = name;
    return n;
}

AstNode *ast_number(int64_t v, SourceLoc loc) {
    AstNode *n = ast_new(AST_NUMBER, loc);
    n->as.lit.value = v;
    return n;
}

AstNode *ast_float(double v, SourceLoc loc) {
    AstNode *n = ast_new(AST_FLOAT, loc);
    n->as.lit.f = v;
    return n;
}

AstNode *ast_string(const char *s, SourceLoc loc) {
    AstNode *n = ast_new(AST_STRING, loc);
    n->as.lit.str = s;
    return n;
}

AstNode *ast_bool(int b, SourceLoc loc) {
    AstNode *n = ast_new(AST_BOOL, loc);
    n->as.lit.b = b;
    return n;
}

AstNode *ast_struct_lit(const char *tn, FieldInit *fi, size_t n, SourceLoc loc) {
    AstNode *node = ast_new(AST_STRUCT_LIT, loc);
    node->as.struct_lit.type_name = tn;
    node->as.struct_lit.fields = fi;
    node->as.struct_lit.field_count = n;
    return node;
}

AstNode *ast_array_lit(AstNode *et, int64_t len, AstNode **elems, size_t n, SourceLoc loc) {
    AstNode *node = ast_new(AST_ARRAY_LIT, loc);
    node->as.array_lit.elem_type = et;
    node->as.array_lit.len = len;
    node->as.array_lit.elems = elems;
    node->as.array_lit.count = n;
    return node;
}

AstNode *ast_tuple(AstNode **elems, size_t n, SourceLoc loc) {
    AstNode *node = ast_new(AST_TUPLE, loc);
    node->as.array_lit.elems = elems;
    node->as.array_lit.count = n;
    return node;
}

AstNode *ast_deref(AstNode *target, SourceLoc loc) {
    AstNode *n = ast_new(AST_DEREF, loc);
    n->as.access.target = target;
    return n;
}

AstNode *ast_addr(AstNode *target, int is_mut, SourceLoc loc) {
    AstNode *n = ast_new(AST_ADDR, loc);
    n->as.access.target = target;
    n->as.access.is_mut = is_mut;
    return n;
}

AstNode *ast_type(const char *name, int tag, AstNode *elem, int64_t alen, SourceLoc loc) {
    AstNode *n = ast_new(AST_TYPE, loc);
    n->as.type.name = name;
    n->as.type.tag = tag;
    n->as.type.elem = elem;
    n->as.type.array_len = alen;
    return n;
}
