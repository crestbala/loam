/**
 * ast_dump.c — `loamc --dump-ast file.loam`: the parser's tree as text, the
 * reference the self-hosted parser (packages/loam/compiler/parser.loam) is
 * diffed against by the `selfhost_parse` host stanza.
 *
 * The tree is printed as `parser.c` returns it, before typecheck: one node per
 * line, two spaces of indent per level, then the node's role in its parent,
 * its kind, `line:col-end_line:end_col`, and the attributes the parser set.
 * An absent child is `role -`. Strings are quoted with `\\`, `\"`, `\n`, `\t`,
 * `\r` and other control bytes (`\xHH`) escaped; floats are the shortest of
 * `%.15g`/`%.16g`/`%.17g` that reads back exactly (std:conv's `f64_to_str`).
 *
 * On a parse error nothing is printed: the diagnostics are on stderr and the
 * exit status is 1. A tree after an error is half-built and not a contract.
 *
 * `--dump-modules` lists what the loader resolved (compile.c), the input the
 * self-hosted checker starts from: the runtime header, then each module in
 * load order with its name, `done_rank` and path, and each import (the
 * loader's implicit `http` / `std:json` ones included) with its resolved path.
 *
 * `--dump-typed` loads and typechecks, then prints every module's tree in the
 * format above plus what typecheck set: ` ty=` on each typed node (a
 * structural spelling: `'T` is a type parameter, `Name#cname` a struct whose C
 * name differs), the C names, resolved targets (` res=Kind "name" file:l:c`),
 * call markers, captures and conversion modes; then the monomorphized
 * instances, struct instances and module globals. On a load or type error
 * only the diagnostics are printed (exit 1 / 2).
 */
#include "ast.h"
#include "compile.h"
#include "parser.h"
#include "sema/typecheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_str(const char *s) {
    putchar('"');
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        unsigned char c = *q;
        if (c == '\\') fputs("\\\\", stdout);
        else if (c == '"') fputs("\\\"", stdout);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
        else putchar(c);
    }
    putchar('"');
}

static void put_attr_str(const char *key, const char *s) {
    if (!s) return;
    printf(" %s=", key);
    put_str(s);
}

static void put_flag(const char *key, int on) {
    if (on) printf(" %s=1", key);
}

static void put_f64(double v) {
    char buf[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (prec == 17 || strtod(buf, NULL) == v) break;
    }
    fputs(buf, stdout);
}

static void indent(int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
}

static void head(int depth, const char *role, const char *kind, SourceLoc l) {
    indent(depth);
    printf("%s %s %d:%d-%d:%d", role, kind, l.line, l.col, l.end_line, l.end_col);
}

static void leaf_str(int depth, const char *role, const char *s) {
    indent(depth);
    printf("%s ", role);
    put_str(s);
    putchar('\n');
}


/* --dump-typed: print typecheck's fields too. */
static int g_typed;

static void put_type(const Type *t) {
    if (!t) {
        fputs("?", stdout);
        return;
    }
    switch (t->kind) {
        case TY_VOID: fputs("void", stdout); return;
        case TY_BOOL: fputs("bool", stdout); return;
        case TY_STRING: fputs("string", stdout); return;
        case TY_INT:
        case TY_FLOAT: fputs(type_name(t), stdout); return;
        case TY_PTR:
            fputs(t->is_mut ? "&mut " : "&", stdout);
            put_type(t->elem);
            return;
        case TY_BOX:
            fputs("Box<", stdout);
            put_type(t->elem);
            putchar('>');
            return;
        case TY_ARRAY:
            printf("[%lld]", (long long)t->array_len);
            put_type(t->elem);
            return;
        case TY_VEC:
            fputs("[]", stdout);
            put_type(t->elem);
            return;
        case TY_STRUCT:
            fputs(t->name ? t->name : "struct", stdout);
            if (t->param_count) {
                putchar('<');
                for (size_t i = 0; i < t->param_count; i++) {
                    if (i) fputs(", ", stdout);
                    put_type(t->params[i]);
                }
                putchar('>');
            }
            if (t->cname) printf("#%s", t->cname);
            return;
        case TY_PARAM:
            printf("'%s", t->name ? t->name : "?");
            return;
        case TY_PROC:
            fputs("fn(", stdout);
            for (size_t i = 0; i < t->param_count; i++) {
                if (i) fputs(", ", stdout);
                put_type(t->params[i]);
            }
            fputs(") -> ", stdout);
            put_type(t->ret);
            return;
        default:
            fputs("<unknown>", stdout);
            return;
    }
}

static const char *kind_name(AstKind k);

/** The declaration a name resolved to: its kind, name and place. */
static void put_ref(const char *key, const AstNode *d) {
    if (!d) return;
    const char *nm = NULL;
    if (d->kind == AST_VAR_DECL) nm = d->as.var.name;
    else if (d->kind == AST_FN_DECL || d->kind == AST_CLOSURE) nm = d->as.fn.name;
    else if (d->kind == AST_STRUCT_DECL) nm = d->as.strct.name;
    else if (d->kind == AST_ENUM_DECL) nm = d->as.enm.name;
    printf(" %s=%s", key, kind_name(d->kind));
    if (nm) {
        putchar(' ');
        put_str(nm);
    }
    printf(" %s:%d:%d", d->loc.file ? d->loc.file : "-", d->loc.line, d->loc.col);
}

static void put_int_attr(const char *key, long long v) {
    if (v) printf(" %s=%lld", key, v);
}

static const char *kind_name(AstKind k) {
    switch (k) {
        case AST_PROGRAM: return "Program";
        case AST_IMPORT: return "Import";
        case AST_FN_DECL: return "Fn";
        case AST_STRUCT_DECL: return "Struct";
        case AST_ENUM_DECL: return "Enum";
        case AST_VAR_DECL: return "Var";
        case AST_BLOCK: return "Block";
        case AST_IF: return "If";
        case AST_FOR: return "For";
        case AST_WHILE: return "While";
        case AST_SWITCH: return "Switch";
        case AST_CASE: return "Case";
        case AST_RETURN: return "Return";
        case AST_BREAK: return "Break";
        case AST_CONTINUE: return "Continue";
        case AST_EXPR_STMT: return "ExprStmt";
        case AST_ASSIGN: return "Assign";
        case AST_BINARY: return "Binary";
        case AST_UNARY: return "Unary";
        case AST_CAST: return "Cast";
        case AST_CALL: return "Call";
        case AST_INDEX: return "Index";
        case AST_FIELD: return "Field";
        case AST_IDENT: return "Ident";
        case AST_NUMBER: return "Number";
        case AST_FLOAT: return "Float";
        case AST_STRING: return "String";
        case AST_BOOL: return "Bool";
        case AST_STRUCT_LIT: return "StructLit";
        case AST_ARRAY_LIT: return "ArrayLit";
        case AST_TUPLE: return "Tuple";
        case AST_DEREF: return "Deref";
        case AST_ADDR: return "Addr";
        case AST_TYPE: return "Type";
        case AST_CLOSURE: return "Closure";
        case AST_INCDEC: return "IncDec";
        case AST_TRY: return "Try";
    }
    return "?";
}

static void dump(const AstNode *n, int depth, const char *role);

static void dump_list(AstNode *const *xs, size_t n, int depth, const char *role) {
    for (size_t i = 0; i < n; i++) dump(xs[i], depth, role);
}

static void dump_names(const char *const *xs, size_t n, int depth, const char *role) {
    for (size_t i = 0; i < n; i++) leaf_str(depth, role, xs[i]);
}

static void dump(const AstNode *n, int depth, const char *role) {
    if (!n) {
        indent(depth);
        printf("%s -\n", role);
        return;
    }
    int d = depth + 1;
    head(depth, role, kind_name(n->kind), n->loc);
    if (n->flags) printf(" flags=%d", n->flags);
    put_attr_str("doc", n->doc);
    if (g_typed) {
        if (n->ty) {
            fputs(" ty=", stdout);
            put_type(n->ty);
        }
        put_int_attr("place_mut", n->place_mut);
        switch (n->kind) {
            case AST_PROGRAM: put_attr_str("mod_name", n->as.program.mod_name); break;
            case AST_IMPORT: put_attr_str("resolved", n->as.import.resolved); break;
            case AST_FN_DECL:
            case AST_CLOSURE:
                put_attr_str("cname", n->as.fn.cname);
                put_int_attr("intrinsic", n->as.fn.is_intrinsic);
                put_int_attr("clos_id", n->as.fn.clos_id);
                put_int_attr("as_value", n->as.fn.used_as_value);
                break;
            case AST_CALL:
                put_int_attr("println", n->as.call.is_println);
                put_int_attr("box_new", n->as.call.is_box_new);
                put_int_attr("vec_push", n->as.call.is_vec_push);
                put_int_attr("vec_pop", n->as.call.is_vec_pop);
                put_int_attr("sizeof", n->as.call.is_sizeof);
                put_int_attr("panic", n->as.call.is_panic);
                put_int_attr("numb", n->as.call.num_builtin);
                put_attr_str("c_builtin", n->as.call.c_builtin);
                put_attr_str("rcname", n->as.call.resolved_cname);
                put_int_attr("fn_val", n->as.call.is_fn_val);
                put_int_attr("sig", n->as.call.sig_cell);
                put_int_attr("value_key", n->as.call.value_key);
                put_int_attr("fut", n->as.call.fut_cell);
                put_int_attr("ch", n->as.call.ch_cell);
                break;
            case AST_FIELD:
            case AST_INDEX:
                put_ref("res", n->as.access.resolved);
                break;
            case AST_IDENT:
                put_ref("res", n->as.ident.resolved);
                if (n->as.ident.def_loc.line)
                    printf(" def=%d:%d", n->as.ident.def_loc.line, n->as.ident.def_loc.col);
                break;
            case AST_STRUCT_LIT: put_int_attr("zero_rest", n->as.struct_lit.zero_rest); break;
            case AST_CAST: put_int_attr("conv", n->as.cast.conv_mode); break;
            case AST_TRY: put_attr_str("errname", n->as.try_expr.errname); break;
            default: break;
        }
    }
    switch (n->kind) {
        case AST_PROGRAM:
            put_attr_str("mod_doc", n->as.program.mod_doc);
            putchar('\n');
            for (size_t i = 0; i < n->as.program.c_dir_count; i++) {
                const CDirective *c = &n->as.program.c_dirs[i];
                head(d, "cdir", "CDir", c->loc);
                printf(" link=%d\n", c->is_link);
                for (size_t k = 0; k < c->arg_count; k++) leaf_str(d + 1, "arg", c->args[k]);
            }
            dump_list(n->as.program.imports, n->as.program.import_count, d, "import");
            dump_list(n->as.program.decls, n->as.program.decl_count, d, "decl");
            return;
        case AST_IMPORT:
            put_attr_str("alias", n->as.import.alias);
            put_attr_str("path", n->as.import.path);
            putchar('\n');
            for (size_t i = 0; i < n->as.import.name_count; i++) {
                indent(d);
                fputs("name", stdout);
                put_attr_str("src", n->as.import.names[i]);
                put_attr_str("local", n->as.import.locals[i]);
                putchar('\n');
            }
            return;
        case AST_FN_DECL:
        case AST_CLOSURE:
            put_attr_str("name", n->as.fn.name);
            put_flag("async", n->as.fn.is_async);
            put_flag("test", n->as.fn.is_test);
            put_flag("server", n->as.fn.is_server);
            put_attr_str("c_sym", n->as.fn.c_sym);
            putchar('\n');
            dump_names(n->as.fn.tparams, n->as.fn.tparam_count, d, "tparam");
            for (size_t i = 0; g_typed && i < n->as.fn.cap_count; i++) {
                indent(d);
                fputs("cap ", stdout);
                put_str(n->as.fn.caps[i]);
                fputs(" ty=", stdout);
                put_type(n->as.fn.cap_types ? n->as.fn.cap_types[i] : NULL);
                putchar('\n');
            }
            for (size_t i = 0; i < n->as.fn.param_count; i++) {
                const Param *pa = &n->as.fn.params[i];
                head(d, "param", "Param", pa->loc);
                put_attr_str("name", pa->name);
                putchar('\n');
                dump(pa->type, d + 1, "type");
                dump(pa->def, d + 1, "def");
            }
            dump(n->as.fn.ret_type, d, "ret");
            dump(n->as.fn.body, d, "body");
            return;
        case AST_STRUCT_DECL:
            put_attr_str("name", n->as.strct.name);
            put_flag("proto", n->as.strct.is_proto);
            put_flag("json", n->as.strct.is_json);
            put_flag("must_check", n->as.strct.is_must_check);
            put_flag("no_send", n->as.strct.is_no_send);
            putchar('\n');
            dump_names(n->as.strct.tparams, n->as.strct.tparam_count, d, "tparam");
            for (size_t i = 0; i < n->as.strct.field_count; i++) {
                const Field *f = &n->as.strct.fields[i];
                indent(d);
                fputs("field", stdout);
                put_attr_str("name", f->name);
                put_attr_str("doc", f->doc);
                putchar('\n');
                dump(f->type, d + 1, "type");
                dump(f->def, d + 1, "def");
            }
            return;
        case AST_ENUM_DECL:
            put_attr_str("name", n->as.enm.name);
            putchar('\n');
            for (size_t i = 0; i < n->as.enm.count; i++) {
                indent(d);
                fputs("variant", stdout);
                put_attr_str("name", n->as.enm.vnames[i]);
                printf(" value=%lld\n", (long long)n->as.enm.vals[i]);
            }
            return;
        case AST_VAR_DECL:
            put_attr_str("name", n->as.var.name);
            printf(" mut=%d\n", n->as.var.is_mut);
            dump(n->as.var.type, d, "type");
            dump(n->as.var.init, d, "init");
            return;
        case AST_BLOCK:
            putchar('\n');
            dump_list(n->as.block.stmts, n->as.block.stmt_count, d, "stmt");
            return;
        case AST_IF:
        case AST_WHILE:
            putchar('\n');
            dump(n->as.if_stmt.cond, d, "cond");
            dump(n->as.if_stmt.then_block, d, "then");
            dump(n->as.if_stmt.else_block, d, "else");
            return;
        case AST_FOR:
            put_attr_str("var", n->as.for_stmt.var);
            putchar('\n');
            dump(n->as.for_stmt.iter, d, "iter");
            dump(n->as.for_stmt.body, d, "body");
            return;
        case AST_SWITCH:
            putchar('\n');
            dump(n->as.switch_stmt.scrut, d, "scrut");
            dump_list(n->as.switch_stmt.cases, n->as.switch_stmt.case_count, d, "case");
            return;
        case AST_CASE:
            printf(" default=%d\n", n->as.switch_case.is_default);
            dump_list(n->as.switch_case.pats, n->as.switch_case.pat_count, d, "pat");
            dump(n->as.switch_case.body, d, "body");
            return;
        case AST_RETURN:
            putchar('\n');
            dump(n->as.ret.expr, d, "expr");
            return;
        case AST_BREAK:
        case AST_CONTINUE:
            putchar('\n');
            return;
        case AST_EXPR_STMT:
            putchar('\n');
            dump(n->as.expr_stmt.expr, d, "expr");
            return;
        case AST_ASSIGN:
        case AST_BINARY:
            printf(" op=%s\n", token_kind_name(n->as.binary.op));
            dump(n->as.binary.left, d, "left");
            dump(n->as.binary.right, d, "right");
            return;
        case AST_UNARY:
            printf(" op=%s\n", token_kind_name(n->as.unary.op));
            dump(n->as.unary.operand, d, "operand");
            return;
        case AST_CAST:
            putchar('\n');
            dump(n->as.cast.expr, d, "expr");
            dump(n->as.cast.type, d, "type");
            return;
        case AST_TRY:
            putchar('\n');
            dump(n->as.try_expr.expr, d, "expr");
            return;
        case AST_CALL:
            putchar('\n');
            dump(n->as.call.callee, d, "callee");
            dump_list(n->as.call.args, n->as.call.arg_count, d, "arg");
            return;
        case AST_INDEX:
            putchar('\n');
            dump(n->as.access.target, d, "target");
            dump(n->as.access.index, d, "index");
            return;
        case AST_FIELD:
            put_attr_str("name", n->as.access.field);
            printf(" colon=%d\n", n->as.access.via_colon);
            dump(n->as.access.target, d, "target");
            return;
        case AST_DEREF:
            putchar('\n');
            dump(n->as.access.target, d, "target");
            return;
        case AST_ADDR:
            printf(" mut=%d\n", n->as.access.is_mut);
            dump(n->as.access.target, d, "target");
            return;
        case AST_IDENT:
            put_attr_str("name", n->as.ident.name);
            putchar('\n');
            return;
        case AST_NUMBER:
            printf(" value=%lld\n", (long long)n->as.lit.value);
            return;
        case AST_FLOAT:
            fputs(" value=", stdout);
            put_f64(n->as.lit.f);
            putchar('\n');
            return;
        case AST_STRING:
            put_attr_str("value", n->as.lit.str);
            putchar('\n');
            return;
        case AST_BOOL:
            printf(" value=%d\n", n->as.lit.b);
            return;
        case AST_STRUCT_LIT:
            put_attr_str("name", n->as.struct_lit.type_name);
            putchar('\n');
            dump_list(n->as.struct_lit.targs, n->as.struct_lit.targ_count, d, "targ");
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++) {
                indent(d);
                fputs("init", stdout);
                put_attr_str("name", n->as.struct_lit.fields[i].name);
                putchar('\n');
                dump(n->as.struct_lit.fields[i].init, d + 1, "value");
            }
            return;
        case AST_ARRAY_LIT:
            printf(" len=%lld\n", (long long)n->as.array_lit.len);
            dump(n->as.array_lit.elem_type, d, "type");
            dump_list(n->as.array_lit.elems, n->as.array_lit.count, d, "elem");
            return;
        case AST_TUPLE:
            putchar('\n');
            dump_list(n->as.array_lit.elems, n->as.array_lit.count, d, "elem");
            return;
        case AST_TYPE:
            printf(" tag=%d", n->as.type.tag);
            put_attr_str("name", n->as.type.name);
            printf(" len=%lld\n", (long long)n->as.type.array_len);
            dump(n->as.type.elem, d, "elem");
            dump_list(n->as.type.fn_params, n->as.type.fn_param_count, d, "param");
            dump_list(n->as.type.targs, n->as.type.targ_count, d, "targ");
            return;
        case AST_INCDEC:
            printf(" dec=%d post=%d\n", n->as.incdec.is_dec, n->as.incdec.is_post);
            dump(n->as.incdec.operand, d, "operand");
            return;
    }
    putchar('\n');
}

int ast_dump_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "loam: cannot read %s\n", path);
        return 1;
    }
    size_t cap = 1 << 16, n = 0;
    char *src = (char *)malloc(cap);
    for (;;) {
        if (n + 4096 > cap) src = (char *)realloc(src, cap *= 2);
        size_t r = fread(src + n, 1, 4096, f);
        n += r;
        if (r == 0) break;
    }
    fclose(f);
    src[n] = 0;
    Lexer l;
    lexer_init(&l, src, path);
    Parser p;
    parser_init(&p, &l);
    AstNode *prog = parser_parse(&p);
    if (p.had_error) return 1; /* the process exits; the tree is not freed */
    dump(prog, 0, "root");
    ast_free(prog);
    free(src);
    return 0;
}

static LoamSession g_sess;

int ast_dump_modules(const char *path) {
    int rc = loam_session_typecheck(&g_sess, path, 0);
    if (rc == 1) return 1;
    fputs("rt ", stdout);
    put_str(LOAM_RT_PATH);
    putchar('\n');
    fputs("std ", stdout);
    put_str(LOAM_STD_DIR);
    putchar('\n');
    for (int m = 0; m < g_sess.nmods; m++) {
        LoamModule *mod = &g_sess.mods[m];
        printf("module %d", m);
        put_attr_str("name", mod->name);
        printf(" rank=%d std=%d", mod->done_rank, loam_is_std_path(mod->path));
        put_attr_str("path", mod->path);
        putchar('\n');
        AstNode *p = mod->ast;
        for (size_t i = 0; p && i < p->as.program.import_count; i++) {
            AstNode *im = p->as.program.imports[i];
            fputs("  import", stdout);
            put_attr_str("alias", im->as.import.alias);
            put_attr_str("spec", im->as.import.path);
            put_attr_str("resolved", im->as.import.resolved);
            putchar('\n');
        }
    }
    return 0;
}

int ast_dump_typed(const char *path) {
    int rc = loam_session_typecheck(&g_sess, path, 1);
    if (rc) return rc;
    g_typed = 1;
    for (int m = 0; m < g_sess.nmods; m++) {
        printf("module %d", m);
        put_attr_str("name", g_sess.mods[m].name);
        putchar('\n');
        dump(g_sess.mods[m].ast, 1, "root");
    }
    for (int i = 0; i < typecheck_mono_count(); i++) {
        fputs("mono", stdout);
        put_attr_str("cname", typecheck_mono_cname(i));
        put_ref("fn", typecheck_mono_fn(i));
        fputs(" args=", stdout);
        Type **args = typecheck_mono_args(i);
        for (size_t k = 0; k < typecheck_mono_nargs(i); k++) {
            if (k) putchar(',');
            put_type(args[k]);
        }
        putchar('\n');
    }
    for (int i = 0; i < typecheck_struct_inst_count(); i++) {
        fputs("struct_inst ", stdout);
        put_type(typecheck_struct_inst(i));
        putchar('\n');
    }
    for (int i = 0; i < typecheck_global_count(); i++) {
        fputs("global", stdout);
        put_attr_str("cname", typecheck_global_cname(i));
        printf(" mod=%d", typecheck_global_mod(i));
        put_ref("var", typecheck_global_var(i));
        putchar('\n');
    }
    return 0;
}
