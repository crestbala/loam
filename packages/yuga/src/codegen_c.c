/**
 * codegen_c.c — translate a typechecked Yuga program to gnu99 C.
 *
 * Layout of the generated file:
 *   1. paste yuga_rt.h (panic, overflow, fmt writev, Box malloc)
 *   2. zeus_rt.h / maya_rt.h if those modules are used (handles + plat/engine decls)
 *   3. struct typedefs (Node/Signal and maya handles stay in runtime headers)
 *   4. prototypes, closure envs, monomorphized generics
 *   5. function bodies; main returns 0
 *
 * Fully-lowered function bodies are emitted from IrModule (blocks + gotos).
 * Closures are env-struct + fn-pointer (`IR_CLOS`). Intrinsics have no C body.
 */
#include "codegen_c.h"
#include "compile.h"
#include "dce.h"
#include "ir.h"
#include "lexer.h"
#include "sema/type.h"
#include "sema/typecheck.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static int tmp_id;
static int drop_sp;
static const char *drop_names[64][64];
static int drop_n[64];
static const char *cur_mod_name;
static int cur_is_main_mod;
static AstNode *emit_clos;
static const char **subst_names;
static Type **subst_args;
static size_t subst_n;
static const char *cname_override;
/* Grows: one thunk per reactive prop adds up fast. */
static AstNode **clos_nodes;
static int nclos_nodes;
static int clos_cap;
static AstNode *clos_hoist_node[64];
static int clos_hoist_tid[64];
static int clos_hoist_n;
static IrModule *emit_ir_mod;
static const IrFn *CF;
/* Phase 12: `yugac test` — emit a runner `main` that calls every `#[test]` fn
   and skips the program's own `main`. */
static int test_mode;
/* `#[server]` splitting: on a client target, a server fn's body is replaced by
   a trap stub, so the body (and its literals) never reach the client binary. */
static int server_split;

void codegen_set_test_mode(int on) {
    test_mode = on;
}

void codegen_set_server_split(int on) {
    server_split = on;
}

static void indent(FILE *o, int n) {
    for (int i = 0; i < n; i++) fprintf(o, "    ");
}

/* Phase 12: `#line` directives map generated C back to the `.loam` source, so
   debuggers, profilers, and sanitizers report Yuga lines. Emitted only at
   statement/instruction boundaries and only when the (file, line) changes, so
   the generated C stays readable. The runtime header is pasted before any
   directive, so its own __FILE__/__LINE__ uses are unaffected. */
static const char *line_map_file;
static int line_map_no;

static void emit_line_directive(FILE *o, SourceLoc loc) {
    if (!loc.file || loc.line <= 0) return;
    int same_file = line_map_file && strcmp(line_map_file, loc.file) == 0;
    if (same_file && line_map_no == loc.line) return;
    if (same_file) {
        /* `#line N` keeps the current filename, so consecutive lines in one
           source file cost ~10 bytes instead of repeating the whole path. */
        fprintf(o, "#line %d\n", loc.line);
    } else {
        fprintf(o, "#line %d \"", loc.line);
        for (const char *p = loc.file; *p; p++) {
            if (*p == '\\' || *p == '"') fputc('\\', o);
            fputc(*p, o);
        }
        fprintf(o, "\"\n");
        line_map_file = loc.file;
    }
    line_map_no = loc.line;
}

static void emit_expr(FILE *o, AstNode *n);
static void emit_cond(FILE *o, AstNode *n);
static void emit_stmt(FILE *o, AstNode *n, int ind);
static void emit_block_body(FILE *o, AstNode *b, int ind, int is_main);
static IrFn *find_ir_fn(const char *cname);
static const char *c_assignop(int op);

/** C type for a Yuga Type (int64_t, yuga_str, yuga_fn, T*, …). */
static void emit_ctype(FILE *o, Type *t) {
    if (t && subst_n)
        t = typecheck_subst(t, subst_names, subst_args, subst_n);
    if (t && t->kind == TY_PARAM && subst_n) {
        for (size_t i = 0; i < subst_n; i++) {
            if (subst_names[i] && t->name && strcmp(subst_names[i], t->name) == 0 && subst_args[i]) {
                emit_ctype(o, subst_args[i]);
                return;
            }
        }
    }
    if (!t) {
        fprintf(o, "int64_t");
        return;
    }
    switch (t->kind) {
        case TY_INT:
            fprintf(o, "%s", type_c_scalar(t));
            break;
        case TY_FLOAT:
            fprintf(o, "%s", type_c_scalar(t));
            break;
        case TY_BOOL:
            fprintf(o, "bool");
            break;
        case TY_STRING:
            fprintf(o, "yuga_str");
            break;
        case TY_VOID:
            fprintf(o, "void");
            break;
        case TY_STRUCT: {
            char cn[256];
            type_c_name(t, cn, sizeof cn);
            fprintf(o, "%s", cn);
            break;
        }
        case TY_VEC:
            fprintf(o, "yuga_vec");
            break;
        case TY_PTR:
            if (!t->is_mut) fprintf(o, "const ");
            emit_ctype(o, t->elem);
            fprintf(o, " *");
            break;
        case TY_BOX:
            emit_ctype(o, t->elem);
            fprintf(o, " *");
            break;
        case TY_ARRAY:
            emit_ctype(o, t->elem);
            break;
        case TY_PROC:
            fprintf(o, "yuga_fn");
            break;
        default:
            fprintf(o, "int64_t");
            break;
    }
}

/** Same as emit_ctype, into a buffer (for casts and `_repl` temps). */
static void format_ctype(char *buf, size_t cap, Type *t) {
    if (!buf || cap == 0) return;
    if (t && subst_n)
        t = typecheck_subst(t, subst_names, subst_args, subst_n);
    if (t && t->kind == TY_PARAM && subst_n) {
        for (size_t i = 0; i < subst_n; i++) {
            if (subst_names[i] && t->name && strcmp(subst_names[i], t->name) == 0 && subst_args[i]) {
                format_ctype(buf, cap, subst_args[i]);
                return;
            }
        }
    }
    if (!t) {
        snprintf(buf, cap, "int64_t");
        return;
    }
    switch (t->kind) {
        case TY_INT:
            snprintf(buf, cap, "%s", type_c_scalar(t));
            break;
        case TY_FLOAT:
            snprintf(buf, cap, "%s", type_c_scalar(t));
            break;
        case TY_BOOL:
            snprintf(buf, cap, "bool");
            break;
        case TY_STRING:
            snprintf(buf, cap, "yuga_str");
            break;
        case TY_VOID:
            snprintf(buf, cap, "void");
            break;
        case TY_STRUCT:
            type_c_name(t, buf, cap);
            break;
        case TY_VEC:
            snprintf(buf, cap, "yuga_vec");
            break;
        case TY_PROC:
            snprintf(buf, cap, "yuga_fn");
            break;
        case TY_PTR:
        case TY_BOX: {
            char inner[256];
            format_ctype(inner, sizeof inner, t->elem);
            if (t->kind == TY_PTR && !t->is_mut)
                snprintf(buf, cap, "const %s *", inner);
            else
                snprintf(buf, cap, "%s *", inner);
            break;
        }
        case TY_ARRAY:
            format_ctype(buf, cap, t->elem);
            break;
        default:
            snprintf(buf, cap, "int64_t");
            break;
    }
}

static void emit_var_decl_type(FILE *o, Type *t, const char *name) {
    emit_ctype(o, t);
    fprintf(o, " %s", name);
    if (t && t->kind == TY_ARRAY) fprintf(o, "[%lld]", (long long)t->array_len);
}

static const char *src_file(AstNode *n) {
    return (n && n->loc.file) ? n->loc.file : "yuga";
}

/** File and line arguments for yuga_panic / yuga_idx / yuga_new. */
static void emit_loc_args(FILE *o, AstNode *n) {
    fprintf(o, "\"");
    const char *f = src_file(n);
    for (const char *p = f; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', o);
        fputc(*p, o);
    }
    fprintf(o, "\", %d", n ? n->loc.line : 0);
}

static int is_ptrish(Type *t) { return t && (t->kind == TY_PTR || t->kind == TY_BOX); }

static void emit_ident_name(FILE *o, const char *name) {
    if (emit_clos && name) {
        for (size_t i = 0; i < emit_clos->as.fn.cap_count; i++) {
            if (strcmp(emit_clos->as.fn.caps[i], name) == 0) {
                fprintf(o, "_e->%s", name);
                return;
            }
        }
    }
    fprintf(o, "%s", name);
}

/** L-value: ident (or _e->cap inside a closure), deref, field, checked index. */
static void emit_place(FILE *o, AstNode *n) {
    if (!n) {
        fprintf(o, "/*place*/");
        return;
    }
    switch (n->kind) {
        case AST_IDENT:
            emit_ident_name(o, n->as.ident.name);
            break;
        case AST_DEREF:
            fprintf(o, "(*");
            emit_expr(o, n->as.access.target);
            fprintf(o, ")");
            break;
        case AST_FIELD:
            if (is_ptrish(n->as.access.target->ty)) {
                fprintf(o, "(");
                emit_expr(o, n->as.access.target);
                fprintf(o, ")->%s", n->as.access.field);
            } else {
                emit_place(o, n->as.access.target);
                fprintf(o, ".%s", n->as.access.field);
            }
            break;
        case AST_INDEX: {
            Type *t = n->as.access.target->ty;
            if (t && (t->kind == TY_PTR || t->kind == TY_BOX)) t = t->elem;
            if (t && t->kind == TY_STRING) {
                fprintf(o, "((int64_t)(unsigned char)");
                emit_place(o, n->as.access.target);
                fprintf(o, ".ptr[");
                if (n->flags & ASTF_INDEX_SAFE)
                    emit_expr(o, n->as.access.index);
                else {
                    fprintf(o, "yuga_idx(");
                    emit_expr(o, n->as.access.index);
                    fprintf(o, ", ");
                    emit_place(o, n->as.access.target);
                    fprintf(o, ".len, ");
                    emit_loc_args(o, n);
                    fprintf(o, ")");
                }
                fprintf(o, "])");
                break;
            }
            emit_place(o, n->as.access.target);
            fprintf(o, "[");
            int64_t len = (t && t->kind == TY_ARRAY) ? t->array_len : 0;
            if (n->flags & ASTF_INDEX_SAFE)
                emit_expr(o, n->as.access.index);
            else {
                fprintf(o, "yuga_idx(");
                emit_expr(o, n->as.access.index);
                fprintf(o, ", %lld, ", (long long)len);
                emit_loc_args(o, n);
                fprintf(o, ")");
            }
            fprintf(o, "]");
            break;
        }
        default:
            emit_expr(o, n);
            break;
    }
}

static void emit_checked_bin(FILE *o, const char *fn, AstNode *n) {
    fprintf(o, "%s(", fn);
    emit_expr(o, n->as.binary.left);
    fprintf(o, ", ");
    emit_expr(o, n->as.binary.right);
    fprintf(o, ", ");
    emit_loc_args(o, n);
    fprintf(o, ")");
}

/** Trapping arithmetic helper for integer type `t` (`yuga_add_i32`). NULL for
 *  a non-integer type or a non-arithmetic operator. Rotating static buffer. */
static const char *arith_fn(const Type *t, int op) {
    const char *sfx = type_int_suffix(t);
    if (!sfx) return NULL;
    const char *base = NULL;
    switch ((TokenKind)op) {
        case TOK_PLUS: case TOK_PLUS_EQ: base = "add"; break;
        case TOK_MINUS: case TOK_MINUS_EQ: base = "sub"; break;
        case TOK_STAR: case TOK_STAR_EQ: base = "mul"; break;
        case TOK_SLASH: case TOK_SLASH_EQ: base = "div"; break;
        case TOK_PERCENT: case TOK_PERCENT_EQ: base = "mod"; break;
        default: return NULL;
    }
    static char bufs[6][32];
    static int rot;
    char *b = bufs[rot++ % 6];
    snprintf(b, 32, "yuga_%s_%s", base, sfx);
    return b;
}

/** Trapping shift helper (`yuga_shl_u8`). NULL if not an integer type. */
static const char *shift_fn(const Type *t, int op) {
    const char *sfx = type_int_suffix(t);
    if (!sfx) return NULL;
    static char bufs[4][32];
    static int rot;
    char *b = bufs[rot++ & 3];
    snprintf(b, 32, "yuga_%s_%s", op == TOK_SHR ? "shr" : "shl", sfx);
    return b;
}

/** Trapping negation helper (`yuga_neg_i16`). NULL if not an integer type. */
static const char *neg_fn(const Type *t) {
    const char *sfx = type_int_suffix(t);
    if (!sfx) return NULL;
    static char bufs[4][32];
    static int rot;
    char *b = bufs[rot++ & 3];
    snprintf(b, 32, "yuga_neg_%s", sfx);
    return b;
}

/* How a numeric conversion is emitted. `fn` receives the helper name. */
typedef enum {
    CAST_PLAIN = 0, /* (to)(src) */
    CAST_CONV,      /* fn((int64_t)(src), f, l) */
    CAST_CONVU,     /* fn((uint64_t)(src), f, l) */
    CAST_CONVF,     /* fn((double)(src), f, l) */
    CAST_SAT,       /* fn((int64_t)(src)) */
    CAST_SATU,      /* fn((uint64_t)(src)) */
} CastShape;

/** Classify a numeric cast and fill `fn` with the runtime helper to call. */
static CastShape cast_shape(const Type *from, const Type *to, int mode, char *fn, size_t fncap) {
    if (fncap) fn[0] = '\0';
    if (!from || !to) return CAST_PLAIN;
    if (to->kind != TY_INT) return CAST_PLAIN; /* to float / bool / same */
    const char *sfx = type_int_suffix(to);
    if (!sfx) return CAST_PLAIN;
    if (from->kind == TY_FLOAT) {
        if (mode != 0) return CAST_PLAIN;
        snprintf(fn, fncap, "yuga_convf_%s", sfx);
        return CAST_CONVF;
    }
    if (from->kind != TY_INT) return CAST_PLAIN;
    if (mode == 1) return CAST_PLAIN; /* wrapping: a plain truncating cast */
    int from_u64 = from->bits == 64 && from->is_unsigned;
    int to_u64 = to->bits == 64 && to->is_unsigned;
    if (from_u64 && !to_u64) {
        if (mode == 2) {
            snprintf(fn, fncap, "yuga_satu_%s", sfx);
            return CAST_SATU;
        }
        snprintf(fn, fncap, "yuga_convu_%s", sfx);
        return CAST_CONVU;
    }
    if (mode == 2) {
        snprintf(fn, fncap, "yuga_sat_%s", sfx);
        return CAST_SAT;
    }
    snprintf(fn, fncap, "yuga_conv_%s", sfx);
    return CAST_CONV;
}

static void emit_c_string_body(FILE *o, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"') {
            fputc('\\', o);
            fputc(c, o);
        } else if (c == '\n')
            fputs("\\n", o);
        else if (c == '\r')
            fputs("\\r", o);
        else if (c == '\t')
            fputs("\\t", o);
        else
            fputc(c, o);
    }
}

static void emit_float_lit(FILE *o, double f, int is_f32) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", f);
    int need_dot = !strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E');
    if (is_f32)
        fprintf(o, need_dot ? "%s.0f" : "%sf", buf);
    else
        fprintf(o, need_dot ? "%s.0" : "%s", buf);
}

/** yuga_str compound literal with compile-time length (no strlen). */
static void emit_str_lit(FILE *o, const char *s) {
    size_t n = s ? strlen(s) : 0;
    fprintf(o, "((yuga_str){(const char *)\"");
    emit_c_string_body(o, s);
    fprintf(o, "\", %lld})", (long long)n);
}

static int is_capturing_clos(AstNode *n) {
    return n && n->kind == AST_CLOSURE && n->as.fn.cap_count > 0;
}

static int call_has_mut_ref_arg(AstNode *n) {
    for (size_t i = 0; i < n->as.call.arg_count; i++) {
        Type *t = n->as.call.args[i] ? n->as.call.args[i]->ty : NULL;
        if (t && t->kind == TY_PTR && t->is_mut) return 1;
    }
    return 0;
}

/**
 * 1 if this call's capturing-closure args can live on the caller's stack:
 * the result cannot store a fn, and no &mut arg could stash one.
 */
static int can_stack_clos_args(AstNode *n) {
    if (!n || type_can_hold_fn(n->ty) || call_has_mut_ref_arg(n)) return 0;
    if (is_capturing_clos(n->as.call.callee)) return 1;
    for (size_t i = 0; i < n->as.call.arg_count; i++)
        if (is_capturing_clos(n->as.call.args[i])) return 1;
    return 0;
}

/** Declare a stack env for `clos` and record it for emit of that node. */
static int type_is_copy_vec(Type *t) {
    return t && t->kind == TY_VEC && type_is_copy(t);
}

/** Copy a capture into a closure env; []T must retain the buffer. */
static void emit_cap_set_ident(FILE *o, Type *ty, const char *dst_field, const char *src) {
    if (type_is_copy_vec(ty)) {
        fprintf(o, "%s = yuga_vec_retain(&", dst_field);
        emit_ident_name(o, src);
        fprintf(o, ")");
    } else {
        fprintf(o, "%s = ", dst_field);
        emit_ident_name(o, src);
    }
}

/** Declare a stack env for `clos` and record it for emit of that node. */
static void hoist_capturing_clos(FILE *o, AstNode *clos) {
    if (!is_capturing_clos(clos) || clos_hoist_n >= 64) return;
    int tid = tmp_id++;
    int id = clos->as.fn.clos_id;
    fprintf(o, "struct yuga_env_%d _ce%d; ", id, tid);
    for (size_t i = 0; i < clos->as.fn.cap_count; i++) {
        char dst[128];
        snprintf(dst, sizeof dst, "_ce%d.%s", tid, clos->as.fn.caps[i]);
        emit_cap_set_ident(o, clos->as.fn.cap_types[i], dst, clos->as.fn.caps[i]);
        fprintf(o, "; ");
    }
    clos_hoist_node[clos_hoist_n] = clos;
    clos_hoist_tid[clos_hoist_n] = tid;
    clos_hoist_n++;
}

static int hoist_call_clos_envs(FILE *o, AstNode *n) {
    int before = clos_hoist_n;
    hoist_capturing_clos(o, n->as.call.callee);
    for (size_t i = 0; i < n->as.call.arg_count; i++)
        hoist_capturing_clos(o, n->as.call.args[i]);
    return clos_hoist_n - before;
}

static int hoist_tid_for(AstNode *n) {
    for (int i = 0; i < clos_hoist_n; i++)
        if (clos_hoist_node[i] == n) return clos_hoist_tid[i];
    return -1;
}

/** malloc a Box and write the payload (GNU statement-expr). */
static void emit_box_new(FILE *o, AstNode *n) {
    Type *bt = n->ty;
    Type *et = (bt && bt->kind == TY_BOX) ? bt->elem : ty_int();
    int id = tmp_id++;
    fprintf(o, "({ ");
    emit_ctype(o, et);
    fprintf(o, " *_b%d = (", id);
    emit_ctype(o, et);
    fprintf(o, " *)yuga_new(sizeof(");
    emit_ctype(o, et);
    fprintf(o, "), ");
    emit_loc_args(o, n);
    fprintf(o, "); *_b%d = ", id);
    emit_expr(o, n->as.call.args[0]);
    fprintf(o, "; _b%d; })", id);
}

/** Lower fmt.println to one writev: strings by len, ints via stack itoa. */
static void emit_println(FILE *o, AstNode *n, int ind) {
    size_t nargs = n->as.call.arg_count;
    int cap = nargs == 0 ? 1 : (int)(2 * nargs);
    indent(o, ind);
    fprintf(o, "{\n");
    indent(o, ind + 1);
    fprintf(o, "struct iovec _iov[%d];\n", cap);
    indent(o, ind + 1);
    fprintf(o, "int _ni = 0;\n");
    for (size_t i = 0; i < nargs; i++) {
        if (i) {
            indent(o, ind + 1);
            fprintf(o, "_iov[_ni].iov_base = (void *)\" \"; _iov[_ni].iov_len = 1; _ni++;\n");
        }
        AstNode *a = n->as.call.args[i];
        Type *t = a->ty;
        if (t && t->kind == TY_STRING) {
            if (a->kind == AST_STRING) {
                size_t nlen = a->as.lit.str ? strlen(a->as.lit.str) : 0;
                indent(o, ind + 1);
                fprintf(o, "_iov[_ni].iov_base = (void *)\"");
                emit_c_string_body(o, a->as.lit.str);
                fprintf(o, "\"; _iov[_ni].iov_len = %llu; _ni++;\n",
                        (unsigned long long)nlen);
            } else {
                indent(o, ind + 1);
                fprintf(o, "yuga_str _ss%zu = ", i);
                emit_expr(o, a);
                fprintf(o, ";\n");
                indent(o, ind + 1);
                fprintf(o, "_iov[_ni].iov_base = (void *)(_ss%zu.ptr ? _ss%zu.ptr : \"\"); ", i, i);
                fprintf(o, "_iov[_ni].iov_len = _ss%zu.len > 0 ? (size_t)_ss%zu.len : 0; _ni++;\n", i, i);
            }
        } else if (t && t->kind == TY_BOOL) {
            indent(o, ind + 1);
            fprintf(o, "if (");
            emit_cond(o, a);
            fprintf(o, ") { _iov[_ni].iov_base = (void *)\"true\"; _iov[_ni].iov_len = 4; }");
            fprintf(o, " else { _iov[_ni].iov_base = (void *)\"false\"; _iov[_ni].iov_len = 5; } _ni++;\n");
        } else if (t && t->kind == TY_FLOAT) {
            indent(o, ind + 1);
            fprintf(o, "char _fb%zu[64]; yuga_str _fs%zu = yuga_fmt_ftoa(_fb%zu, ", i, i, i);
            emit_expr(o, a);
            fprintf(o, ");\n");
            indent(o, ind + 1);
            fprintf(o, "_iov[_ni].iov_base = (void *)_fs%zu.ptr; _iov[_ni].iov_len = (size_t)_fs%zu.len; _ni++;\n",
                    i, i);
        } else {
            indent(o, ind + 1);
            fprintf(o, "char _ib%zu[24]; yuga_str _is%zu = yuga_fmt_itoa(_ib%zu, ", i, i, i);
            emit_expr(o, a);
            fprintf(o, ");\n");
            indent(o, ind + 1);
            fprintf(o, "_iov[_ni].iov_base = (void *)_is%zu.ptr; _iov[_ni].iov_len = (size_t)_is%zu.len; _ni++;\n",
                    i, i);
        }
    }
    indent(o, ind + 1);
    fprintf(o, "_iov[_ni].iov_base = (void *)\"\\n\"; _iov[_ni].iov_len = 1; _ni++;\n");
    indent(o, ind + 1);
    fprintf(o, "yuga_writev_all(_iov, _ni);\n");
    indent(o, ind);
    fprintf(o, "}\n");
}

static const char *cname_for_call(AstNode *cal) {
    static char buf[256];
    if (!cal) return "unknown";
    if (cal->kind == AST_IDENT) {
        /* Honor the binding typecheck recorded (a default cloned into another
           module must not re-resolve to a same-named fn there). */
        if (cal->as.ident.resolved && cal->as.ident.resolved->kind == AST_FN_DECL &&
            cal->as.ident.resolved->as.fn.cname)
            return cal->as.ident.resolved->as.fn.cname;
        if (cur_is_main_mod)
            snprintf(buf, sizeof buf, "yuga_%s", cal->as.ident.name);
        else
            snprintf(buf, sizeof buf, "yuga_%s_%s",
                     cur_mod_name ? cur_mod_name : "mod", cal->as.ident.name);
        return buf;
    }
    if (cal->kind == AST_FIELD && cal->as.access.target &&
        cal->as.access.target->kind == AST_IDENT) {
        snprintf(buf, sizeof buf, "yuga_%s_%s", cal->as.access.target->as.ident.name,
                 cal->as.access.field);
        return buf;
    }
    return "unknown";
}

/** Call through a yuga_fn fat pointer (env first argument). */
static void emit_fn_val_call(FILE *o, AstNode *n) {
    Type *ft = n->as.call.callee ? n->as.call.callee->ty : NULL;
    if (!ft || ft->kind != TY_PROC) ft = n->ty;
    Type *ret = ty_void();
    if (ft && ft->kind == TY_PROC && ft->ret) ret = ft->ret;
    int stack = can_stack_clos_args(n);
    int pushed = 0;
    fprintf(o, "({ ");
    if (stack) pushed = hoist_call_clos_envs(o, n);
    int id = tmp_id++;
    fprintf(o, "yuga_fn _fv%d = ", id);
    emit_expr(o, n->as.call.callee);
    fprintf(o, "; ((");
    emit_ctype(o, ret);
    fprintf(o, "(*)(void *");
    if (ft && ft->kind == TY_PROC) {
        for (size_t i = 0; i < ft->param_count; i++) {
            fprintf(o, ", ");
            emit_ctype(o, ft->params[i]);
        }
    }
    fprintf(o, "))_fv%d.fn)(_fv%d.env", id, id);
    for (size_t i = 0; i < n->as.call.arg_count; i++) {
        fprintf(o, ", ");
        emit_expr(o, n->as.call.args[i]);
    }
    clos_hoist_n -= pushed;
    if (ret->kind == TY_VOID)
        fprintf(o, "); 0; })");
    else
        fprintf(o, "); })");
}

/** Expression as a C rvalue (GNU statement-exprs for Box, closures, fn vals). */
/** One arm of an `if` used as a value: run its statements, then assign the
 *  trailing expression to the result temporary. */
static void emit_if_branch(FILE *o, AstNode *blk, int id) {
    fprintf(o, "{ ");
    if (blk && blk->kind == AST_BLOCK) {
        size_t c = blk->as.block.stmt_count;
        for (size_t i = 0; i < c; i++) {
            AstNode *st = blk->as.block.stmts[i];
            if (i + 1 == c && st && st->kind == AST_EXPR_STMT) {
                fprintf(o, "_iv%d = ", id);
                emit_expr(o, st->as.expr_stmt.expr);
                fprintf(o, "; ");
                continue;
            }
            emit_stmt(o, st, 0);
        }
    }
    fprintf(o, "} ");
}

static void emit_expr(FILE *o, AstNode *n) {
    if (!n) {
        fprintf(o, "0");
        return;
    }
    switch (n->kind) {
        case AST_IF: {
            /* value position: a statement expression with a result temporary */
            int id = tmp_id++;
            fprintf(o, "({ ");
            emit_ctype(o, n->ty);
            fprintf(o, " _iv%d; if (", id);
            emit_cond(o, n->as.if_stmt.cond);
            fprintf(o, ") ");
            emit_if_branch(o, n->as.if_stmt.then_block, id);
            fprintf(o, "else ");
            emit_if_branch(o, n->as.if_stmt.else_block, id);
            fprintf(o, "_iv%d; })", id);
            break;
        }

        case AST_NUMBER:
            fprintf(o, "%lld", (long long)n->as.lit.value);
            break;
        case AST_FLOAT:
            emit_float_lit(o, n->as.lit.f, n->ty && n->ty->kind == TY_FLOAT && n->ty->bits == 32);
            break;
        case AST_BOOL:
            fprintf(o, "%s", n->as.lit.b ? "true" : "false");
            break;
        case AST_STRING:
            emit_str_lit(o, n->as.lit.str);
            break;
        case AST_IDENT:
            if (n->flags & ASTF_FN_VAL) {
                const char *cn = cname_for_call(n);
                fprintf(o, "((yuga_fn){(void *)%s__as_fn, NULL, 0})", cn);
            } else if ((n->flags & ASTF_MOVED) && n->ty && n->ty->kind == TY_BOX) {
                fprintf(o, "((");
                emit_ctype(o, n->ty);
                fprintf(o, ")yuga_move_ptr((void **)&");
                emit_ident_name(o, n->as.ident.name);
                fprintf(o, "))");
            } else {
                emit_ident_name(o, n->as.ident.name);
            }
            break;
        case AST_BINARY: {
            TokenKind op = n->as.binary.op;
            int is_f = n->ty && n->ty->kind == TY_FLOAT;
            if (type_is_int_kind(n->ty) && (op == TOK_SHL || op == TOK_SHR))
                emit_checked_bin(o, shift_fn(n->ty, op), n);
            else if (!is_f && op == TOK_PLUS) emit_checked_bin(o, arith_fn(n->ty, op), n);
            else if (!is_f && op == TOK_MINUS) emit_checked_bin(o, arith_fn(n->ty, op), n);
            else if (!is_f && op == TOK_STAR) emit_checked_bin(o, arith_fn(n->ty, op), n);
            else if (!is_f && op == TOK_SLASH) emit_checked_bin(o, arith_fn(n->ty, op), n);
            else if (!is_f && op == TOK_PERCENT) emit_checked_bin(o, arith_fn(n->ty, op), n);
            else if (n->as.binary.left && n->as.binary.left->ty &&
                     n->as.binary.left->ty->kind == TY_STRING &&
                     (op == TOK_EQ_EQ || op == TOK_BANG_EQ)) {
                fprintf(o, "%syuga_fmt_eq(", op == TOK_BANG_EQ ? "!" : "");
                emit_expr(o, n->as.binary.left);
                fprintf(o, ", ");
                emit_expr(o, n->as.binary.right);
                fprintf(o, ")");
            } else {
                const char *s = "?";
                if (op == TOK_PLUS) s = "+";
                else if (op == TOK_MINUS) s = "-";
                else if (op == TOK_STAR) s = "*";
                else if (op == TOK_SLASH) s = "/";
                else if (op == TOK_PERCENT) s = "%";
                else if (op == TOK_EQ_EQ) s = "==";
                else if (op == TOK_BANG_EQ) s = "!=";
                else if (op == TOK_LT) s = "<";
                else if (op == TOK_GT) s = ">";
                else if (op == TOK_LT_EQ) s = "<=";
                else if (op == TOK_GT_EQ) s = ">=";
                else if (op == TOK_AMP_AMP) s = "&&";
                else if (op == TOK_PIPE_PIPE) s = "||";
                else if (op == TOK_AMP) s = "&";
                else if (op == TOK_PIPE) s = "|";
                else if (op == TOK_CARET) s = "^";
                else if (op == TOK_SHL) s = "<<";
                else if (op == TOK_SHR) s = ">>";
                fprintf(o, "(");
                emit_expr(o, n->as.binary.left);
                fprintf(o, " %s ", s);
                emit_expr(o, n->as.binary.right);
                fprintf(o, ")");
            }
            break;
        }
        case AST_UNARY: {
            TokenKind uop = n->as.unary.op;
            if (uop == TOK_BANG) {
                fprintf(o, "(!");
                emit_expr(o, n->as.unary.operand);
                fprintf(o, ")");
            } else if (uop == TOK_TILDE) {
                fprintf(o, "((");
                emit_ctype(o, n->ty);
                fprintf(o, ")(~");
                emit_expr(o, n->as.unary.operand);
                fprintf(o, "))");
            } else if (type_is_int_kind(n->ty)) {
                fprintf(o, "%s(", neg_fn(n->ty));
                emit_expr(o, n->as.unary.operand);
                fprintf(o, ", ");
                emit_loc_args(o, n);
                fprintf(o, ")");
            } else {
                fprintf(o, "(-");
                emit_expr(o, n->as.unary.operand);
                fprintf(o, ")");
            }
            break;
        }
        case AST_INCDEC: {
            /* Read-modify-write of the operand place. GCC/Clang statement
               expression: the place is addressed once (side effects in an
               index or field base run once). */
            fprintf(o, "({ ");
            emit_ctype(o, n->ty);
            fprintf(o, " *_p = &");
            emit_place(o, n->as.incdec.operand);
            fprintf(o, "; ");
            if (n->as.incdec.is_post) {
                emit_ctype(o, n->ty);
                fprintf(o, " _old = *_p; ");
            }
            if (type_is_int_kind(n->ty)) {
                fprintf(o, "*_p = %s(*_p, (", arith_fn(n->ty, n->as.incdec.is_dec ? TOK_MINUS : TOK_PLUS));
                emit_ctype(o, n->ty);
                fprintf(o, ")1, ");
                emit_loc_args(o, n);
                fprintf(o, "); ");
            } else {
                fprintf(o, "*_p = *_p ");
                fprintf(o, n->as.incdec.is_dec ? "- 1; " : "+ 1; ");
            }
            fprintf(o, n->as.incdec.is_post ? "_old; })" : "*_p; })");
            break;
        }
        case AST_CAST: {
            Type *from = n->as.cast.expr ? n->as.cast.expr->ty : NULL;
            char fn[32];
            CastShape cs = cast_shape(from, n->ty, n->as.cast.conv_mode, fn, sizeof fn);
            if (cs == CAST_PLAIN) {
                fprintf(o, "((");
                emit_ctype(o, n->ty);
                fprintf(o, ")");
                emit_expr(o, n->as.cast.expr);
                fprintf(o, ")");
            } else {
                fprintf(o, "%s(", fn);
                if (cs == CAST_CONV || cs == CAST_SAT)
                    fprintf(o, "(int64_t)");
                else if (cs == CAST_CONVU || cs == CAST_SATU)
                    fprintf(o, "(uint64_t)");
                else
                    fprintf(o, "(double)");
                emit_expr(o, n->as.cast.expr);
                if (cs == CAST_CONV || cs == CAST_CONVU || cs == CAST_CONVF) {
                    fprintf(o, ", ");
                    emit_loc_args(o, n);
                }
                fprintf(o, ")");
            }
            break;
        }
        case AST_ADDR:
            fprintf(o, "&");
            emit_place(o, n->as.access.target);
            break;
        case AST_DEREF:
            fprintf(o, "(*");
            emit_expr(o, n->as.access.target);
            fprintf(o, ")");
            break;
        case AST_FIELD:
            emit_place(o, n);
            break;
        case AST_INDEX:
            emit_place(o, n);
            break;
        case AST_CALL:
            if (n->as.call.is_box_new) {
                emit_box_new(o, n);
                break;
            }
            if (n->as.call.sig_cell == 1 && n->as.call.arg_count == 1) {
                fprintf(o, "({ yuga_arena_ensure(); int64_t _sid = ");
                if (n->as.call.args[0]->ty && n->as.call.args[0]->ty->kind == TY_INT &&
                    n->as.call.args[0]->ty->bits == 32) {
                    fprintf(o, "yuga_zeus_sig_alloc_int(");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, "); ");
                } else {
                    fprintf(o, "yuga_zeus_sig_alloc_zero(); ");
                    emit_ctype(o, n->as.call.args[0]->ty);
                    fprintf(o, " _sv = ");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, "; ");
                    if (type_is_copy_vec(n->as.call.args[0]->ty))
                        fprintf(o, "{ yuga_vec _kept = yuga_vec_retain(&_sv); yuga_zeus_sig_bind(_sid, &_kept, sizeof(_kept)); } ");
                    else
                        fprintf(o, "yuga_zeus_sig_bind(_sid, &_sv, sizeof(_sv)); ");
                }
                fprintf(o, "_sid; })");
                break;
            }
            if (n->as.call.sig_cell == 2 && n->as.call.arg_count == 1) {
                fprintf(o, "({ ");
                emit_ctype(o, n->ty);
                fprintf(o, " _sv; yuga_zeus_sig_load(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ", &_sv, sizeof(_sv)); ");
                if (type_is_copy_vec(n->ty))
                    fprintf(o, "_sv = yuga_vec_retain(&_sv); ");
                fprintf(o, "_sv; })");
                break;
            }
            if (n->as.call.sig_cell == 3 && n->as.call.arg_count == 2) {
                Type *vt = n->as.call.args[1]->ty;
                if (vt && vt->kind == TY_INT && vt->bits == 32) {
                    fprintf(o, "(yuga_arena_store_sig(");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, ", ");
                    emit_expr(o, n->as.call.args[1]);
                    fprintf(o, "), 0)");
                } else if (type_is_copy_vec(vt)) {
                    fprintf(o, "({ yuga_vec _sv = ");
                    emit_expr(o, n->as.call.args[1]);
                    fprintf(o, "; int64_t _sid = ");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, "; if (yuga_zeus_sig_changed(_sid, &_sv, sizeof(_sv))) { "
                               "yuga_vec _old; yuga_zeus_sig_load(_sid, &_old, sizeof(_old)); "
                               "_old = yuga_vec_retain(&_old); "
                               "yuga_vec _new = yuga_vec_retain(&_sv); "
                               "yuga_zeus_sig_bind(_sid, &_new, sizeof(_new)); "
                               "yuga_vec_drop(&_old); yuga_track_notify(_sid); } 0; })");
                } else {
                    fprintf(o, "({ ");
                    emit_ctype(o, vt);
                    fprintf(o, " _sv = ");
                    emit_expr(o, n->as.call.args[1]);
                    fprintf(o, "; int64_t _sid = ");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, "; if (yuga_zeus_sig_changed(_sid, &_sv, sizeof(_sv))) { "
                               "yuga_zeus_sig_bind(_sid, &_sv, sizeof(_sv)); yuga_track_notify(_sid); "
                               "} 0; })");
                }
                break;
            }
            if (n->as.call.fut_cell == 1 && n->as.call.arg_count == 1) {
                Type *vt = n->as.call.args[0]->ty;
                fprintf(o, "({ ");
                if (type_is_copy_vec(vt)) {
                    fprintf(o, "yuga_vec _fv = yuga_vec_retain(&");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, "); yuga_fut_push(&_fv, sizeof(_fv)); })");
                } else {
                    emit_ctype(o, vt);
                    fprintf(o, " _fv = ");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, "; yuga_fut_push(&_fv, sizeof(_fv)); })");
                }
                break;
            }
            if (n->as.call.fut_cell == 2 && n->as.call.arg_count == 1) {
                fprintf(o, "({ ");
                emit_ctype(o, n->ty);
                fprintf(o, " _fv; yuga_fut_load(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ", &_fv, sizeof(_fv)); ");
                if (type_is_copy_vec(n->ty))
                    fprintf(o, "_fv = yuga_vec_retain(&_fv); ");
                fprintf(o, "_fv; })");
                break;
            }
            if (n->as.call.fut_cell == 3 && n->as.call.arg_count == 2) {
                Type *vt = n->as.call.args[1]->ty;
                if (type_is_copy_vec(vt)) {
                    fprintf(o, "({ int64_t _fid = ");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, "; yuga_vec _old; yuga_fut_load(_fid, &_old, sizeof(_old)); "
                               "yuga_vec _new = yuga_vec_retain(&");
                    emit_expr(o, n->as.call.args[1]);
                    fprintf(o, "); yuga_fut_store(_fid, &_new, sizeof(_new)); "
                               "yuga_vec_drop(&_old); 0; })");
                } else {
                    fprintf(o, "({ ");
                    emit_ctype(o, vt);
                    fprintf(o, " _fv = ");
                    emit_expr(o, n->as.call.args[1]);
                    fprintf(o, "; yuga_fut_store(");
                    emit_expr(o, n->as.call.args[0]);
                    fprintf(o, ", &_fv, sizeof(_fv)); 0; })");
                }
                break;
            }
            if (n->as.call.fut_cell == 4 && n->as.call.arg_count == 1) {
                fprintf(o, "yuga_fut_ready(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ")");
                break;
            }
            if (n->as.call.fut_cell == 5 && n->as.call.arg_count == 1) {
                fprintf(o, "(yuga_fut_clear(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, "), 0)");
                break;
            }
            if (n->as.call.ch_cell == 1 && n->as.call.arg_count == 2) {
                Type *vt = n->as.call.args[1]->ty;
                fprintf(o, "({ ");
                emit_ctype(o, vt);
                fprintf(o, " _cv = ");
                emit_expr(o, n->as.call.args[1]);
                fprintf(o, "; yuga_ch_alloc(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ", sizeof(_cv)); })");
                break;
            }
            if (n->as.call.ch_cell == 2 && n->as.call.arg_count == 2) {
                Type *vt = n->as.call.args[1]->ty;
                fprintf(o, "({ ");
                emit_ctype(o, vt);
                fprintf(o, " _cv = ");
                emit_expr(o, n->as.call.args[1]);
                fprintf(o, "; yuga_ch_send(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ", &_cv, sizeof(_cv)); 0; })");
                break;
            }
            if (n->as.call.ch_cell == 3 && n->as.call.arg_count == 2) {
                Type *vt = n->as.call.args[1]->ty;
                fprintf(o, "({ ");
                emit_ctype(o, vt);
                fprintf(o, " _cv = ");
                emit_expr(o, n->as.call.args[1]);
                fprintf(o, "; yuga_ch_try_send(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ", &_cv, sizeof(_cv)); })");
                break;
            }
            if (n->as.call.ch_cell == 4 && n->as.call.arg_count == 1) {
                fprintf(o, "({ ");
                emit_ctype(o, n->ty);
                fprintf(o, " _cv; yuga_ch_recv(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ", &_cv, sizeof(_cv)); _cv; })");
                break;
            }
            if (n->as.call.ch_cell == 5 && n->as.call.arg_count == 1) {
                fprintf(o, "({ ");
                emit_ctype(o, n->ty);
                fprintf(o, " _cv; yuga_ch_pop(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ", &_cv, sizeof(_cv)); _cv; })");
                break;
            }
            if (n->as.call.ch_cell == 6 && n->as.call.arg_count == 1) {
                fprintf(o, "yuga_ch_ready(");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ")");
                break;
            }
            if (n->as.call.is_sizeof && n->as.call.arg_count == 1) {
                Type *at = n->as.call.args[0]->ty;
                size_t nel = (at && at->kind == TY_ARRAY && at->array_len > 0)
                                 ? (size_t)at->array_len
                                 : 1;
                fprintf(o, "((int32_t)(sizeof(");
                emit_ctype(o, at && at->kind == TY_ARRAY ? at->elem : at);
                fprintf(o, ") * %zu))", nel);
                break;
            }
            if (n->as.call.is_panic && n->as.call.arg_count == 1) {
                fprintf(o, "yuga_panic_str(");
                emit_loc_args(o, n);
                fprintf(o, ", ");
                emit_expr(o, n->as.call.args[0]);
                fprintf(o, ")");
                break;
            }
            if (n->as.call.num_builtin != NUMB_NONE) {
                fprintf(o, "%s(", numeric_builtin_cname(n->as.call.num_builtin, n->ty));
                for (size_t k = 0; k < n->as.call.arg_count; k++) {
                    if (k) fprintf(o, ", ");
                    emit_expr(o, n->as.call.args[k]);
                }
                fprintf(o, ")");
                break;
            }
            if (n->as.call.c_builtin) {
                fprintf(o, "%s(", n->as.call.c_builtin);
                for (size_t k = 0; k < n->as.call.arg_count; k++) {
                    if (k) fprintf(o, ", ");
                    emit_expr(o, n->as.call.args[k]);
                }
                fprintf(o, ")");
                break;
            }
            if (n->as.call.is_println) {
                fprintf(o, "0 /* println as expr */");
                break;
            }
            if (n->as.call.is_fn_val) {
                emit_fn_val_call(o, n);
                break;
            }
            {
                int stack = can_stack_clos_args(n);
                int pushed = 0;
                if (stack) {
                    fprintf(o, "({ ");
                    pushed = hoist_call_clos_envs(o, n);
                }
                {
                    const char *cn = NULL;
                    if (subst_n)
                        cn = typecheck_callee_cname(n, subst_names, subst_args, subst_n);
                    if (!cn) cn = n->as.call.resolved_cname;
                    if (!cn) cn = cname_for_call(n->as.call.callee);
                    fprintf(o, "%s(", cn ? cn : "unknown");
                }
                for (size_t i = 0; i < n->as.call.arg_count; i++) {
                    if (i) fprintf(o, ", ");
                    emit_expr(o, n->as.call.args[i]);
                }
                fprintf(o, ")");
                if (stack) {
                    clos_hoist_n -= pushed;
                    fprintf(o, "; })");
                }
            }
            break;
        case AST_STRUCT_LIT: {
            fprintf(o, "(%s){ ", n->as.struct_lit.type_name);
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++) {
                if (i) fprintf(o, ", ");
                fprintf(o, ".%s = ", n->as.struct_lit.fields[i].name);
                emit_expr(o, n->as.struct_lit.fields[i].init);
            }
            fprintf(o, " }");
            break;
        }
        case AST_ARRAY_LIT: {
            /* A fixed-size `[N]T{...}` gets a compound-literal cast so it's a
             * valid expression anywhere (`return [3]int{...}[i]`, a call arg,
             * …), not only as a `let` initializer. Bare `{ ... }` is only
             * legal in that one declaration position. `[]T{...}` (TY_VEC) is
             * a different runtime representation and unaffected. */
            if (n->ty && n->ty->kind == TY_ARRAY) {
                fprintf(o, "(");
                emit_ctype(o, n->ty->elem);
                fprintf(o, "[%lld]", (long long)n->ty->array_len);
                fprintf(o, "){ ");
            } else {
                fprintf(o, "{ ");
            }
            for (size_t i = 0; i < n->as.array_lit.count; i++) {
                if (i) fprintf(o, ", ");
                emit_expr(o, n->as.array_lit.elems[i]);
            }
            fprintf(o, " }");
            break;
        }
        case AST_CLOSURE: {
            int id = n->as.fn.clos_id;
            int hid = hoist_tid_for(n);
            if (hid >= 0) {
                fprintf(o, "((yuga_fn){(void *)yuga_clos_%d, &_ce%d, sizeof(struct yuga_env_%d)})",
                        id, hid, id);
                break;
            }
            if (!n->as.fn.cap_count) {
                fprintf(o, "((yuga_fn){(void *)yuga_clos_%d, NULL, 0})", id);
                break;
            }
            int tid = tmp_id++;
            fprintf(o, "({ struct yuga_env_%d *_ce%d = (struct yuga_env_%d *)yuga_new(sizeof(struct yuga_env_%d), ",
                    id, tid, id, id);
            emit_loc_args(o, n);
            fprintf(o, "); ");
            for (size_t i = 0; i < n->as.fn.cap_count; i++) {
                char dst[128];
                snprintf(dst, sizeof dst, "_ce%d->%s", tid, n->as.fn.caps[i]);
                emit_cap_set_ident(o, n->as.fn.cap_types[i], dst, n->as.fn.caps[i]);
                fprintf(o, "; ");
            }
            fprintf(o, "(yuga_fn){(void *)yuga_clos_%d, _ce%d, sizeof(struct yuga_env_%d)}; })",
                    id, tid, id);
            break;
        }
        default:
            fprintf(o, "0");
            break;
    }
}

/* Comparisons parenthesize in emit_expr so they nest safely. `if ((x == 0))`
   trips clang -Wparentheses-equality, so if-conditions omit that wrapper. */
static void emit_cond(FILE *o, AstNode *n) {
    if (n && n->kind == AST_BINARY) {
        TokenKind op = n->as.binary.op;
        const char *s = NULL;
        if (op == TOK_EQ_EQ) s = "==";
        else if (op == TOK_BANG_EQ) s = "!=";
        else if (op == TOK_LT) s = "<";
        else if (op == TOK_GT) s = ">";
        else if (op == TOK_LT_EQ) s = "<=";
        else if (op == TOK_GT_EQ) s = ">=";
        else if (op == TOK_AMP_AMP) s = "&&";
        else if (op == TOK_PIPE_PIPE) s = "||";
        if (s) {
            emit_expr(o, n->as.binary.left);
            fprintf(o, " %s ", s);
            emit_expr(o, n->as.binary.right);
            return;
        }
    }
    emit_expr(o, n);
}

/** yuga_drop Box bindings recorded for this scope, reverse order. */
static void emit_drops_scope(FILE *o, int ind, int sp) {
    for (int i = drop_n[sp] - 1; i >= 0; i--) {
        indent(o, ind);
        fprintf(o, "yuga_drop((void **)&%s);\n", drop_names[sp][i]);
    }
}

static void emit_drops_all(FILE *o, int ind) {
    for (int s = drop_sp; s >= 1; s--) emit_drops_scope(o, ind, s);
}

static void emit_assign(FILE *o, AstNode *n, int ind) {
    indent(o, ind);
    if (n->as.assign.op == TOK_EQ) {
        emit_place(o, n->as.assign.left);
        fprintf(o, " = ");
        emit_expr(o, n->as.assign.right);
        fprintf(o, ";\n");
        return;
    }
    Type *lt = n->as.assign.left ? n->as.assign.left->ty : NULL;
    if (type_is_int_kind(lt) &&
        (n->as.assign.op == TOK_SHL_EQ || n->as.assign.op == TOK_SHR_EQ)) {
        emit_place(o, n->as.assign.left);
        fprintf(o, " = %s(", shift_fn(lt, n->as.assign.op == TOK_SHR_EQ ? TOK_SHR : TOK_SHL));
        emit_place(o, n->as.assign.left);
        fprintf(o, ", ");
        emit_expr(o, n->as.assign.right);
        fprintf(o, ", ");
        emit_loc_args(o, n);
        fprintf(o, ");\n");
        return;
    }
    const char *aop = c_assignop(n->as.assign.op);
    if (aop) {
        emit_place(o, n->as.assign.left);
        fprintf(o, " %s ", aop);
        emit_expr(o, n->as.assign.right);
        fprintf(o, ";\n");
        return;
    }
    if (lt && lt->kind == TY_FLOAT) {
        const char *op = "+=";
        if (n->as.assign.op == TOK_MINUS_EQ) op = "-=";
        else if (n->as.assign.op == TOK_STAR_EQ) op = "*=";
        else if (n->as.assign.op == TOK_SLASH_EQ) op = "/=";
        emit_place(o, n->as.assign.left);
        fprintf(o, " %s ", op);
        emit_expr(o, n->as.assign.right);
        fprintf(o, ";\n");
        return;
    }
    const char *fn = arith_fn(lt, (int)n->as.assign.op);
    if (!fn) fn = "yuga_add_i64";
    emit_place(o, n->as.assign.left);
    fprintf(o, " = %s(", fn);
    emit_place(o, n->as.assign.left);
    fprintf(o, ", ");
    emit_expr(o, n->as.assign.right);
    fprintf(o, ", ");
    emit_loc_args(o, n);
    fprintf(o, ");\n");
}

/** Statement: let (including stack closure env), assign, if, for, return, … */
static void emit_stmt(FILE *o, AstNode *n, int ind) {
    if (!n) return;
    emit_line_directive(o, n->loc);
    switch (n->kind) {
        case AST_VAR_DECL: {
            if (n->as.var.init && n->as.var.init->kind == AST_CLOSURE &&
                n->as.var.init->as.fn.cap_count) {
                AstNode *cl = n->as.var.init;
                int id = cl->as.fn.clos_id;
                int tid = tmp_id++;
                indent(o, ind);
                fprintf(o, "struct yuga_env_%d _ce%d;\n", id, tid);
                for (size_t i = 0; i < cl->as.fn.cap_count; i++) {
                    indent(o, ind);
                    {
                        char dst[128];
                        snprintf(dst, sizeof dst, "_ce%d.%s", tid, cl->as.fn.caps[i]);
                        emit_cap_set_ident(o, cl->as.fn.cap_types[i], dst, cl->as.fn.caps[i]);
                    }
                    fprintf(o, ";\n");
                }
                indent(o, ind);
                emit_var_decl_type(o, n->ty, n->as.var.name);
                fprintf(o, " = (yuga_fn){(void *)yuga_clos_%d, &_ce%d, sizeof(struct yuga_env_%d)};\n",
                        id, tid, id);
                break;
            }
            indent(o, ind);
            emit_var_decl_type(o, n->ty, n->as.var.name);
            if (n->as.var.init && n->as.var.init->kind == AST_CALL &&
                n->as.var.init->as.call.is_box_new) {
                fprintf(o, " = (");
                emit_ctype(o, n->ty);
                fprintf(o, ")yuga_new(sizeof(");
                emit_ctype(o, n->ty->elem);
                fprintf(o, "), ");
                emit_loc_args(o, n);
                fprintf(o, ");\n");
                indent(o, ind);
                fprintf(o, "*%s = ", n->as.var.name);
                emit_expr(o, n->as.var.init->as.call.args[0]);
                fprintf(o, ";\n");
            } else {
                fprintf(o, " = ");
                if (n->as.var.init) emit_expr(o, n->as.var.init);
                else fprintf(o, "{0}");
                fprintf(o, ";\n");
            }
            if (n->ty && n->ty->kind == TY_BOX && drop_n[drop_sp] < 64)
                drop_names[drop_sp][drop_n[drop_sp]++] = n->as.var.name;
            break;
        }
        case AST_ASSIGN:
            emit_assign(o, n, ind);
            break;
        case AST_EXPR_STMT:
            if (n->as.expr_stmt.expr && n->as.expr_stmt.expr->kind == AST_CALL &&
                n->as.expr_stmt.expr->as.call.is_println) {
                emit_println(o, n->as.expr_stmt.expr, ind);
                break;
            }
            indent(o, ind);
            emit_expr(o, n->as.expr_stmt.expr);
            fprintf(o, ";\n");
            break;
        case AST_RETURN:
            emit_drops_all(o, ind);
            indent(o, ind);
            fprintf(o, "return");
            if (n->as.ret.expr) {
                fprintf(o, " ");
                emit_expr(o, n->as.ret.expr);
            }
            fprintf(o, ";\n");
            break;
        case AST_IF:
            indent(o, ind);
            fprintf(o, "if (");
            emit_cond(o, n->as.if_stmt.cond);
            fprintf(o, ") {\n");
            drop_sp++;
            drop_n[drop_sp] = 0;
            if (n->as.if_stmt.then_block && n->as.if_stmt.then_block->kind == AST_BLOCK)
                emit_block_body(o, n->as.if_stmt.then_block, ind + 1, 0);
            emit_drops_scope(o, ind + 1, drop_sp);
            drop_sp--;
            indent(o, ind);
            fprintf(o, "}");
            if (n->as.if_stmt.else_block) {
                fprintf(o, " else ");
                if (n->as.if_stmt.else_block->kind == AST_IF) {
                    fprintf(o, "\n");
                    emit_stmt(o, n->as.if_stmt.else_block, ind);
                    break;
                }
                fprintf(o, "{\n");
                drop_sp++;
                drop_n[drop_sp] = 0;
                if (n->as.if_stmt.else_block->kind == AST_BLOCK)
                    emit_block_body(o, n->as.if_stmt.else_block, ind + 1, 0);
                emit_drops_scope(o, ind + 1, drop_sp);
                drop_sp--;
                indent(o, ind);
                fprintf(o, "}\n");
            } else {
                fprintf(o, "\n");
            }
            break;
        case AST_FOR: {
            int id = tmp_id++;
            indent(o, ind);
            fprintf(o, "{\n");
            indent(o, ind + 1);
            fprintf(o, "int64_t _lo%d = ", id);
            emit_expr(o, n->as.for_stmt.iter->as.binary.left);
            fprintf(o, ";\n");
            indent(o, ind + 1);
            fprintf(o, "int64_t _hi%d = ", id);
            emit_expr(o, n->as.for_stmt.iter->as.binary.right);
            fprintf(o, ";\n");
            indent(o, ind + 1);
            fprintf(o, "for (int64_t %s = _lo%d; %s < _hi%d; %s = yuga_add_i64(%s, 1, ",
                    n->as.for_stmt.var, id, n->as.for_stmt.var, id, n->as.for_stmt.var,
                    n->as.for_stmt.var);
            emit_loc_args(o, n);
            fprintf(o, ")) {\n");
            drop_sp++;
            drop_n[drop_sp] = 0;
            if (n->as.for_stmt.body && n->as.for_stmt.body->kind == AST_BLOCK)
                emit_block_body(o, n->as.for_stmt.body, ind + 2, 0);
            emit_drops_scope(o, ind + 2, drop_sp);
            drop_sp--;
            indent(o, ind + 1);
            fprintf(o, "}\n");
            indent(o, ind);
            fprintf(o, "}\n");
            break;
        }
        case AST_WHILE:
            indent(o, ind);
            fprintf(o, "while (");
            emit_cond(o, n->as.if_stmt.cond);
            fprintf(o, ") {\n");
            drop_sp++;
            drop_n[drop_sp] = 0;
            if (n->as.if_stmt.then_block && n->as.if_stmt.then_block->kind == AST_BLOCK)
                emit_block_body(o, n->as.if_stmt.then_block, ind + 1, 0);
            emit_drops_scope(o, ind + 1, drop_sp);
            drop_sp--;
            indent(o, ind);
            fprintf(o, "}\n");
            break;
        case AST_BREAK:
            indent(o, ind);
            fprintf(o, "break;\n");
            break;
        case AST_CONTINUE:
            indent(o, ind);
            fprintf(o, "continue;\n");
            break;
        case AST_MATCH: {
            indent(o, ind);
            fprintf(o, "{\n");
            indent(o, ind + 1);
            emit_var_decl_type(o, n->as.match_stmt.scrut->ty, "_m");
            fprintf(o, " = ");
            emit_expr(o, n->as.match_stmt.scrut);
            fprintf(o, ";\n");
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++) {
                AstNode *arm = n->as.match_stmt.arms[i];
                indent(o, ind + 1);
                if (i) fprintf(o, "else ");
                if (arm && arm->as.match_arm.is_wild) {
                    fprintf(o, "{\n");
                } else {
                    fprintf(o, "if (");
                    Type *st = n->as.match_stmt.scrut->ty;
                    for (size_t p = 0; p < (arm ? arm->as.match_arm.pat_count : 0); p++) {
                        if (p) fprintf(o, " || ");
                        if (st && st->kind == TY_STRING) {
                            fprintf(o, "yuga_fmt_eq(_m, ");
                            emit_expr(o, arm->as.match_arm.pats[p]);
                            fprintf(o, ")");
                        } else {
                            fprintf(o, "_m == ");
                            emit_expr(o, arm->as.match_arm.pats[p]);
                        }
                    }
                    fprintf(o, ") {\n");
                }
                drop_sp++;
                drop_n[drop_sp] = 0;
                if (arm && arm->as.match_arm.body && arm->as.match_arm.body->kind == AST_BLOCK)
                    emit_block_body(o, arm->as.match_arm.body, ind + 2, 0);
                emit_drops_scope(o, ind + 2, drop_sp);
                drop_sp--;
                indent(o, ind + 1);
                fprintf(o, "}\n");
            }
            indent(o, ind);
            fprintf(o, "}\n");
            break;
        }
        case AST_BLOCK:
            indent(o, ind);
            fprintf(o, "{\n");
            drop_sp++;
            drop_n[drop_sp] = 0;
            emit_block_body(o, n, ind + 1, 0);
            emit_drops_scope(o, ind + 1, drop_sp);
            drop_sp--;
            indent(o, ind);
            fprintf(o, "}\n");
            break;
        default:
            break;
    }
}

static void emit_block_body(FILE *o, AstNode *b, int ind, int is_main) {
    if (!b || b->kind != AST_BLOCK) return;
    for (size_t i = 0; i < b->as.block.stmt_count; i++)
        emit_stmt(o, b->as.block.stmts[i], ind);
    (void)is_main;
}

static int struct_targs_concrete(Type *t) {
    if (!t) return 0;
    for (size_t i = 0; i < t->param_count; i++)
        if (!t->params[i] || t->params[i]->kind == TY_PARAM) return 0;
    return 1;
}

static void emit_struct_type(FILE *o, Type *t) {
    char cn[256];
    type_c_name(t, cn, sizeof cn);
    fprintf(o, "typedef struct %s {\n", cn);
    for (size_t i = 0; i < t->field_count; i++) {
        fprintf(o, "    ");
        emit_var_decl_type(o, t->field_types[i], t->field_names[i]);
        fprintf(o, ";\n");
    }
    fprintf(o, "} %s;\n\n", cn);
}

static void emit_struct(FILE *o, AstNode *st) {
    if (st->as.strct.name && strcmp(st->as.strct.name, "Future") == 0) {
        fprintf(o, "typedef struct Future {\n    int64_t id;\n} Future;\n\n");
        return;
    }
    if (st->as.strct.name && strcmp(st->as.strct.name, "Chan") == 0) {
        fprintf(o, "typedef struct Chan {\n    int64_t id;\n} Chan;\n\n");
        return;
    }
    if (st->as.strct.tparam_count) return;
    if (st->ty)
        emit_struct_type(o, st->ty);
}

static void emit_fn_sig(FILE *o, AstNode *fn, int is_main) {
    if (is_main) {
        /* iOS / Android: the host owns process main; Yuga entry is yuga_app_main. */
        fprintf(o, "#if defined(YUGA_IOS) || defined(YUGA_ANDROID)\n"
                   "int yuga_app_main(void)\n#else\nint main(void)\n#endif\n");
        return;
    }
    Type *ft = fn->ty;
    Type *ret = (ft && ft->ret) ? ft->ret : ty_void();
    emit_ctype(o, ret);
    fprintf(o, " %s(", cname_override ? cname_override
                                      : (fn->as.fn.cname ? fn->as.fn.cname : fn->as.fn.name));
    if (fn->as.fn.param_count == 0) fprintf(o, "void");
    for (size_t i = 0; i < fn->as.fn.param_count; i++) {
        if (i) fprintf(o, ", ");
        Type *pt = (ft && i < ft->param_count) ? ft->params[i] : ty_int();
        emit_var_decl_type(o, pt, fn->as.fn.params[i].name);
    }
    fprintf(o, ")");
}

static const char *lv(int id) {
    static char bufs[8][64];
    static int n;
    if (!CF || id < 0 || id >= CF->nlocals) return "/*err*/";
    const IrLocal *l = &CF->locals[id];
    if (l->is_global && l->cname) return l->cname;
    if (l->is_param && l->name) return l->name;
    char *b = bufs[n++ & 7];
    snprintf(b, sizeof bufs[0], "_l%d", id);
    return b;
}

static void emit_ir_loc(FILE *o, SourceLoc loc) {
    fprintf(o, "\"");
    const char *f = loc.file ? loc.file : "yuga";
    for (const char *p = f; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', o);
        fputc(*p, o);
    }
    fprintf(o, "\", %d", loc.line);
}

static int is_ptrish_ty(Type *t) { return t && (t->kind == TY_PTR || t->kind == TY_BOX); }

static void emit_ir_place(FILE *o, const IrPlace *p) {
    if (!p) {
        fprintf(o, "/*place*/");
        return;
    }
    switch (p->kind) {
        case IR_PL_LOCAL:
            fprintf(o, "%s", lv(p->local));
            break;
        case IR_PL_FIELD:
            if (CF && CF->clos_id && CF->env_local >= 0 && p->base &&
                p->base->kind == IR_PL_LOCAL && p->base->local == CF->env_local) {
                fprintf(o, "((struct yuga_env_%d *)%s)->%s", CF->clos_id, lv(CF->env_local),
                        p->field ? p->field : "?");
            } else if (is_ptrish_ty(p->base ? p->base->ty : NULL)) {
                fprintf(o, "(");
                emit_ir_place(o, p->base);
                fprintf(o, ")->%s", p->field ? p->field : "?");
            } else {
                emit_ir_place(o, p->base);
                fprintf(o, ".%s", p->field ? p->field : "?");
            }
            break;
        case IR_PL_INDEX: {
            Type *bt = p->base ? p->base->ty : NULL;
            Type *vt = bt;
            if (vt && (vt->kind == TY_PTR || vt->kind == TY_BOX)) vt = vt->elem;
            if (vt && vt->kind == TY_VEC) {
                fprintf(o, "((");
                emit_ctype(o, vt->elem);
                fprintf(o, " *)(");
                emit_ir_place(o, p->base);
                if (is_ptrish_ty(bt))
                    fprintf(o, ")->ptr)[%s]", lv(p->index));
                else
                    fprintf(o, ".ptr))[%s]", lv(p->index));
            } else if (vt && vt->kind == TY_STRING) {
                fprintf(o, "((int64_t)(unsigned char)(");
                emit_ir_place(o, p->base);
                if (is_ptrish_ty(bt))
                    fprintf(o, ")->ptr[%s]))", lv(p->index));
                else
                    fprintf(o, ".ptr[%s]))", lv(p->index));
            } else {
                emit_ir_place(o, p->base);
                fprintf(o, "[%s]", lv(p->index));
            }
            break;
        }
        case IR_PL_DEREF:
            fprintf(o, "(*");
            emit_ir_place(o, p->base);
            fprintf(o, ")");
            break;
    }
}

static const char *c_binop(int op) {
    switch ((TokenKind)op) {
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_PERCENT: return "%";
        case TOK_EQ_EQ: return "==";
        case TOK_BANG_EQ: return "!=";
        case TOK_LT: return "<";
        case TOK_GT: return ">";
        case TOK_LT_EQ: return "<=";
        case TOK_GT_EQ: return ">=";
        case TOK_AMP_AMP: return "&&";
        case TOK_PIPE_PIPE: return "||";
        case TOK_AMP: return "&";
        case TOK_PIPE: return "|";
        case TOK_CARET: return "^";
        case TOK_SHL: return "<<";
        case TOK_SHR: return ">>";
        default: return "?";
    }
}

/** `op=` text for the non-arithmetic compound assignments. */
static const char *c_assignop(int op) {
    switch ((TokenKind)op) {
        case TOK_AMP_EQ: return "&=";
        case TOK_PIPE_EQ: return "|=";
        case TOK_CARET_EQ: return "^=";
        case TOK_SHL_EQ: return "<<=";
        case TOK_SHR_EQ: return ">>=";
        default: return NULL;
    }
}

static void emit_steal(FILE *o, const char *place, Type *t, int ind) {
    if (!t || !place) return;
    if (t->kind == TY_BOX) {
        indent(o, ind);
        fprintf(o, "%s = NULL;\n", place);
        return;
    }
    if (t->kind == TY_PROC) {
        indent(o, ind);
        fprintf(o, "%s.fn = NULL; %s.env = NULL;\n", place, place);
        return;
    }
    if (t->kind == TY_VEC) {
        indent(o, ind);
        fprintf(o, "%s.ptr = NULL; %s.len = 0; %s.cap = 0;\n", place, place, place);
        return;
    }
    if (t->kind == TY_ARRAY && t->elem && type_needs_drop(t->elem)) {
        int64_t n = t->array_len;
        for (int64_t i = 0; i < n; i++) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s[%lld]", place, (long long)i);
            emit_steal(o, buf, t->elem, ind);
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++) {
            if (!type_needs_drop(t->field_types[i])) continue;
            char buf[256];
            snprintf(buf, sizeof buf, "%s.%s", place, t->field_names[i]);
            emit_steal(o, buf, t->field_types[i], ind);
        }
    }
}

static void emit_drop_place(FILE *o, const char *place, Type *t, int ind) {
    if (!type_needs_drop(t) || !place) return;
    if (t->kind == TY_BOX) {
        indent(o, ind);
        fprintf(o, "yuga_drop((void **)&%s);\n", place);
        return;
    }
    if (t->kind == TY_PROC) {
        indent(o, ind);
        fprintf(o, "yuga_fn_drop(&%s);\n", place);
        return;
    }
    if (t->kind == TY_VEC) {
        if (t->elem && type_needs_drop(t->elem)) {
            char cn[256], ebuf[256];
            format_ctype(cn, sizeof cn, t->elem);
            indent(o, ind);
            fprintf(o, "{ int64_t _di; for (_di = 0; _di < %s.len; _di++) {\n", place);
            snprintf(ebuf, sizeof ebuf, "((%s *)%s.ptr)[_di]", cn, place);
            emit_drop_place(o, ebuf, t->elem, ind + 1);
            indent(o, ind);
            fprintf(o, "} }\n");
        }
        indent(o, ind);
        fprintf(o, "yuga_vec_drop(&%s);\n", place);
        return;
    }
    if (t->kind == TY_ARRAY) {
        if (t->elem && type_needs_drop(t->elem)) {
            indent(o, ind);
            fprintf(o, "{ int64_t _di; for (_di = 0; _di < %lld; _di++) {\n",
                    (long long)t->array_len);
            char ebuf[256];
            snprintf(ebuf, sizeof ebuf, "%s[_di]", place);
            emit_drop_place(o, ebuf, t->elem, ind + 1);
            indent(o, ind);
            fprintf(o, "} }\n");
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++) {
            if (!type_needs_drop(t->field_types[i])) continue;
            char buf[256];
            snprintf(buf, sizeof buf, "%s.%s", place, t->field_names[i]);
            emit_drop_place(o, buf, t->field_types[i], ind);
        }
    }
}

/* A callee drops its params at exit, yet call arguments share storage with
 * the caller (assignment/capture retain; calls do not). Without a bump here,
 * the first call with a vec argument frees the caller's buffer out from under
 * it, and loops over the same data crash. Take one rc per nested vec the
 * callee will drop; the callee's param drops then release exactly these. */
static void emit_nested_keeps(FILE *o, const char *place, Type *t, int ind) {
    if (!t || !type_needs_drop(t)) return;
    if (t->kind == TY_VEC) {
        indent(o, ind);
        fprintf(o, "yuga_vec_retain(&%s);\n", place);
        if (t->elem && type_needs_drop(t->elem)) {
            char cn[256], ebuf[256];
            format_ctype(cn, sizeof cn, t->elem);
            indent(o, ind);
            fprintf(o, "{ int64_t _di; for (_di = 0; _di < %s.len; _di++) {\n", place);
            snprintf(ebuf, sizeof ebuf, "((%s *)%s.ptr)[_di]", cn, place);
            emit_nested_keeps(o, ebuf, t->elem, ind + 1);
            indent(o, ind);
            fprintf(o, "} }\n");
        }
        return;
    }
    if (t->kind == TY_ARRAY) {
        if (t->elem && type_needs_drop(t->elem)) {
            indent(o, ind);
            fprintf(o, "{ int64_t _ai; for (_ai = 0; _ai < %lld; _ai++) {\n",
                    (long long)t->array_len);
            char ebuf[256];
            snprintf(ebuf, sizeof ebuf, "%s[_ai]", place);
            emit_nested_keeps(o, ebuf, t->elem, ind + 1);
            indent(o, ind);
            fprintf(o, "} }\n");
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++) {
            if (!type_needs_drop(t->field_types[i])) continue;
            char buf[256];
            snprintf(buf, sizeof buf, "%s.%s", place, t->field_names[i]);
            emit_nested_keeps(o, buf, t->field_types[i], ind);
        }
    }
}

/* The counterpart to `emit_nested_keeps` for the handles a call *adopts*. A
   `Box`/`fn` leaf is passed to the callee as-is, so the callee's parameter
   drop frees it; the caller keeps its own copy of the pointer only because the
   argument's type also has a `[]T` leaf that had to be retained (see
   `type_arg_transfers`). Clear those adopted pointers so the caller's own drop
   does not release them a second time. */
static void emit_nested_transfers(FILE *o, const char *place, Type *t, int ind) {
    if (!t || !type_needs_drop(t)) return;
    if (t->kind == TY_BOX) {
        indent(o, ind);
        fprintf(o, "%s = NULL;\n", place);
        return;
    }
    if (t->kind == TY_PROC) {
        indent(o, ind);
        fprintf(o, "%s.fn = NULL; %s.env = NULL;\n", place, place);
        return;
    }
    /* A retained []T leaf still belongs to the caller, so it stays. */
    if (t->kind == TY_VEC) return;
    if (t->kind == TY_ARRAY) {
        if (t->elem && type_needs_drop(t->elem)) {
            char ebuf[256];
            indent(o, ind);
            fprintf(o, "{ int64_t _ti; for (_ti = 0; _ti < %lld; _ti++) {\n",
                    (long long)t->array_len);
            snprintf(ebuf, sizeof ebuf, "%s[_ti]", place);
            emit_nested_transfers(o, ebuf, t->elem, ind + 1);
            indent(o, ind);
            fprintf(o, "} }\n");
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++) {
            if (!type_needs_drop(t->field_types[i])) continue;
            char buf[256];
            snprintf(buf, sizeof buf, "%s.%s", place, t->field_names[i]);
            emit_nested_transfers(o, buf, t->field_types[i], ind);
        }
    }
}

static void emit_array_copy(FILE *o, int dst, int src, Type *t) {
    int64_t n = t && t->kind == TY_ARRAY ? t->array_len : 0;
    char dbuf[64], sbuf[64];
    snprintf(dbuf, sizeof dbuf, "%s", lv(dst));
    snprintf(sbuf, sizeof sbuf, "%s", lv(src));
    fprintf(o, "    { int64_t _ci; for (_ci = 0; _ci < %lld; _ci++) %s[_ci] = %s[_ci]; }\n",
            (long long)n, dbuf, sbuf);
}

static void emit_ir_inst(FILE *o, const IrInst *in) {
    indent(o, 1);
    switch (in->op) {
        case IR_CONST_INT:
            fprintf(o, "%s = %lld;\n", lv(in->dst), (long long)in->imm);
            break;
        case IR_CONST_FLOAT:
            fprintf(o, "%s = ", lv(in->dst));
            emit_float_lit(o, in->fimm, in->ty && in->ty->kind == TY_FLOAT && in->ty->bits == 32);
            fprintf(o, ";\n");
            break;
        case IR_CONST_BOOL:
            fprintf(o, "%s = %s;\n", lv(in->dst), in->imm ? "true" : "false");
            break;
        case IR_CONST_STR:
            fprintf(o, "%s = ", lv(in->dst));
            emit_str_lit(o, in->str);
            fprintf(o, ";\n");
            break;
        case IR_LOAD:
            fprintf(o, "%s = ", lv(in->dst));
            emit_ir_place(o, in->place);
            fprintf(o, ";\n");
            /* A read of an owning value is an owned copy: the place keeps its
               own reference, so the temporary takes one and releases it at the
               end of its scope. Every consumer then balances — a `let` or a
               call argument retains again and a struct field, a `return`, or a
               `push` steals this reference. */
            if (in->ty && type_needs_drop(in->ty))
                emit_nested_keeps(o, lv(in->dst), in->ty, 1);
            break;
        case IR_STORE: {
            int is_shift = in->binop == TOK_SHL_EQ || in->binop == TOK_SHR_EQ;
            const char *cf = NULL;
            if (in->ty && type_is_int_kind(in->ty)) {
                cf = is_shift ? shift_fn(in->ty, in->binop == TOK_SHR_EQ ? TOK_SHR : TOK_SHL)
                              : arith_fn(in->ty, in->binop);
            }
            if (cf && (in->binop == TOK_PLUS_EQ || in->binop == TOK_MINUS_EQ ||
                       in->binop == TOK_STAR_EQ || in->binop == TOK_SLASH_EQ ||
                       in->binop == TOK_PERCENT_EQ || is_shift)) {
                emit_ir_place(o, in->place);
                fprintf(o, " = %s(", cf);
                emit_ir_place(o, in->place);
                fprintf(o, ", %s, ", lv(in->a));
                emit_ir_loc(o, in->loc);
                fprintf(o, ");\n");
            } else if (in->binop == TOK_PLUS_EQ || in->binop == TOK_MINUS_EQ ||
                       in->binop == TOK_STAR_EQ || in->binop == TOK_SLASH_EQ ||
                       in->binop == TOK_PERCENT_EQ) {
                const char *op = "+=";
                if (in->binop == TOK_MINUS_EQ) op = "-=";
                else if (in->binop == TOK_STAR_EQ) op = "*=";
                else if (in->binop == TOK_SLASH_EQ) op = "/=";
                else if (in->binop == TOK_PERCENT_EQ) op = "%=";
                emit_ir_place(o, in->place);
                fprintf(o, " %s %s;\n", op, lv(in->a));
            } else {
                const char *aop = NULL;
                if ((aop = c_assignop(in->binop)) != NULL) {
                    emit_ir_place(o, in->place);
                    fprintf(o, " %s %s;\n", aop, lv(in->a));
                } else if (in->ty && in->ty->kind == TY_VEC && type_is_copy(in->ty)) {
                    fprintf(o, "{\n        yuga_vec _repl = ");
                    emit_ir_place(o, in->place);
                    fprintf(o, ";\n        ");
                    emit_ir_place(o, in->place);
                    fprintf(o, " = yuga_vec_retain(&%s);\n", lv(in->a));
                    emit_drop_place(o, "_repl", in->ty, 1);
                    fprintf(o, "    }\n");
                } else if (in->ty && type_needs_drop(in->ty) && in->ty->kind != TY_ARRAY) {
                    char cn[256];
                    format_ctype(cn, sizeof cn, in->ty);
                    fprintf(o, "{\n        %s _repl = ", cn);
                    emit_ir_place(o, in->place);
                    fprintf(o, ";\n        ");
                    emit_ir_place(o, in->place);
                    fprintf(o, " = %s;\n", lv(in->a));
                    emit_steal(o, lv(in->a), in->ty, 1);
                    emit_drop_place(o, "_repl", in->ty, 1);
                    fprintf(o, "    }\n");
                } else {
                    emit_ir_place(o, in->place);
                    fprintf(o, " = %s;\n", lv(in->a));
                }
            }
            break;
        }
        case IR_ADDR:
            fprintf(o, "%s = &", lv(in->dst));
            emit_ir_place(o, in->place);
            fprintf(o, ";\n");
            break;
        case IR_MOVE: {
            Type *ty = in->ty ? in->ty : (in->dst >= 0 ? CF->locals[in->dst].ty : NULL);
            if (ty && ty->kind == TY_BOX) {
                fprintf(o, "%s = (", lv(in->dst));
                emit_ctype(o, ty);
                fprintf(o, ")yuga_move_ptr((void **)&%s);\n", lv(in->a));
            } else if (ty && ty->kind == TY_PROC) {
                if (type_is_copy(ty))
                    fprintf(o, "%s = %s;\n", lv(in->dst), lv(in->a));
                else
                    fprintf(o, "%s = yuga_fn_move(&%s);\n", lv(in->dst), lv(in->a));
            } else if (ty && ty->kind == TY_VEC) {
                if (type_is_copy(ty))
                    fprintf(o, "%s = yuga_vec_retain(&%s);\n", lv(in->dst), lv(in->a));
                else
                    fprintf(o, "%s = yuga_vec_move(&%s);\n", lv(in->dst), lv(in->a));
            } else if (ty && ty->kind == TY_ARRAY) {
                fprintf(o, "\n");
                emit_array_copy(o, in->dst, in->a, ty);
            } else if (ty && type_needs_drop(ty) && ty->kind == TY_STRUCT) {
                char dbuf[64], sbuf[64];
                snprintf(dbuf, sizeof dbuf, "%s", lv(in->dst));
                snprintf(sbuf, sizeof sbuf, "%s", lv(in->a));
                fprintf(o, "%s = %s;\n", dbuf, sbuf);
                emit_steal(o, sbuf, ty, 1);
            } else {
                fprintf(o, "%s = %s;\n", lv(in->dst), lv(in->a));
            }
            break;
        }
        case IR_BIN: {
            Type *at = (in->a >= 0 && in->a < CF->nlocals) ? CF->locals[in->a].ty : NULL;
            const char *cf = NULL;
            if ((in->binop == TOK_SHL || in->binop == TOK_SHR) && in->ty &&
                type_is_int_kind(in->ty))
                cf = shift_fn(in->ty, in->binop);
            else if (in->checked)
                cf = arith_fn(in->ty, in->binop);
            if ((in->binop == TOK_EQ_EQ || in->binop == TOK_BANG_EQ) && at &&
                at->kind == TY_STRING) {
                fprintf(o, "%s = %syuga_fmt_eq(%s, %s);\n", lv(in->dst),
                        in->binop == TOK_BANG_EQ ? "!" : "", lv(in->a), lv(in->b));
            } else if (cf) {
                fprintf(o, "%s = %s(%s, %s, ", lv(in->dst), cf, lv(in->a), lv(in->b));
                emit_ir_loc(o, in->loc);
                fprintf(o, ");\n");
            } else {
                fprintf(o, "%s = (%s %s %s);\n", lv(in->dst), lv(in->a), c_binop(in->binop),
                        lv(in->b));
            }
            break;
        }
        case IR_UN: {
            Type *at = (in->a >= 0 && in->a < CF->nlocals) ? CF->locals[in->a].ty : NULL;
            if (in->binop == TOK_TILDE && at && type_is_int_kind(at)) {
                fprintf(o, "%s = (", lv(in->dst));
                emit_ctype(o, in->ty);
                fprintf(o, ")(~%s);\n", lv(in->a));
            } else if (in->binop != TOK_BANG && in->binop != TOK_TILDE && at &&
                       type_is_int_kind(at)) {
                fprintf(o, "%s = %s(%s, ", lv(in->dst), neg_fn(in->ty), lv(in->a));
                emit_ir_loc(o, in->loc);
                fprintf(o, ");\n");
            } else {
                fprintf(o, "%s = (%s%s);\n", lv(in->dst),
                        in->binop == TOK_BANG ? "!" : (in->binop == TOK_TILDE ? "~" : "-"),
                        lv(in->a));
            }
            break;
        }
        case IR_CAST: {
            Type *from = (in->a >= 0 && in->a < CF->nlocals) ? CF->locals[in->a].ty : NULL;
            char fn[32];
            CastShape cs = cast_shape(from, in->ty, in->conv_mode, fn, sizeof fn);
            if (cs == CAST_PLAIN) {
                fprintf(o, "%s = (", lv(in->dst));
                emit_ctype(o, in->ty);
                fprintf(o, ")%s;\n", lv(in->a));
            } else {
                fprintf(o, "%s = %s(", lv(in->dst), fn);
                if (cs == CAST_CONV || cs == CAST_SAT)
                    fprintf(o, "(int64_t)");
                else if (cs == CAST_CONVU || cs == CAST_SATU)
                    fprintf(o, "(uint64_t)");
                else
                    fprintf(o, "(double)");
                fprintf(o, "%s", lv(in->a));
                if (cs == CAST_CONV || cs == CAST_CONVU || cs == CAST_CONVF) {
                    fprintf(o, ", ");
                    emit_ir_loc(o, in->loc);
                }
                fprintf(o, ");\n");
            }
            break;
        }
        case IR_SIZEOF: {
            Type *at = in->ty;
            size_t nel = (at && at->kind == TY_ARRAY && at->array_len > 0)
                             ? (size_t)at->array_len
                             : 1;
            fprintf(o, "%s = (int32_t)(sizeof(", lv(in->dst));
            emit_ctype(o, at && at->kind == TY_ARRAY ? at->elem : at);
            fprintf(o, ") * %zu);\n", nel);
            break;
        }
        case IR_PANIC:
            fprintf(o, "yuga_panic_str(");
            emit_ir_loc(o, in->loc);
            fprintf(o, ", %s);\n", lv(in->a));
            break;
        case IR_CALL:
        case IR_CALL_VAL: {
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_sig_push") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                Type *vt = in->ty;
                fprintf(o, "yuga_arena_ensure();\n");
                indent(o, 1);
                if (vt && vt->kind == TY_INT && vt->bits == 32) {
                    /* Mirror slot + cell, reusing freed ids when possible. */
                    fprintf(o, "%s = yuga_zeus_sig_alloc_int(%s);\n", lv(in->dst),
                            lv(in->args[0]));
                } else {
                    fprintf(o, "%s = yuga_zeus_sig_alloc_zero();\n", lv(in->dst));
                }
                indent(o, 1);
                if (type_is_copy_vec(vt)) {
                    fprintf(o, "{ yuga_vec _sigv = yuga_vec_retain(&%s); yuga_zeus_sig_bind(%s, &_sigv, sizeof(_sigv)); }\n",
                            lv(in->args[0]), lv(in->dst));
                } else {
                    fprintf(o, "yuga_zeus_sig_bind(%s, &%s, sizeof(", lv(in->dst), lv(in->args[0]));
                    emit_ctype(o, vt);
                    fprintf(o, "));\n");
                }
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_sig_load") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                fprintf(o, "yuga_zeus_sig_load(%s, &%s, sizeof(", lv(in->args[0]), lv(in->dst));
                emit_ctype(o, in->ty);
                fprintf(o, "));\n");
                if (type_is_copy_vec(in->ty)) {
                    indent(o, 1);
                    fprintf(o, "%s = yuga_vec_retain(&%s);\n", lv(in->dst), lv(in->dst));
                }
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_sig_store") == 0 &&
                in->nargs >= 2) {
                Type *vt = in->ty;
                if (vt && vt->kind == TY_VOID && in->args[1] >= 0 && in->args[1] < CF->nlocals)
                    vt = CF->locals[in->args[1]].ty;
                if (vt && vt->kind == TY_INT && vt->bits == 32) {
                    fprintf(o, "yuga_arena_store_sig(%s, %s);\n", lv(in->args[0]),
                            lv(in->args[1]));
                } else if (type_is_copy_vec(vt)) {
                    fprintf(o, "if (yuga_zeus_sig_changed(%s, &%s, sizeof(%s))) {\n",
                            lv(in->args[0]), lv(in->args[1]), lv(in->args[1]));
                    indent(o, 2);
                    fprintf(o, "yuga_vec _old; yuga_zeus_sig_load(%s, &_old, sizeof(_old)); _old = yuga_vec_retain(&_old);\n",
                            lv(in->args[0]));
                    indent(o, 2);
                    fprintf(o, "yuga_vec _new = yuga_vec_retain(&%s); yuga_zeus_sig_bind(%s, &_new, sizeof(_new));\n",
                            lv(in->args[1]), lv(in->args[0]));
                    indent(o, 2);
                    fprintf(o, "yuga_vec_drop(&_old); yuga_track_notify(%s);\n", lv(in->args[0]));
                    indent(o, 1);
                    fprintf(o, "}\n");
                } else {
                    fprintf(o, "if (yuga_zeus_sig_changed(%s, &%s, sizeof(", lv(in->args[0]),
                            lv(in->args[1]));
                    emit_ctype(o, vt);
                    fprintf(o, "))) { yuga_zeus_sig_bind(%s, &%s, sizeof(", lv(in->args[0]),
                            lv(in->args[1]));
                    emit_ctype(o, vt);
                    fprintf(o, ")); yuga_track_notify(%s); }\n", lv(in->args[0]));
                }
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_fut_push") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                Type *vt = in->ty;
                if (type_is_copy_vec(vt)) {
                    fprintf(o, "{ yuga_vec _fv = yuga_vec_retain(&%s); %s = yuga_fut_push(&_fv, sizeof(_fv)); }\n",
                            lv(in->args[0]), lv(in->dst));
                } else {
                    fprintf(o, "%s = yuga_fut_push(&%s, sizeof(", lv(in->dst), lv(in->args[0]));
                    emit_ctype(o, vt);
                    fprintf(o, "));\n");
                }
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_fut_load") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                fprintf(o, "yuga_fut_load(%s, &%s, sizeof(", lv(in->args[0]), lv(in->dst));
                emit_ctype(o, in->ty);
                fprintf(o, "));\n");
                if (type_is_copy_vec(in->ty)) {
                    indent(o, 1);
                    fprintf(o, "%s = yuga_vec_retain(&%s);\n", lv(in->dst), lv(in->dst));
                }
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_fut_store") == 0 &&
                in->nargs >= 2) {
                Type *vt = in->ty;
                if (vt && vt->kind == TY_VOID && in->args[1] >= 0 && in->args[1] < CF->nlocals)
                    vt = CF->locals[in->args[1]].ty;
                if (type_is_copy_vec(vt)) {
                    fprintf(o, "{ yuga_vec _old; yuga_fut_load(%s, &_old, sizeof(_old)); "
                               "yuga_vec _new = yuga_vec_retain(&%s); yuga_fut_store(%s, &_new, sizeof(_new)); "
                               "yuga_vec_drop(&_old); }\n",
                            lv(in->args[0]), lv(in->args[1]), lv(in->args[0]));
                } else {
                    fprintf(o, "yuga_fut_store(%s, &%s, sizeof(", lv(in->args[0]), lv(in->args[1]));
                    emit_ctype(o, vt);
                    fprintf(o, "));\n");
                }
                break;
            }
            /* Channels: payloads are Send (plain data), so the value is one
               C blob of sizeof(T) — no vec retain/drop dance like futures. */
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_ch_alloc") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                fprintf(o, "%s = yuga_ch_alloc(%s, sizeof(", lv(in->dst), lv(in->args[0]));
                emit_ctype(o, in->ty);
                fprintf(o, "));\n");
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_ch_send") == 0 &&
                in->nargs >= 2) {
                fprintf(o, "yuga_ch_send(%s, &%s, sizeof(", lv(in->args[0]), lv(in->args[1]));
                emit_ctype(o, in->ty);
                fprintf(o, "));\n");
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_ch_try_send") == 0 &&
                in->nargs >= 2 && in->dst >= 0) {
                fprintf(o, "%s = yuga_ch_try_send(%s, &%s, sizeof(", lv(in->dst),
                        lv(in->args[0]), lv(in->args[1]));
                emit_ctype(o, in->ty);
                fprintf(o, "));\n");
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_ch_recv") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                fprintf(o, "yuga_ch_recv(%s, &%s, sizeof(", lv(in->args[0]), lv(in->dst));
                emit_ctype(o, in->ty);
                fprintf(o, "));\n");
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_ch_pop") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                fprintf(o, "yuga_ch_pop(%s, &%s, sizeof(", lv(in->args[0]), lv(in->dst));
                emit_ctype(o, in->ty);
                fprintf(o, "));\n");
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_ch_ready") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                fprintf(o, "%s = yuga_ch_ready(%s);\n", lv(in->dst), lv(in->args[0]));
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_vec_push") == 0 &&
                in->nargs >= 2) {
                Type *vt = (in->args[0] >= 0 && in->args[0] < CF->nlocals)
                               ? CF->locals[in->args[0]].ty
                               : NULL;
                const char *amp = (vt && vt->kind == TY_VEC) ? "&" : "";
                fprintf(o, "yuga_vec_push(%s%s, &%s, sizeof(", amp, lv(in->args[0]),
                        lv(in->args[1]));
                emit_ctype(o, in->ty);
                fprintf(o, "), ");
                emit_ir_loc(o, in->loc);
                fprintf(o, ");\n");
                break;
            }
            if (in->op == IR_CALL && in->callee && strcmp(in->callee, "yuga_vec_pop") == 0 &&
                in->nargs >= 1 && in->dst >= 0) {
                Type *vt = (in->args[0] >= 0 && in->args[0] < CF->nlocals)
                               ? CF->locals[in->args[0]].ty
                               : NULL;
                const char *amp = (vt && vt->kind == TY_VEC) ? "&" : "";
                fprintf(o, "yuga_vec_pop(%s%s, &%s, sizeof(", amp, lv(in->args[0]), lv(in->dst));
                emit_ctype(o, in->ty);
                fprintf(o, "), ");
                emit_ir_loc(o, in->loc);
                fprintf(o, ");\n");
                break;
            }
            /* Vec/struct call args share storage with the caller, but the
               callee drops its params on exit. Keep the shared storage alive
               for the call so reuses (loops, repeated calls) stay intact. */
            int nkeep = 0;
            for (int k = 0; k < in->nargs; k++) {
                Type *at = (in->args[k] >= 0 && in->args[k] < CF->nlocals)
                               ? CF->locals[in->args[k]].ty
                               : NULL;
                if (in->op == IR_CALL_VAL && at == NULL) {
                    Type *ft = (in->a >= 0 && in->a < CF->nlocals)
                                   ? CF->locals[in->a].ty
                                   : NULL;
                    if (ft && ft->kind == TY_PROC && (size_t)k < ft->param_count)
                        at = ft->params[k];
                }
                if (at && type_needs_drop(at) && at->kind != TY_BOX &&
                    at->kind != TY_PROC) {
                    nkeep++;
                }
            }
            if (nkeep > 0) {
                indent(o, 1);
                fprintf(o, "{\n");
                for (int k = 0; k < in->nargs; k++) {
                    Type *at = (in->args[k] >= 0 && in->args[k] < CF->nlocals)
                                   ? CF->locals[in->args[k]].ty
                                   : NULL;
                    if (in->op == IR_CALL_VAL && at == NULL) {
                        Type *ft = (in->a >= 0 && in->a < CF->nlocals)
                                       ? CF->locals[in->a].ty
                                       : NULL;
                        if (ft && ft->kind == TY_PROC && (size_t)k < ft->param_count)
                            at = ft->params[k];
                    }
                    if (at && type_needs_drop(at) && at->kind != TY_BOX &&
                        at->kind != TY_PROC)
                        emit_nested_keeps(o, lv(in->args[k]), at, 2);
                }
                indent(o, 1);
            }
            if (in->dst >= 0) fprintf(o, "%s = ", lv(in->dst));
            if (in->op == IR_CALL) {
                fprintf(o, "%s(", in->callee ? in->callee : "unknown");
                for (int k = 0; k < in->nargs; k++)
                    fprintf(o, "%s%s", k ? ", " : "", lv(in->args[k]));
                fprintf(o, ");\n");
            } else {
                Type *ft = (in->a >= 0 && in->a < CF->nlocals) ? CF->locals[in->a].ty : NULL;
                Type *ret = (ft && ft->kind == TY_PROC && ft->ret) ? ft->ret : ty_void();
                char fv[64];
                snprintf(fv, sizeof fv, "%s", lv(in->a));
                fprintf(o, "((");
                emit_ctype(o, ret);
                fprintf(o, "(*)(void *");
                if (ft && ft->kind == TY_PROC) {
                    for (size_t k = 0; k < ft->param_count; k++) {
                        fprintf(o, ", ");
                        emit_ctype(o, ft->params[k]);
                    }
                }
                fprintf(o, "))%s.fn)(%s.env", fv, fv);
                for (int k = 0; k < in->nargs; k++) fprintf(o, ", %s", lv(in->args[k]));
                fprintf(o, ");\n");
            }
            if (nkeep > 0) {
                /* After the call: the callee owns those handles now. Before it,
                   the argument expressions would see NULL. */
                for (int k = 0; k < in->nargs; k++) {
                    Type *at = (in->args[k] >= 0 && in->args[k] < CF->nlocals)
                                   ? CF->locals[in->args[k]].ty
                                   : NULL;
                    if (in->op == IR_CALL_VAL && at == NULL) {
                        Type *ft = (in->a >= 0 && in->a < CF->nlocals)
                                       ? CF->locals[in->a].ty
                                       : NULL;
                        if (ft && ft->kind == TY_PROC && (size_t)k < ft->param_count)
                            at = ft->params[k];
                    }
                    if (!at || !type_needs_drop(at)) continue;
                    if (!type_arg_transfers(at))
                        emit_nested_transfers(o, lv(in->args[k]), at, 2);
                }
                fprintf(o, "    }\n");
            }
            break;
        }
        case IR_ALLOC: {
            Type *bt = in->ty;
            Type *et = (bt && bt->kind == TY_BOX) ? bt->elem : ty_int();
            fprintf(o, "%s = (", lv(in->dst));
            emit_ctype(o, et);
            fprintf(o, " *)yuga_new(sizeof(");
            emit_ctype(o, et);
            fprintf(o, "), ");
            emit_ir_loc(o, in->loc);
            fprintf(o, ");\n");
            if (in->a >= 0) {
                indent(o, 1);
                fprintf(o, "*%s = %s;\n", lv(in->dst), lv(in->a));
            }
            break;
        }
        case IR_DROP: {
            Type *ty = (in->a >= 0 && in->a < CF->nlocals) ? CF->locals[in->a].ty : NULL;
            char buf[64];
            snprintf(buf, sizeof buf, "%s", lv(in->a));
            if (ty && type_needs_drop(ty))
                emit_drop_place(o, buf, ty, 0);
            else
                fprintf(o, "yuga_drop((void **)&%s);\n", buf);
            break;
        }
        case IR_BOUND:
            if (in->b >= 0)
                fprintf(o, "(void)yuga_idx(%s, %s, ", lv(in->a), lv(in->b));
            else
                fprintf(o, "(void)yuga_idx(%s, %lld, ", lv(in->a), (long long)in->imm);
            emit_ir_loc(o, in->loc);
            fprintf(o, ");\n");
            break;
        case IR_STRUCT_LIT: {
            Type *t = in->ty;
            char cn[256];
            if (t && t->kind == TY_STRUCT)
                type_c_name(t, cn, sizeof cn);
            else
                snprintf(cn, sizeof cn, "%s", t && t->name ? t->name : "struct");
            fprintf(o, "%s = (%s){ ", lv(in->dst), cn);
            for (int k = 0; k < in->nargs; k++) {
                if (k) fprintf(o, ", ");
                if (t && k < (int)t->field_count && t->field_names[k])
                    fprintf(o, ".%s = %s", t->field_names[k], lv(in->args[k]));
                else
                    fprintf(o, "%s", lv(in->args[k]));
            }
            fprintf(o, " };\n");
            break;
        }
        case IR_ARRAY_LIT: {
            char dbuf[64];
            snprintf(dbuf, sizeof dbuf, "%s", lv(in->dst));
            if (in->ty && in->ty->kind == TY_VEC) {
                fprintf(o, "%s = yuga_vec_new();\n", dbuf);
                for (int k = 0; k < in->nargs; k++) {
                    indent(o, 1);
                    fprintf(o, "yuga_vec_push(&%s, &%s, sizeof(", dbuf, lv(in->args[k]));
                    emit_ctype(o, in->ty->elem);
                    fprintf(o, "), ");
                    emit_ir_loc(o, in->loc);
                    fprintf(o, ");\n");
                    if (in->args[k] >= 0 && in->args[k] < CF->nlocals &&
                        type_needs_drop(CF->locals[in->args[k]].ty))
                        emit_steal(o, lv(in->args[k]), CF->locals[in->args[k]].ty, 1);
                }
            } else {
                for (int k = 0; k < in->nargs; k++) {
                    if (k) indent(o, 1);
                    fprintf(o, "%s[%d] = %s;\n", dbuf, k, lv(in->args[k]));
                }
                if (in->nargs == 0) fprintf(o, "(void)%s;\n", dbuf);
            }
            break;
        }
        case IR_FN_VAL:
            fprintf(o, "%s = ((yuga_fn){(void *)%s__as_fn, NULL, 0});\n", lv(in->dst),
                    in->callee ? in->callee : "unknown");
            break;
        case IR_CLOS: {
            char dbuf[64];
            snprintf(dbuf, sizeof dbuf, "%s", lv(in->dst));
            if (in->nargs == 0) {
                fprintf(o, "%s = ((yuga_fn){(void *)%s, NULL, 0});\n", dbuf,
                        in->callee ? in->callee : "unknown");
            } else {
                int id = (int)in->imm;
                IrFn *cf = find_ir_fn(in->callee);
                fprintf(o, "{\n");
                indent(o, 2);
                fprintf(o, "struct yuga_env_%d *_ce = (struct yuga_env_%d *)yuga_new(sizeof(struct yuga_env_%d), ",
                        id, id, id);
                emit_ir_loc(o, in->loc);
                fprintf(o, ");\n");
                for (int k = 0; k < in->nargs; k++) {
                    const char *fnm = (cf && k < cf->ncaps && cf->caps[k]) ? cf->caps[k] : "f";
                    Type *ct = (cf && k < cf->ncaps && cf->cap_types) ? cf->cap_types[k] : NULL;
                    indent(o, 2);
                    if (type_is_copy_vec(ct)) {
                        fprintf(o, "_ce->%s = yuga_vec_retain(&%s);\n", fnm, lv(in->args[k]));
                    } else {
                        fprintf(o, "_ce->%s = %s;\n", fnm, lv(in->args[k]));
                        /* The env outlives the frame that captured it, so it
                           needs its own reference to every []T the capture
                           reaches — including the ones inside a struct, which
                           a plain struct copy leaves shared with the captured
                           local's own drop. */
                        if (ct && type_needs_drop(ct) && !type_arg_transfers(ct)) {
                            char ep[256];
                            snprintf(ep, sizeof ep, "_ce->%s", fnm);
                            emit_nested_keeps(o, ep, ct, 2);
                        }
                    }
                }
                indent(o, 2);
                fprintf(o, "%s = ((yuga_fn){(void *)%s, _ce, sizeof(struct yuga_env_%d)});\n",
                        dbuf, in->callee ? in->callee : "unknown", id);
                indent(o, 1);
                fprintf(o, "}\n");
            }
            break;
        }
    }
}

static void emit_zero_ret(FILE *o, Type *t) {
    if (!t || t->kind == TY_VOID) {
        fprintf(o, "return;\n");
        return;
    }
    fprintf(o, "return ");
    switch (t->kind) {
        case TY_BOOL:
            fprintf(o, "false");
            break;
        case TY_FLOAT:
            fprintf(o, "0.0");
            break;
        case TY_STRING:
            fprintf(o, "((yuga_str){0})");
            break;
        case TY_PTR:
        case TY_BOX:
            fprintf(o, "NULL");
            break;
        case TY_STRUCT:
            fprintf(o, "(%s){0}", t->name ? t->name : "struct");
            break;
        case TY_PROC:
            fprintf(o, "((yuga_fn){0})");
            break;
        default:
            fprintf(o, "0");
            break;
    }
    fprintf(o, ";\n");
}

static void emit_ir_term(FILE *o, const IrBlock *bb, int is_main) {
    indent(o, 1);
    switch (bb->term) {
        case IR_TERM_RET:
            if (is_main && bb->term_val < 0)
                fprintf(o, "return 0;\n");
            else if (bb->term_val >= 0)
                fprintf(o, "return %s;\n", lv(bb->term_val));
            else {
                Type *ret = (CF && CF->sig && CF->sig->kind == TY_PROC && CF->sig->ret)
                                ? CF->sig->ret
                                : ty_void();
                emit_zero_ret(o, is_main ? ty_int() : ret);
            }
            break;
        case IR_TERM_JMP:
            fprintf(o, "goto bb%d;\n", bb->succ[0]);
            break;
        case IR_TERM_BR:
            fprintf(o, "if (%s) goto bb%d; else goto bb%d;\n", lv(bb->term_val), bb->succ[0],
                    bb->succ[1]);
            break;
        case IR_TERM_UNREACHABLE:
            fprintf(o, "yuga_panic(\"yuga\", 0, \"unreachable\");\n");
            break;
    }
}

static IrFn *find_ir_fn(const char *cname) {
    if (!emit_ir_mod || !cname) return NULL;
    for (int i = 0; i < emit_ir_mod->nfns; i++) {
        IrFn *f = &emit_ir_mod->fns[i];
        if (f->cname && strcmp(f->cname, cname) == 0) return f;
    }
    return NULL;
}

static void emit_ir_fn_body(FILE *o, const IrFn *fn, int is_main) {
    CF = fn;
    fprintf(o, " {\n");
    if (fn->clos_id && fn->ncaps == 0) fprintf(o, "    (void)_env;\n");
    if (fn->is_main && emit_ir_mod) {
        /* Imports first, main module last. Modules are lowered importer-
           before-imported, so walking back puts a module's dependencies
           ahead of it: a global initialised with `zeus.signal(0)` must not
           run before the arena it allocates into, or the arena's own
           initialiser wipes the slot and every later write is dropped. */
        for (int i = emit_ir_mod->nfns - 1; i >= 0; i--) {
            IrFn *g = &emit_ir_mod->fns[i];
            if (g->name && strcmp(g->name, "__init") == 0 && g->cname)
                fprintf(o, "    %s();\n", g->cname);
        }
    }
    for (int i = 0; i < fn->nlocals; i++) {
        if (fn->locals[i].is_param || fn->locals[i].is_global) continue;
        indent(o, 1);
        emit_var_decl_type(o, fn->locals[i].ty, lv(i));
        /* SSA temps on un-taken branches are still dropped at return. */
        if (type_needs_drop(fn->locals[i].ty))
            fprintf(o, " = {0}");
        fprintf(o, ";\n");
    }
    for (int b = 0; b < fn->nblocks; b++) {
        const IrBlock *bb = &fn->blocks[b];
        fprintf(o, "bb%d:;\n", bb->id);
        for (int i = 0; i < bb->ninsts; i++) {
            emit_line_directive(o, bb->insts[i].loc);
            emit_ir_inst(o, &bb->insts[i]);
        }
        emit_ir_term(o, bb, is_main || fn->is_main);
    }
    fprintf(o, "}\n\n");
    CF = NULL;
}

static void emit_fn_sig(FILE *o, AstNode *fn, int is_main);

/** Emit a Yuga function as C. Fully-lowered functions come from IR. */
static void emit_fn(FILE *o, AstNode *fn, int is_main) {
    const char *cn = cname_override ? cname_override
                                    : (fn->as.fn.cname ? fn->as.fn.cname : fn->as.fn.name);
    IrFn *irf = find_ir_fn(cn);
    emit_line_directive(o, fn->loc);
    emit_fn_sig(o, fn, is_main);
    if (irf && irf->lowered) {
        emit_ir_fn_body(o, irf, is_main);
        return;
    }
    fprintf(o, " {\n");
    drop_sp = 1;
    drop_n[1] = 0;
    tmp_id = 0;
    clos_hoist_n = 0;
    if (fn->as.fn.body) emit_block_body(o, fn->as.fn.body, 1, is_main);
    emit_drops_scope(o, 1, 1);
    if (is_main) fprintf(o, "    return 0;\n");
    fprintf(o, "}\n\n");
}

/** Paste yuga_rt.h into the translation unit (fallback includes if missing). */
static void copy_runtime(FILE *out, const char *rt_path) {
    /* The pasted runtime defines the recoverable-trap state in this TU. */
    fputs("#define YUGA_RT_DEFINE_JMP 1\n", out);
    FILE *f = rt_path ? fopen(rt_path, "r") : NULL;
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, out);
        fclose(f);
        fputc('\n', out);
        return;
    }
    fprintf(out, "#include <stdio.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <stdbool.h>\n\n");
}

static int mods_use(YugaModule *mods, int nmods, const char *name) {
    for (int m = 0; m < nmods; m++)
        if (mods[m].name && strcmp(mods[m].name, name) == 0) return 1;
    return 0;
}

/** Walk the AST and record every AST_CLOSURE for env/fn emission. */
static void collect_clos(AstNode *n) {
    if (!n) return;
    if (n->kind == AST_CLOSURE) {
        if (nclos_nodes >= clos_cap) {
            clos_cap = clos_cap ? clos_cap * 2 : 256;
            clos_nodes = (AstNode **)realloc(clos_nodes, (size_t)clos_cap * sizeof(AstNode *));
            if (!clos_nodes) yuga_fatal("out of memory");
        }
        clos_nodes[nclos_nodes++] = n;
        collect_clos(n->as.fn.body);
        return;
    }
    switch (n->kind) {
        case AST_PROGRAM:
            for (size_t i = 0; i < n->as.program.decl_count; i++) collect_clos(n->as.program.decls[i]);
            break;
        case AST_FN_DECL:
            if (!n->as.fn.tparam_count) collect_clos(n->as.fn.body);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++) collect_clos(n->as.block.stmts[i]);
            break;
        case AST_IF:
            collect_clos(n->as.if_stmt.cond);
            collect_clos(n->as.if_stmt.then_block);
            collect_clos(n->as.if_stmt.else_block);
            break;
        case AST_FOR:
            collect_clos(n->as.for_stmt.iter);
            collect_clos(n->as.for_stmt.body);
            break;
        case AST_WHILE:
            collect_clos(n->as.if_stmt.cond);
            collect_clos(n->as.if_stmt.then_block);
            break;
        case AST_MATCH:
            collect_clos(n->as.match_stmt.scrut);
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++)
                collect_clos(n->as.match_stmt.arms[i]);
            break;
        case AST_MATCH_ARM:
            for (size_t i = 0; i < n->as.match_arm.pat_count; i++)
                collect_clos(n->as.match_arm.pats[i]);
            collect_clos(n->as.match_arm.body);
            break;
        case AST_RETURN:
            collect_clos(n->as.ret.expr);
            break;
        case AST_EXPR_STMT:
            collect_clos(n->as.expr_stmt.expr);
            break;
        case AST_VAR_DECL:
            collect_clos(n->as.var.init);
            break;
        case AST_ASSIGN:
            collect_clos(n->as.assign.left);
            collect_clos(n->as.assign.right);
            break;
        case AST_BINARY:
            collect_clos(n->as.binary.left);
            collect_clos(n->as.binary.right);
            break;
        case AST_UNARY:
            collect_clos(n->as.unary.operand);
            break;
        case AST_INCDEC:
            collect_clos(n->as.incdec.operand);
            break;
        case AST_CAST:
            collect_clos(n->as.cast.expr);
            break;
        case AST_CALL:
            collect_clos(n->as.call.callee);
            for (size_t i = 0; i < n->as.call.arg_count; i++) collect_clos(n->as.call.args[i]);
            break;
        case AST_INDEX:
        case AST_FIELD:
        case AST_DEREF:
        case AST_ADDR:
            collect_clos(n->as.access.target);
            collect_clos(n->as.access.index);
            break;
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++)
                collect_clos(n->as.struct_lit.fields[i].init);
            break;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++) collect_clos(n->as.array_lit.elems[i]);
            break;
        default:
            break;
    }
}

static void emit_clos_sig(FILE *o, AstNode *fn, int proto) {
    Type *ft = fn->ty;
    Type *ret = (ft && ft->ret) ? ft->ret : ty_void();
    emit_ctype(o, ret);
    fprintf(o, " yuga_clos_%d(void *_env", fn->as.fn.clos_id);
    for (size_t i = 0; i < fn->as.fn.param_count; i++) {
        fprintf(o, ", ");
        Type *pt = (ft && i < ft->param_count) ? ft->params[i] : ty_int();
        emit_var_decl_type(o, pt, fn->as.fn.params[i].name);
    }
    fprintf(o, ")");
    if (proto) fprintf(o, ";\n");
}

static void emit_clos_env(FILE *o, AstNode *fn) {
    if (!fn->as.fn.cap_count) return;
    fprintf(o, "struct yuga_env_%d {\n", fn->as.fn.clos_id);
    for (size_t i = 0; i < fn->as.fn.cap_count; i++) {
        fprintf(o, "    ");
        emit_var_decl_type(o, fn->as.fn.cap_types[i], fn->as.fn.caps[i]);
        fprintf(o, ";\n");
    }
    fprintf(o, "};\n");
}

/** Closures lowered from a generic instance: `yuga_clos_<id>_<mono>`. */
static int is_mono_clos(const IrFn *fn) {
    if (!fn || !fn->cname) return 0;
    if (strncmp(fn->cname, "yuga_clos_", 10) != 0) return 0;
    const char *p = fn->cname + 10;
    while (*p >= '0' && *p <= '9') p++;
    return *p == '_';
}

static int clos_id_in_nodes(int id) {
    for (int i = 0; i < nclos_nodes; i++)
        if (clos_nodes[i] && clos_nodes[i]->as.fn.clos_id == id) return 1;
    return 0;
}

static void emit_mono_clos_env(FILE *o, const IrFn *fn, int *seen, int *nseen, int nseen_cap) {
    if (!fn->ncaps) return;
    if (clos_id_in_nodes(fn->clos_id)) return;
    for (int i = 0; i < *nseen; i++)
        if (seen[i] == fn->clos_id) return;
    if (*nseen < nseen_cap) seen[(*nseen)++] = fn->clos_id;
    fprintf(o, "struct yuga_env_%d {\n", fn->clos_id);
    for (int k = 0; k < fn->ncaps; k++) {
        fprintf(o, "    ");
        emit_var_decl_type(o, fn->cap_types ? fn->cap_types[k] : ty_int(),
                           fn->caps ? fn->caps[k] : "c");
        fprintf(o, ";\n");
    }
    fprintf(o, "};\n");
}

static void emit_mono_clos_sig(FILE *o, const IrFn *fn, int proto) {
    Type *ret = (fn->sig && fn->sig->kind == TY_PROC && fn->sig->ret) ? fn->sig->ret : ty_void();
    emit_ctype(o, ret);
    fprintf(o, " %s(void *_env", fn->cname);
    for (int i = 0; i < fn->nlocals; i++) {
        if (!fn->locals[i].is_param) continue;
        if (fn->locals[i].name && strcmp(fn->locals[i].name, "_env") == 0) continue;
        fprintf(o, ", ");
        emit_var_decl_type(o, fn->locals[i].ty, fn->locals[i].name);
    }
    fprintf(o, ")");
    if (proto) fprintf(o, ";\n");
}

/** C function that implements a closure (env pointer + params). */
static void emit_closure_fn(FILE *o, AstNode *fn) {
    char cn[32];
    snprintf(cn, sizeof cn, "yuga_clos_%d", fn->as.fn.clos_id);
    IrFn *irf = find_ir_fn(cn);
    emit_clos_sig(o, fn, 0);
    if (irf && irf->lowered) {
        emit_ir_fn_body(o, irf, 0);
        return;
    }
    fprintf(o, " {\n");
    if (fn->as.fn.cap_count)
        fprintf(o, "    struct yuga_env_%d *_e = (struct yuga_env_%d *)_env;\n",
                fn->as.fn.clos_id, fn->as.fn.clos_id);
    else
        fprintf(o, "    (void)_env;\n");
    emit_clos = fn;
    drop_sp = 1;
    drop_n[1] = 0;
    tmp_id = 0;
    clos_hoist_n = 0;
    if (fn->as.fn.body) emit_block_body(o, fn->as.fn.body, 1, 0);
    emit_drops_scope(o, 1, 1);
    emit_clos = NULL;
    fprintf(o, "}\n\n");
}

/** Adapter so a named Yuga fn can be stored in a yuga_fn (env unused). */
static void emit_as_fn_tramp(FILE *o, AstNode *fn) {
    Type *ft = fn->ty;
    Type *ret = (ft && ft->ret) ? ft->ret : ty_void();
    const char *cn = fn->as.fn.cname ? fn->as.fn.cname : fn->as.fn.name;
    emit_ctype(o, ret);
    fprintf(o, " %s__as_fn(void *_env", cn);
    for (size_t i = 0; i < fn->as.fn.param_count; i++) {
        fprintf(o, ", ");
        Type *pt = (ft && i < ft->param_count) ? ft->params[i] : ty_int();
        emit_var_decl_type(o, pt, fn->as.fn.params[i].name);
    }
    fprintf(o, ") {\n    (void)_env;\n    ");
    if (ret->kind != TY_VOID) fprintf(o, "return ");
    fprintf(o, "%s(", cn);
    for (size_t i = 0; i < fn->as.fn.param_count; i++) {
        if (i) fprintf(o, ", ");
        fprintf(o, "%s", fn->as.fn.params[i].name);
    }
    fprintf(o, ");\n}\n\n");
}

static void set_mono_subst(int i) {
    AstNode *fn = typecheck_mono_fn(i);
    subst_names = fn ? fn->as.fn.tparams : NULL;
    subst_args = typecheck_mono_args(i);
    subst_n = typecheck_mono_nargs(i);
    cname_override = typecheck_mono_cname(i);
}

static void clear_subst(void) {
    subst_names = NULL;
    subst_args = NULL;
    subst_n = 0;
    cname_override = NULL;
}

/** Write a complete C translation unit for `mods`. */
void codegen_emit_c(FILE *out, YugaModule *mods, int nmods, const char *rt_path) {
    /* Per-widget opt-in: emit only decls reachable from the entry module and
       the C-seam entry points. Typecheck/IR still cover everything. Set
       YUGA_NO_DCE=1 to emit the full monolith (debug / size comparisons). */
    if (!getenv("YUGA_NO_DCE")) {
        yuga_dce_set_test_mode(test_mode);
        yuga_dce_run(mods, nmods);
    }
    IrModule *ir = ir_lower(mods, nmods);
    (void)ir_verify(ir);
    emit_ir_mod = ir;
    line_map_file = NULL;
    line_map_no = 0;

    fprintf(out, "/* Generated by yugac */\n");
    copy_runtime(out, rt_path);
    if (mods_use(mods, nmods, "zeus"))
        fprintf(out, "#include \"zeus_rt.h\"\n");
    if (mods_use(mods, nmods, "net"))
        fprintf(out, "#include \"net_rt.h\"\n");
    if (mods_use(mods, nmods, "maya"))
        fprintf(out, "#include \"maya_rt.h\"\n");
    if (mods_use(mods, nmods, "raygui"))
        fprintf(out, "#include \"raygui_rt.h\"\n");
    fprintf(out, "\n");

    /* Closures only from kept fn bodies (plus module-level inits, whose
       `__init` is always emitted). Closures of pruned fns would otherwise
       drag their whole subtree back into the C. */
    nclos_nodes = 0;
    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind == AST_FN_DECL) {
                if (d->as.fn.tparam_count || !yuga_dce_keep(d)) continue;
                collect_clos(d->as.fn.body);
            } else if (d->kind == AST_VAR_DECL) {
                collect_clos(d->as.var.init);
            }
        }
    }

    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_STRUCT_DECL) continue;
            /* Handle types live in the runtime headers. */
            if (mods[m].name && strcmp(mods[m].name, "maya") == 0)
                continue;
            if (mods[m].name && strcmp(mods[m].name, "zeus") == 0) {
                const char *sn = d->as.strct.name;
                if (sn && (strcmp(sn, "Node") == 0 || strcmp(sn, "Signal") == 0))
                    continue;
            }
            emit_struct(out, d);
        }
    }
    for (int i = 0; i < typecheck_struct_inst_count(); i++) {
        Type *t = typecheck_struct_inst(i);
        if (t && t->name &&
            (strcmp(t->name, "Signal") == 0 || strcmp(t->name, "Future") == 0 ||
             strcmp(t->name, "Chan") == 0))
            continue;
        if (t && struct_targs_concrete(t)) emit_struct_type(out, t);
    }

    for (int i = 0; i < typecheck_global_count(); i++) {
        AstNode *gv = typecheck_global_var(i);
        const char *cn = typecheck_global_cname(i);
        if (!gv || !cn) continue;
        emit_var_decl_type(out, gv->ty, cn);
        fprintf(out, ";\n");
    }
    if (typecheck_global_count()) fprintf(out, "\n");
    for (int i = 0; i < emit_ir_mod->nfns; i++) {
        IrFn *g = &emit_ir_mod->fns[i];
        if (g->name && strcmp(g->name, "__init") == 0 && g->cname)
            fprintf(out, "void %s(void);\n", g->cname);
    }

    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        /* std module seams are declared by the runtime headers included above
           (yuga_rt.h / zeus_rt.h / net_rt.h / maya_rt.h). Bodyless fns in
           user modules are app C seams: emit an extern prototype so the
           generated C compiles against app-supplied C (driver links a
           `runtime/<app>_runtime.c` beside the entry file, if present). */
        int std_mod = yuga_is_std_path(mods[m].path);
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_FN_DECL || d->as.fn.tparam_count) continue;
            if (!yuga_dce_keep(d)) continue;
            if (d->as.fn.is_intrinsic) {
                if (std_mod) continue;
                emit_fn_sig(out, d, 0);
                fprintf(out, ";\n");
                continue;
            }
            int is_main = (m == 0 && strcmp(d->as.fn.name, "main") == 0);
            if (is_main) continue;
            emit_fn_sig(out, d, 0);
            fprintf(out, ";\n");
        }
    }
    for (int i = 0; i < nclos_nodes; i++) emit_clos_env(out, clos_nodes[i]);
    for (int i = 0; i < nclos_nodes; i++) emit_clos_sig(out, clos_nodes[i], 1);
    if (emit_ir_mod) {
        int nfn = emit_ir_mod->nfns;
        int *seen = nfn ? (int *)calloc((size_t)nfn, sizeof(int)) : NULL;
        int nseen = 0;
        for (int i = 0; i < nfn; i++) {
            IrFn *cf = &emit_ir_mod->fns[i];
            if (!is_mono_clos(cf)) continue;
            emit_mono_clos_env(out, cf, seen, &nseen, nfn);
            emit_mono_clos_sig(out, cf, 1);
        }
        free(seen);
    }
    for (int i = 0; i < typecheck_mono_count(); i++) {
        AstNode *fn = typecheck_mono_fn(i);
        if (!fn || !yuga_dce_keep(fn)) continue;
        set_mono_subst(i);
        emit_fn_sig(out, fn, 0);
        fprintf(out, ";\n");
        clear_subst();
    }
    fprintf(out, "\n");

    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind == AST_FN_DECL && d->as.fn.used_as_value && !d->as.fn.is_intrinsic &&
                !d->as.fn.tparam_count && yuga_dce_keep(d))
                emit_as_fn_tramp(out, d);
        }
    }

    cur_is_main_mod = 1;
    for (int i = 0; i < nclos_nodes; i++) emit_closure_fn(out, clos_nodes[i]);
    if (emit_ir_mod) {
        for (int i = 0; i < emit_ir_mod->nfns; i++) {
            IrFn *cf = &emit_ir_mod->fns[i];
            if (!is_mono_clos(cf) || !cf->lowered) continue;
            emit_mono_clos_sig(out, cf, 0);
            emit_ir_fn_body(out, cf, 0);
        }
    }

    for (int i = 0; i < typecheck_mono_count(); i++) {
        AstNode *fn = typecheck_mono_fn(i);
        if (!fn || !yuga_dce_keep(fn)) continue;
        set_mono_subst(i);
        emit_fn(out, fn, 0);
        clear_subst();
    }

    for (int i = 0; i < emit_ir_mod->nfns; i++) {
        IrFn *g = &emit_ir_mod->fns[i];
        if (!g->name || strcmp(g->name, "__init") != 0 || !g->lowered) continue;
        fprintf(out, "void %s(void)", g->cname);
        emit_ir_fn_body(out, g, 0);
    }

    for (int m = nmods - 1; m >= 0; m--) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        cur_mod_name = mods[m].name;
        cur_is_main_mod = (m == 0);
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_FN_DECL || d->as.fn.is_intrinsic || d->as.fn.tparam_count)
                continue;
            if (!yuga_dce_keep(d)) continue;
            int is_main = (m == 0 && strcmp(d->as.fn.name, "main") == 0);
            if (is_main && test_mode) continue; /* the runner owns `main` */
            if (d->as.fn.is_server && server_split) {
                /* Client build: emit the signature with a trap body. The real
                   body is never emitted, so its literals are provably absent. */
                emit_fn_sig(out, d, 0);
                fprintf(out,
                        " { yuga_panic(\"<server>\", 0, \"server function excluded "
                        "from client build\"); }\n");
                continue;
            }
            emit_fn(out, d, is_main);
        }
    }

    if (test_mode) {
        /* The runner is thin: it names each test through `test.begin`, calls
           the collected `#[test]` fns, and defers the report to `test.ok` /
           `test.summary`. Naming the test *before* the call means a trap (which
           aborts without unwinding) still reports which test was running. */
        fprintf(out, "\nint main(void) {\n");
        /* Module-level `let` initializers run in per-module `__init` fns that
           the program's own `main` would call; the runner replaces `main`, so
           it must call them too (std tables, arena, etc.). */
        if (emit_ir_mod) {
            for (int i = emit_ir_mod->nfns - 1; i >= 0; i--) {
                IrFn *g = &emit_ir_mod->fns[i];
                if (g->name && strcmp(g->name, "__init") == 0 && g->cname)
                    fprintf(out, "    %s();\n", g->cname);
            }
        }
        for (int m = 0; m < nmods; m++) {
            AstNode *p = mods[m].ast;
            if (!p) continue;
            for (size_t i = 0; i < p->as.program.decl_count; i++) {
                AstNode *d = p->as.program.decls[i];
                if (d->kind != AST_FN_DECL || !d->as.fn.is_test) continue;
                if (!d->as.fn.cname || !yuga_dce_keep(d)) continue;
                char label[192];
                if (m == 0 || !mods[m].name)
                    snprintf(label, sizeof label, "%s",
                             d->as.fn.name ? d->as.fn.name : "test");
                else
                    snprintf(label, sizeof label, "%s.%s", mods[m].name,
                             d->as.fn.name ? d->as.fn.name : "test");
                fprintf(out, "    yuga_test_begin(");
                emit_str_lit(out, label);
                fprintf(out, ");\n");
                fprintf(out, "    %s();\n", d->as.fn.cname);
                fprintf(out, "    yuga_test_ok();\n");
            }
        }
        fprintf(out, "    return yuga_test_summary();\n}\n");
    }

    emit_ir_mod = NULL;
    ir_free(ir);
}
