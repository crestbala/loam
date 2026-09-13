/**
 * ir.c — lower the checked AST into the backend-neutral IR.
 *
 * The AST is a tree of nested statements; the IR is a list of basic blocks with
 * explicit terminators. Everything a backend would otherwise re-derive is made
 * explicit here: control flow, drops, and the place a memory access names.
 *
 * Capturing closures become a heap env + function pointer (`IR_CLOS`), dropped
 * like Box. Generic instances are lowered after the named functions, with type
 * parameters already substituted.
 */
#include "ir.h"
#include "diagnostics.h"
#include "lexer.h"
#include "sema/typecheck.h"

#include <stdlib.h>
#include <string.h>

static IrFn *F;
static YugaModule *Gmods;
static int Gnmods, Gcur;
static const char **subst_names;
static Type **subst_args;
static size_t subst_n;
static const char *cname_override;

/** Substitute type parameters for the generic instance being lowered. */
static Type *ir_subst(Type *t) {
    if (!t || !subst_n) return t;
    return typecheck_subst(t, subst_names, subst_args, subst_n);
}

static const char *fn_cname_in(YugaModule *m, const char *name) {
    AstNode *p = m ? m->ast : NULL;
    if (!p) return NULL;
    for (size_t i = 0; i < p->as.program.decl_count; i++) {
        AstNode *d = p->as.program.decls[i];
        if (d->kind == AST_FN_DECL && d->as.fn.name && strcmp(d->as.fn.name, name) == 0)
            return d->as.fn.cname;
    }
    return NULL;
}

/** Linkage symbol for a call target. Typecheck already assigned every function
    a unique cname; the IR records which one rather than re-deriving mangling. */
static const char *resolve_callee(AstNode *cal) {
    if (!cal) return NULL;
    if (cal->kind == AST_IDENT) {
        /* The binding typecheck recorded (a default like `= __noop` is
           resolved in the fn's own module, whatever module the call
           appears in); fall back to a name search only when unbound. */
        if (cal->as.ident.resolved && cal->as.ident.resolved->kind == AST_FN_DECL &&
            cal->as.ident.resolved->as.fn.cname)
            return cal->as.ident.resolved->as.fn.cname;
        const char *c = fn_cname_in(&Gmods[Gcur], cal->as.ident.name);
        if (c) return c;
        for (int i = 0; i < Gnmods; i++) {
            c = fn_cname_in(&Gmods[i], cal->as.ident.name);
            if (c) return c;
        }
        return cal->as.ident.name;
    }
    if (cal->kind == AST_FIELD && cal->as.access.target &&
        cal->as.access.target->kind == AST_IDENT) {
        const char *mod = cal->as.access.target->as.ident.name;
        for (int i = 0; i < Gnmods; i++)
            if (Gmods[i].name && strcmp(Gmods[i].name, mod) == 0) {
                const char *c = fn_cname_in(&Gmods[i], cal->as.access.field);
                if (c) return c;
            }
        return cal->as.access.field;
    }
    return NULL;
}

static int new_local(Type *ty, const char *name, int is_param) {
    if (F->nlocals >= F->lcap) {
        F->lcap = F->lcap ? F->lcap * 2 : 16;
        F->locals = (IrLocal *)realloc(F->locals, (size_t)F->lcap * sizeof(IrLocal));
    }
    IrLocal *l = &F->locals[F->nlocals];
    memset(l, 0, sizeof(*l));
    l->id = F->nlocals;
    l->ty = ir_subst(ty);
    l->name = name;
    l->is_param = is_param;
    l->needs_drop = type_needs_drop(l->ty);
    return F->nlocals++;
}

static int local_for_global(AstNode *gv);

static int new_block(void) {
    if (F->nblocks >= F->bcap) {
        F->bcap = F->bcap ? F->bcap * 2 : 8;
        F->blocks = (IrBlock *)realloc(F->blocks, (size_t)F->bcap * sizeof(IrBlock));
    }
    IrBlock *b = &F->blocks[F->nblocks];
    memset(b, 0, sizeof(*b));
    b->id = F->nblocks;
    b->term = IR_TERM_UNREACHABLE;
    b->term_val = -1;
    b->succ[0] = b->succ[1] = -1;
    return F->nblocks++;
}

static int CUR; /* block being appended to */

/* break / continue targets, innermost last. Grows: a fixed cap skipped the
   push once it was full but still ran the matching pop, which unbalanced the
   stack and pointed break/continue at an enclosing loop's blocks. */
static int *loop_cont;
static int *loop_brk;
static int *loop_base;
static int nloop, loop_cap;

static void loop_push(int cont, int brk, int base) {
    if (nloop == loop_cap) {
        loop_cap = loop_cap ? loop_cap * 2 : 16;
        loop_cont = (int *)realloc(loop_cont, (size_t)loop_cap * sizeof(int));
        loop_brk = (int *)realloc(loop_brk, (size_t)loop_cap * sizeof(int));
        loop_base = (int *)realloc(loop_base, (size_t)loop_cap * sizeof(int));
        if (!loop_cont || !loop_brk || !loop_base) yuga_fatal("out of memory");
    }
    loop_cont[nloop] = cont;
    loop_brk[nloop] = brk;
    loop_base[nloop] = base;
    nloop++;
}

static IrInst *emit(IrOp op, SourceLoc loc) {
    IrBlock *b = &F->blocks[CUR];
    if (b->ninsts >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->insts = (IrInst *)realloc(b->insts, (size_t)b->cap * sizeof(IrInst));
    }
    IrInst *i = &b->insts[b->ninsts++];
    memset(i, 0, sizeof(*i));
    i->op = op;
    i->dst = i->a = i->b = -1;
    i->loc = loc;
    return i;
}

static void term_ret(int val, SourceLoc loc) {
    IrBlock *b = &F->blocks[CUR];
    if (b->term != IR_TERM_UNREACHABLE) return;
    b->term = IR_TERM_RET;
    b->term_val = val;
    b->term_loc = loc;
}

static void term_jmp(int to) {
    IrBlock *b = &F->blocks[CUR];
    if (b->term != IR_TERM_UNREACHABLE) return;
    b->term = IR_TERM_JMP;
    b->succ[0] = to;
}

static void term_br(int cond, int t, int f) {
    IrBlock *b = &F->blocks[CUR];
    if (b->term != IR_TERM_UNREACHABLE) return;
    b->term = IR_TERM_BR;
    b->term_val = cond;
    b->succ[0] = t;
    b->succ[1] = f;
}

static void emit_drops_from(int lo, SourceLoc loc) {
    for (int i = F->nlocals - 1; i >= lo; i--) {
        if (!F->locals[i].needs_drop) continue;
        IrInst *d = emit(IR_DROP, loc);
        d->a = i;
    }
}

static int block_closed(void) { return F->blocks[CUR].term != IR_TERM_UNREACHABLE; }

/* --- scope: name -> local id ------------------------------------------- */

typedef struct Scope {
    const char *name;
    int local;
    int depth;
    struct Scope *next;
} Scope;

static Scope *scopes;
static int sdepth;

static void scope_push(const char *name, int local) {
    Scope *s = (Scope *)calloc(1, sizeof(Scope));
    s->name = name;
    s->local = local;
    s->depth = sdepth;
    s->next = scopes;
    scopes = s;
}

static void scope_pop_to(int d) {
    while (scopes && scopes->depth > d) {
        Scope *s = scopes;
        scopes = s->next;
        free(s);
    }
}

static int scope_find(const char *name) {
    for (Scope *s = scopes; s; s = s->next)
        if (strcmp(s->name, name) == 0) return s->local;
    return -1;
}

/* --- lowering ----------------------------------------------------------- */

static int lower_expr(AstNode *n);
static void lower_stmt(AstNode *n);

static IrPlace *place_local(int id, Type *ty) {
    IrPlace *p = (IrPlace *)calloc(1, sizeof(IrPlace));
    p->kind = IR_PL_LOCAL;
    p->local = id;
    p->ty = ty;
    return p;
}

static IrPlace *clone_place(const IrPlace *p) {
    if (!p) return NULL;
    IrPlace *c = (IrPlace *)calloc(1, sizeof(IrPlace));
    *c = *p;
    c->base = clone_place(p->base);
    return c;
}

/** Build the place an lvalue names, or NULL if it is not an lvalue.
 *
 * `allow_temp` makes a non-place base legal by evaluating it into a fresh
 * local and rooting the path there, so reads like `[3]int{8,12,16}[i]`,
 * `f().x`, and `Point{..}.x` lower instead of bailing. It must stay off for
 * assignment targets and `&`: materializing there would silently write to (or
 * hand out the address of) a temporary the caller never sees.
 */
static IrPlace *lower_place_mode(AstNode *n, int allow_temp) {
    if (!n) return NULL;
    switch (n->kind) {
        case AST_IDENT: {
            int id = scope_find(n->as.ident.name);
            if (id < 0) return NULL;
            return place_local(id, id >= 0 && id < F->nlocals ? F->locals[id].ty : ir_subst(n->ty));
        }
        case AST_FIELD: {
            if (n->as.access.resolved && n->as.access.resolved->kind == AST_VAR_DECL) {
                int id = local_for_global(n->as.access.resolved);
                if (id < 0) return NULL;
                return place_local(id, ir_subst(n->ty));
            }
            IrPlace *base = lower_place_mode(n->as.access.target, allow_temp);
            if (!base) return NULL;
            IrPlace *p = (IrPlace *)calloc(1, sizeof(IrPlace));
            p->kind = IR_PL_FIELD;
            p->field = n->as.access.field;
            p->base = base;
            p->ty = ir_subst(n->ty);
            return p;
        }
        case AST_INDEX: {
            IrPlace *base = lower_place_mode(n->as.access.target, allow_temp);
            if (!base) return NULL;
            int idx = lower_expr(n->as.access.index);
            if (!(n->flags & ASTF_INDEX_SAFE)) {
                Type *t = n->as.access.target ? ir_subst(n->as.access.target->ty) : NULL;
                if (t && (t->kind == TY_PTR || t->kind == TY_BOX)) t = t->elem;
                if (t && (t->kind == TY_VEC || t->kind == TY_STRING)) {
                    IrPlace *fp = (IrPlace *)calloc(1, sizeof(IrPlace));
                    fp->kind = IR_PL_FIELD;
                    fp->field = "len";
                    fp->base = clone_place(base);
                    fp->ty = ty_int();
                    int lenl = new_local(ty_int(), NULL, 0);
                    IrInst *ld = emit(IR_LOAD, n->loc);
                    ld->dst = lenl;
                    ld->place = fp;
                    ld->ty = ty_int();
                    IrInst *bd = emit(IR_BOUND, n->loc);
                    bd->a = idx;
                    bd->b = lenl;
                    bd->ty = ty_int();
                } else {
                    int64_t len = (t && t->kind == TY_ARRAY) ? t->array_len : 0;
                    IrInst *bd = emit(IR_BOUND, n->loc);
                    bd->a = idx;
                    bd->imm = len;
                    bd->ty = ty_int();
                }
            }
            IrPlace *p = (IrPlace *)calloc(1, sizeof(IrPlace));
            p->kind = IR_PL_INDEX;
            p->base = base;
            p->index = idx;
            p->ty = ir_subst(n->ty);
            return p;
        }
        case AST_DEREF: {
            IrPlace *base = lower_place_mode(n->as.access.target, allow_temp);
            if (!base) return NULL;
            IrPlace *p = (IrPlace *)calloc(1, sizeof(IrPlace));
            p->kind = IR_PL_DEREF;
            p->base = base;
            p->ty = ir_subst(n->ty);
            return p;
        }
        default:
            /* Not a place. In a read, evaluate it once into a local and treat
               that local as the root of the path. */
            if (allow_temp) {
                int v = lower_expr(n);
                if (v < 0) return NULL;
                return place_local(v, v < F->nlocals ? F->locals[v].ty : ir_subst(n->ty));
            }
            return NULL;
    }
}

/** Strict: assignment targets and `&`, where a temporary would be wrong. */
static IrPlace *lower_place(AstNode *n) { return lower_place_mode(n, 0); }

/** Reads, where a temporary base is fine. */
static IrPlace *lower_place_rv(AstNode *n) { return lower_place_mode(n, 1); }

static int emit_named_call(const char *callee, int *args, int nargs, Type *ty,
                           SourceLoc loc) {
    IrInst *i = emit(IR_CALL, loc);
    i->dst = (ty && ty->kind != TY_VOID) ? new_local(ty, NULL, 0) : -1;
    i->callee = callee;
    i->args = args;
    i->nargs = nargs;
    i->ty = ty;
    return i->dst;
}

static int ident_is_state(AstNode *n) {
    if (!n || n->kind != AST_IDENT) return 0;
    AstNode *d = n->as.ident.resolved;
    return d && (d->flags & ASTF_STATE);
}

static int fn_returns_node(void) {
    Type *r = (F && F->sig) ? F->sig->ret : NULL;
    return r && r->kind == TY_STRUCT && r->name && strcmp(r->name, "Node") == 0;
}

static int state_sig_local(AstNode *n) {
    if (!n || n->kind != AST_IDENT || !n->as.ident.name) return -1;
    return scope_find(n->as.ident.name);
}

static int emit_state_get(int sid, SourceLoc loc) {
    int *args = (int *)calloc(1, sizeof(int));
    args[0] = sid;
    return emit_named_call("yuga_zeus_get", args, 1, ty_int(), loc);
}

static void emit_state_set(int sid, int val, SourceLoc loc) {
    int *args = (int *)calloc(2, sizeof(int));
    args[0] = sid;
    args[1] = val;
    emit_named_call("yuga_zeus_set", args, 2, ty_void(), loc);
}

static int assign_to_bin(int op) {
    if (op == TOK_PLUS_EQ) return TOK_PLUS;
    if (op == TOK_MINUS_EQ) return TOK_MINUS;
    if (op == TOK_STAR_EQ) return TOK_STAR;
    if (op == TOK_SLASH_EQ) return TOK_SLASH;
    if (op == TOK_PERCENT_EQ) return TOK_PERCENT;
    if (op == TOK_AMP_EQ) return TOK_AMP;
    if (op == TOK_PIPE_EQ) return TOK_PIPE;
    if (op == TOK_CARET_EQ) return TOK_CARET;
    if (op == TOK_SHL_EQ) return TOK_SHL;
    if (op == TOK_SHR_EQ) return TOK_SHR;
    return 0;
}

/** Lower `fmt.println` to the write/write_int/write_bool + writeln sequence.
    Variadic mixed types are not a Yuga function; this is the language's
    compile-time expansion, not a C-backend special case. */
static int lower_println(AstNode *n) {
    for (size_t k = 0; k < n->as.call.arg_count; k++) {
        if (k) {
            int sp = new_local(ty_string(), NULL, 0);
            IrInst *c = emit(IR_CONST_STR, n->loc);
            c->dst = sp;
            c->str = " ";
            c->ty = ty_string();
            int *a = (int *)calloc(1, sizeof(int));
            a[0] = sp;
            emit_named_call("yuga_fmt_write", a, 1, ty_void(), n->loc);
        }
        AstNode *arg = n->as.call.args[k];
        int v = lower_expr(arg);
        int *a = (int *)calloc(1, sizeof(int));
        a[0] = v;
        Type *t = ir_subst(arg->ty);
        if (t && t->kind == TY_STRING)
            emit_named_call("yuga_fmt_write", a, 1, ty_void(), n->loc);
        else if (t && t->kind == TY_BOOL)
            emit_named_call("yuga_fmt_write_bool", a, 1, ty_void(), n->loc);
        else if (t && t->kind == TY_FLOAT)
            emit_named_call("yuga_fmt_write_float", a, 1, ty_void(), n->loc);
        else
            emit_named_call("yuga_fmt_write_int", a, 1, ty_void(), n->loc);
    }
    emit_named_call("yuga_fmt_writeln", NULL, 0, ty_void(), n->loc);
    return -1;
}

static int lower_call(AstNode *n) {
    int dst = -1;
    if (n->ty && n->ty->kind != TY_VOID) dst = new_local(n->ty, NULL, 0);

    if (n->as.call.is_box_new) {
        int v = n->as.call.arg_count ? lower_expr(n->as.call.args[0]) : -1;
        IrInst *i = emit(IR_ALLOC, n->loc);
        i->dst = dst;
        i->a = v;
        i->ty = ir_subst(n->ty);
        return dst;
    }
    if (n->as.call.sig_cell) {
        size_t ac = n->as.call.arg_count;
        int *args = ac ? (int *)calloc(ac, sizeof(int)) : NULL;
        for (size_t k = 0; k < ac; k++)
            args[k] = lower_expr(n->as.call.args[k]);
        IrInst *i = emit(IR_CALL, n->loc);
        i->dst = n->as.call.sig_cell == 3 ? -1 : dst;
        i->args = args;
        i->nargs = (int)ac;
        if (n->as.call.sig_cell == 1 && ac)
            i->ty = ir_subst(n->as.call.args[0]->ty);
        else
            i->ty = ir_subst(n->ty);
        if (n->as.call.sig_cell == 1) i->callee = "yuga_sig_push";
        else if (n->as.call.sig_cell == 2) i->callee = "yuga_sig_load";
        else i->callee = "yuga_sig_store";
        return n->as.call.sig_cell == 3 ? -1 : dst;
    }
    if (n->as.call.fut_cell) {
        size_t ac = n->as.call.arg_count;
        int *args = ac ? (int *)calloc(ac, sizeof(int)) : NULL;
        for (size_t k = 0; k < ac; k++)
            args[k] = lower_expr(n->as.call.args[k]);
        IrInst *i = emit(IR_CALL, n->loc);
        int no_dst = n->as.call.fut_cell == 3 || n->as.call.fut_cell == 5;
        i->dst = no_dst ? -1 : dst;
        i->args = args;
        i->nargs = (int)ac;
        if (n->as.call.fut_cell == 1 && ac)
            i->ty = ir_subst(n->as.call.args[0]->ty);
        else if (n->as.call.fut_cell == 3 && ac >= 2)
            i->ty = ir_subst(n->as.call.args[1]->ty);
        else
            i->ty = ir_subst(n->ty);
        if (n->as.call.fut_cell == 1) i->callee = "yuga_fut_push";
        else if (n->as.call.fut_cell == 2) i->callee = "yuga_fut_load";
        else if (n->as.call.fut_cell == 3) i->callee = "yuga_fut_store";
        else if (n->as.call.fut_cell == 4) i->callee = "yuga_fut_ready";
        else i->callee = "yuga_fut_clear";
        return no_dst ? -1 : dst;
    }
    if (n->as.call.ch_cell) {
        size_t ac = n->as.call.arg_count;
        int *args = ac ? (int *)calloc(ac, sizeof(int)) : NULL;
        for (size_t k = 0; k < ac; k++)
            args[k] = lower_expr(n->as.call.args[k]);
        IrInst *i = emit(IR_CALL, n->loc);
        int no_dst = n->as.call.ch_cell == 2;
        i->dst = no_dst ? -1 : dst;
        i->args = args;
        i->nargs = (int)ac;
        /* inst ty = payload T (new/send/try_send) or the return type; codegen
           prints sizeof(T) from it. No []T payloads exist: Send excludes them. */
        if ((n->as.call.ch_cell == 1 || n->as.call.ch_cell == 2 || n->as.call.ch_cell == 3) && ac >= 2)
            i->ty = ir_subst(n->as.call.args[1]->ty);
        else
            i->ty = ir_subst(n->ty);
        if (n->as.call.ch_cell == 1) i->callee = "yuga_ch_alloc";
        else if (n->as.call.ch_cell == 2) i->callee = "yuga_ch_send";
        else if (n->as.call.ch_cell == 3) i->callee = "yuga_ch_try_send";
        else if (n->as.call.ch_cell == 4) i->callee = "yuga_ch_recv";
        else if (n->as.call.ch_cell == 5) i->callee = "yuga_ch_pop";
        else i->callee = "yuga_ch_ready";
        return no_dst ? -1 : dst;
    }
    if (n->as.call.is_println) return lower_println(n);
    if (n->as.call.is_vec_push) {
        int *args = (int *)calloc(2, sizeof(int));
        args[0] = n->as.call.arg_count > 0 ? lower_expr(n->as.call.args[0]) : -1;
        args[1] = n->as.call.arg_count > 1 ? lower_expr(n->as.call.args[1]) : -1;
        IrInst *i = emit(IR_CALL, n->loc);
        i->callee = "yuga_vec_push";
        i->args = args;
        i->nargs = 2;
        i->ty = n->as.call.arg_count > 1 && n->as.call.args[1]
                    ? ir_subst(n->as.call.args[1]->ty)
                    : ty_int();
        if (args[1] >= 0 && args[1] < F->nlocals && type_needs_drop(F->locals[args[1]].ty))
            F->locals[args[1]].needs_drop = 0;
        return -1;
    }
    if (n->as.call.is_vec_pop) {
        int *args = (int *)calloc(1, sizeof(int));
        args[0] = n->as.call.arg_count > 0 ? lower_expr(n->as.call.args[0]) : -1;
        IrInst *i = emit(IR_CALL, n->loc);
        i->dst = dst;
        i->callee = "yuga_vec_pop";
        i->args = args;
        i->nargs = 1;
        i->ty = ir_subst(n->ty);
        return dst;
    }
    if (n->as.call.num_builtin != NUMB_NONE) {
        size_t ac = n->as.call.arg_count;
        int *args = ac ? (int *)calloc(ac, sizeof(int)) : NULL;
        for (size_t k = 0; k < ac; k++) args[k] = lower_expr(n->as.call.args[k]);
        IrInst *i = emit(IR_CALL, n->loc);
        i->dst = dst;
        i->args = args;
        i->nargs = (int)ac;
        i->ty = ir_subst(n->ty);
        i->callee = yuga_dup(numeric_builtin_cname(n->as.call.num_builtin, n->ty));
        return dst;
    }
    if (n->as.call.is_sizeof) {
        IrInst *i = emit(IR_SIZEOF, n->loc);
        i->dst = dst;
        i->a = -1;
        i->ty = n->as.call.arg_count ? ir_subst(n->as.call.args[0]->ty) : ty_void();
        return dst;
    }
    if (n->as.call.c_builtin) {
        int nargs = (int)n->as.call.arg_count;
        int *args = nargs ? (int *)calloc((size_t)nargs, sizeof(int)) : NULL;
        for (int k = 0; k < nargs; k++)
            args[k] = lower_expr(n->as.call.args[k]);
        IrInst *i = emit(IR_CALL, n->loc);
        i->dst = dst;
        i->args = args;
        i->nargs = nargs;
        i->ty = ir_subst(n->ty);
        i->callee = n->as.call.c_builtin;
        for (int k = 0; k < nargs; k++) {
            int a = args[k];
            if (a >= 0 && a < F->nlocals && type_needs_drop(F->locals[a].ty))
                F->locals[a].needs_drop = 0;
        }
        return dst;
    }

    int callee_local = -1;
    if (n->as.call.is_fn_val)
        callee_local = lower_expr(n->as.call.callee);

    const char *callee = NULL;
    if (!n->as.call.is_fn_val) {
        if (subst_n)
            callee = typecheck_callee_cname(n, subst_names, subst_args, subst_n);
        if (!callee) callee = n->as.call.resolved_cname;
        if (!callee) callee = resolve_callee(n->as.call.callee);
    }
    int bind_state = callee && strcmp(callee, "yuga_zeus_bind_n") == 0 &&
                     n->as.call.arg_count >= 2 && ident_is_state(n->as.call.args[1]);

    int *args = n->as.call.arg_count
                    ? (int *)calloc(n->as.call.arg_count, sizeof(int))
                    : NULL;
    for (size_t k = 0; k < n->as.call.arg_count; k++) {
        if (bind_state && k == 1) {
            args[k] = state_sig_local(n->as.call.args[k]);
            if (args[k] < 0) {
                F->lowered = 0;
                return -1;
            }
        } else {
            args[k] = lower_expr(n->as.call.args[k]);
        }
    }
    if (bind_state) callee = "yuga_zeus_bind";

    IrInst *i = emit(n->as.call.is_fn_val ? IR_CALL_VAL : IR_CALL, n->loc);
    i->dst = dst;
    i->args = args;
    i->nargs = (int)n->as.call.arg_count;
    i->ty = ir_subst(n->ty);
    if (n->as.call.is_fn_val)
        i->a = callee_local;
    else
        i->callee = callee;
    /* Ownership of Box/[]T args leaves with the callee. */
    for (int k = 0; k < i->nargs; k++) {
        int a = i->args[k];
        if (a >= 0 && a < F->nlocals && type_needs_drop(F->locals[a].ty))
            F->locals[a].needs_drop = 0;
    }
    return dst;
}

/** Lower an expression, returning the local holding its value. */
/** One arm of an `if` used as a value: its statements run, and its trailing
 *  expression is moved into `dst`. */
static void lower_block_value(AstNode *blk, int dst, Type *ty) {
    if (!blk || blk->kind != AST_BLOCK) return;
    sdepth++;
    size_t c = blk->as.block.stmt_count;
    for (size_t i = 0; i < c; i++) {
        AstNode *st = blk->as.block.stmts[i];
        if (i + 1 == c && st && st->kind == AST_EXPR_STMT) {
            int v = lower_expr(st->as.expr_stmt.expr);
            if (v >= 0 && !block_closed()) {
                IrInst *m = emit(IR_MOVE, st->loc);
                m->dst = dst;
                m->a = v;
                m->ty = ir_subst(ty);
            }
            break;
        }
        lower_stmt(st);
        if (block_closed()) break;
    }
    scope_pop_to(sdepth - 1);
    sdepth--;
}

static int lower_expr(AstNode *n) {
    if (!n) return -1;
    switch (n->kind) {
        case AST_IF: {
            /* value position: both arms write the same result local */
            int d = new_local(n->ty, NULL, 0);
            int c = lower_expr(n->as.if_stmt.cond);
            int bt = new_block(), be = new_block(), join = new_block();
            term_br(c, bt, be);
            CUR = bt;
            lower_block_value(n->as.if_stmt.then_block, d, n->ty);
            term_jmp(join);
            CUR = be;
            lower_block_value(n->as.if_stmt.else_block, d, n->ty);
            term_jmp(join);
            CUR = join;
            return d;
        }
        case AST_NUMBER: {
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_CONST_INT, n->loc);
            i->dst = d;
            i->imm = n->as.lit.value;
            i->ty = n->ty;
            return d;
        }
        case AST_FLOAT: {
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_CONST_FLOAT, n->loc);
            i->dst = d;
            i->fimm = n->as.lit.f;
            i->ty = n->ty;
            return d;
        }
        case AST_BOOL: {
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_CONST_BOOL, n->loc);
            i->dst = d;
            i->imm = n->as.lit.b;
            i->ty = n->ty;
            return d;
        }
        case AST_STRING: {
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_CONST_STR, n->loc);
            i->dst = d;
            i->str = n->as.lit.str;
            i->ty = n->ty;
            return d;
        }
        case AST_IDENT:
            if (n->flags & ASTF_FN_VAL) {
                int d = new_local(n->ty, NULL, 0);
                IrInst *i = emit(IR_FN_VAL, n->loc);
                i->dst = d;
                i->callee = resolve_callee(n);
                i->ty = ir_subst(n->ty);
                return d;
            }
            if (ident_is_state(n)) {
                int sid = state_sig_local(n);
                if (sid < 0) {
                    F->lowered = 0;
                    return -1;
                }
                return emit_state_get(sid, n->loc);
            }
            /* fall through to place */
        case AST_FIELD:
        case AST_INDEX:
        case AST_DEREF: {
            IrPlace *p = lower_place_rv(n);
            if (!p) {
                F->lowered = 0;
                return -1;
            }
            int src;
            if (p->kind == IR_PL_LOCAL)
                src = p->local;
            else {
                int d = new_local(n->ty, NULL, 0);
                IrInst *i = emit(IR_LOAD, n->loc);
                i->dst = d;
                i->place = p;
                i->ty = ir_subst(n->ty);
                src = d;
                /* A load of an owning type is a copy for reading/calling, not
                   a transfer — the place still owns it. */
                if (!(n->flags & ASTF_MOVED) && type_needs_drop(F->locals[d].ty))
                    F->locals[d].needs_drop = 0;
            }
            /* Ownership transfer: the source is dead after this use. */
            if ((n->flags & ASTF_MOVED) && n->ty && type_needs_drop(n->ty)) {
                int d = new_local(n->ty, NULL, 0);
                IrInst *mv = emit(IR_MOVE, n->loc);
                mv->dst = d;
                mv->a = src;
                mv->ty = ir_subst(n->ty);
                return d;
            }
            return src;
        }
        case AST_ADDR: {
            IrPlace *p = lower_place(n->as.access.target);
            if (!p) {
                F->lowered = 0;
                return -1;
            }
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_ADDR, n->loc);
            i->dst = d;
            i->place = p;
            i->is_mut = n->as.access.is_mut;
            i->ty = ir_subst(n->ty);
            return d;
        }
        case AST_BINARY: {
            TokenKind op = n->as.binary.op;
            /* && and || short-circuit: assign lhs, overwrite with rhs if taken. */
            if (op == TOK_AMP_AMP || op == TOK_PIPE_PIPE) {
                int d = new_local(n->ty, NULL, 0);
                int lhs = lower_expr(n->as.binary.left);
                IrInst *m = emit(IR_MOVE, n->loc);
                m->dst = d;
                m->a = lhs;
                m->ty = ir_subst(n->ty);
                int rhs_bb = new_block(), join = new_block();
                if (op == TOK_AMP_AMP)
                    term_br(lhs, rhs_bb, join);
                else
                    term_br(lhs, join, rhs_bb);
                CUR = rhs_bb;
                int rhs = lower_expr(n->as.binary.right);
                IrInst *m2 = emit(IR_MOVE, n->loc);
                m2->dst = d;
                m2->a = rhs;
                m2->ty = ir_subst(n->ty);
                term_jmp(join);
                CUR = join;
                return d;
            }
            int a = lower_expr(n->as.binary.left);
            int b = lower_expr(n->as.binary.right);
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_BIN, n->loc);
            i->dst = d;
            i->a = a;
            i->b = b;
            i->binop = (int)op;
            i->ty = ir_subst(n->ty);
            i->checked = n->ty && n->ty->kind == TY_INT &&
                         (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR ||
                          op == TOK_SLASH || op == TOK_PERCENT);
            return d;
        }
        case AST_UNARY: {
            int a = lower_expr(n->as.unary.operand);
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_UN, n->loc);
            i->dst = d;
            i->a = a;
            i->binop = (int)n->as.unary.op;
            i->ty = ir_subst(n->ty);
            return d;
        }
        case AST_INCDEC: {
            TokenKind bop = n->as.incdec.is_dec ? TOK_MINUS : TOK_PLUS;
            /* Closure-captured `let mut`: same signal path as compound assign. */
            if (ident_is_state(n->as.incdec.operand)) {
                int sid = state_sig_local(n->as.incdec.operand);
                if (sid < 0) {
                    F->lowered = 0;
                    return -1;
                }
                int cur = emit_state_get(sid, n->loc);
                int one = new_local(ty_int(), NULL, 0);
                IrInst *k = emit(IR_CONST_INT, n->loc);
                k->dst = one;
                k->imm = 1;
                k->ty = ty_int();
                int nx = new_local(ty_int(), NULL, 0);
                IrInst *b = emit(IR_BIN, n->loc);
                b->dst = nx;
                b->a = cur;
                b->b = one;
                b->binop = (int)bop;
                b->ty = ty_int();
                b->checked = 1;
                emit_state_set(sid, nx, n->loc);
                return n->as.incdec.is_post ? cur : nx;
            }
            IrPlace *p = lower_place(n->as.incdec.operand);
            if (!p) {
                F->lowered = 0;
                return -1;
            }
            int src;
            if (p->kind == IR_PL_LOCAL) {
                if (n->as.incdec.is_post) {
                    /* The store below overwrites the local; keep the old
                       value in a fresh temp so postfix yields it. */
                    int ld = new_local(n->ty, NULL, 0);
                    IrInst *mv = emit(IR_MOVE, n->loc);
                    mv->dst = ld;
                    mv->a = p->local;
                    mv->ty = ir_subst(n->ty);
                    src = ld;
                } else {
                    src = p->local;
                }
            } else {
                int ld = new_local(n->ty, NULL, 0);
                IrInst *l = emit(IR_LOAD, n->loc);
                l->dst = ld;
                l->place = p;
                l->ty = ir_subst(n->ty);
                src = ld;
            }
            int one = new_local(n->ty, NULL, 0);
            if (n->ty && n->ty->kind == TY_FLOAT) {
                IrInst *k = emit(IR_CONST_FLOAT, n->loc);
                k->dst = one;
                k->fimm = 1.0;
                k->ty = ir_subst(n->ty);
            } else {
                IrInst *k = emit(IR_CONST_INT, n->loc);
                k->dst = one;
                k->imm = 1;
                k->ty = ir_subst(n->ty);
            }
            int nx = new_local(n->ty, NULL, 0);
            IrInst *b = emit(IR_BIN, n->loc);
            b->dst = nx;
            b->a = src;
            b->b = one;
            b->binop = (int)bop;
            b->ty = ir_subst(n->ty);
            if (n->ty && n->ty->kind != TY_FLOAT) b->checked = 1;
            IrInst *st = emit(IR_STORE, n->loc);
            st->place = clone_place(p);
            st->a = nx;
            st->ty = ir_subst(n->ty);
            return n->as.incdec.is_post ? src : nx;
        }
        case AST_CAST: {
            int a = lower_expr(n->as.cast.expr);
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_CAST, n->loc);
            i->dst = d;
            i->a = a;
            i->ty = ir_subst(n->ty);
            i->conv_mode = n->as.cast.conv_mode;
            return d;
        }
        case AST_CALL:
            return lower_call(n);
        case AST_STRUCT_LIT: {
            Type *t = ir_subst(n->ty);
            int nf = t && t->kind == TY_STRUCT ? (int)t->field_count
                                               : (int)n->as.struct_lit.field_count;
            int *args = nf ? (int *)calloc((size_t)nf, sizeof(int)) : NULL;
            if (t && t->kind == TY_STRUCT) {
                for (int f = 0; f < nf; f++) {
                    args[f] = -1;
                    for (size_t k = 0; k < n->as.struct_lit.field_count; k++)
                        if (t->field_names[f] && n->as.struct_lit.fields[k].name &&
                            strcmp(t->field_names[f], n->as.struct_lit.fields[k].name) == 0) {
                            args[f] = lower_expr(n->as.struct_lit.fields[k].init);
                            break;
                        }
                }
            } else {
                for (int k = 0; k < nf; k++)
                    args[k] = lower_expr(n->as.struct_lit.fields[k].init);
            }
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_STRUCT_LIT, n->loc);
            i->dst = d;
            i->args = args;
            i->nargs = nf;
            i->ty = t;
            for (int k = 0; k < nf; k++)
                if (args[k] >= 0 && args[k] < F->nlocals &&
                    type_needs_drop(F->locals[args[k]].ty))
                    F->locals[args[k]].needs_drop = 0;
            return d;
        }
        case AST_ARRAY_LIT: {
            int nf = (int)n->as.array_lit.count;
            int *args = nf ? (int *)calloc((size_t)nf, sizeof(int)) : NULL;
            for (int k = 0; k < nf; k++) args[k] = lower_expr(n->as.array_lit.elems[k]);
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_ARRAY_LIT, n->loc);
            i->dst = d;
            i->args = args;
            i->nargs = nf;
            i->ty = ir_subst(n->ty);
            if (i->ty && i->ty->kind == TY_VEC) {
                for (int k = 0; k < nf; k++)
                    if (args[k] >= 0 && args[k] < F->nlocals &&
                        type_needs_drop(F->locals[args[k]].ty))
                        F->locals[args[k]].needs_drop = 0;
            }
            return d;
        }
        case AST_CLOSURE: {
            if (!n->as.fn.clos_id) n->as.fn.clos_id = 1;
            int nc = (int)n->as.fn.cap_count;
            int *args = nc ? (int *)calloc((size_t)nc, sizeof(int)) : NULL;
            for (int k = 0; k < nc; k++) {
                int id = scope_find(n->as.fn.caps[k]);
                if (id < 0) {
                    F->lowered = 0;
                    return -1;
                }
                args[k] = id;
            }
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_CLOS, n->loc);
            i->dst = d;
            i->imm = n->as.fn.clos_id;
            i->args = args;
            i->nargs = nc;
            i->ty = ir_subst(n->ty);
            {
                char buf[256];
                if (cname_override)
                    snprintf(buf, sizeof buf, "yuga_clos_%d_%s", n->as.fn.clos_id, cname_override);
                else
                    snprintf(buf, sizeof buf, "yuga_clos_%d", n->as.fn.clos_id);
                i->callee = yuga_dup(buf);
            }
            return d;
        }
        default:
            F->lowered = 0;
            return -1;
    }
}

/**
 * Ownership follows the move: a local that has been moved out of is dead, so
 * it must not appear in a drop list. The C backend gets away without this
 * because `yuga_move_ptr` NULLs the source and `yuga_drop` skips NULL — a
 * runtime check standing in for a static fact. Stating it here means a backend
 * that does not null on move is still correct.
 */
static void transfer_moved(void) {
    for (int b = 0; b < F->nblocks; b++)
        for (int i = 0; i < F->blocks[b].ninsts; i++) {
            IrInst *in = &F->blocks[b].insts[i];
            if (in->op == IR_MOVE && in->a >= 0 && in->a < F->nlocals)
                F->locals[in->a].needs_drop = 0;
        }
}

/**
 * Drop every owning local on each returning path, innermost first.
 *
 * Runs after lowering rather than during it: which locals still own anything is
 * only known once every move in the function has been seen. A local being
 * returned is not dropped — ownership leaves with it.
 */
static void emit_drops_at_exits(void) {
    transfer_moved();
    for (int b = 0; b < F->nblocks; b++) {
        IrBlock *bb = &F->blocks[b];
        if (bb->term != IR_TERM_RET) continue;
        int saved = CUR;
        CUR = b;
        for (int i = F->nlocals - 1; i >= 0; i--) {
            if (!F->locals[i].needs_drop) continue;
            if (i == bb->term_val) continue;
            IrInst *d = emit(IR_DROP, bb->term_loc);
            d->a = i;
        }
        CUR = saved;
    }
}

static void lower_stmt(AstNode *n) {
    if (!n || block_closed()) return;
    switch (n->kind) {
        case AST_VAR_DECL: {
            if (n->flags & ASTF_STATE) {
                Type *sig_ty = typecheck_signal_type();
                if (!sig_ty) {
                    F->lowered = 0;
                    return;
                }
                int initv = n->as.var.init ? lower_expr(n->as.var.init) : -1;
                if (initv < 0) {
                    initv = new_local(ty_int(), NULL, 0);
                    IrInst *z = emit(IR_CONST_INT, n->loc);
                    z->dst = initv;
                    z->imm = 0;
                    z->ty = ty_int();
                }
                int *args = (int *)calloc(1, sizeof(int));
                args[0] = initv;
                const char *callee = fn_returns_node() ? "yuga_zeus_hook_signal" : "yuga_zeus_signal";
                int id = emit_named_call(callee, args, 1, sig_ty, n->loc);
                if (id >= 0 && n->as.var.name) F->locals[id].name = n->as.var.name;
                scope_push(n->as.var.name, id);
                return;
            }
            int v = n->as.var.init ? lower_expr(n->as.var.init) : -1;
            int id = new_local(n->ty, n->as.var.name, 0);
            scope_push(n->as.var.name, id);
            if (v >= 0) {
                IrInst *i = emit(IR_MOVE, n->loc);
                i->dst = id;
                i->a = v;
                i->ty = ir_subst(n->ty);
            }
            return;
        }
        case AST_ASSIGN: {
            if (ident_is_state(n->as.assign.left)) {
                int sid = state_sig_local(n->as.assign.left);
                if (sid < 0) {
                    F->lowered = 0;
                    return;
                }
                int v = lower_expr(n->as.assign.right);
                int bin = assign_to_bin((int)n->as.assign.op);
                if (bin) {
                    int cur = emit_state_get(sid, n->loc);
                    int d = new_local(ty_int(), NULL, 0);
                    IrInst *b = emit(IR_BIN, n->loc);
                    b->dst = d;
                    b->a = cur;
                    b->b = v;
                    b->binop = bin;
                    b->ty = ty_int();
                    b->checked = 1;
                    v = d;
                }
                emit_state_set(sid, v, n->loc);
                return;
            }
            int v = lower_expr(n->as.assign.right);
            IrPlace *p = lower_place(n->as.assign.left);
            if (!p) {
                F->lowered = 0;
                return;
            }
            /* Compound assignment already carries its op in the checked AST for
               the simple case; anything else arrives as a plain store. */
            IrInst *i = emit(IR_STORE, n->loc);
            i->place = p;
            i->a = v;
            i->binop = (int)n->as.assign.op;
            i->ty = ir_subst(n->as.assign.left->ty);
            return;
        }
        case AST_EXPR_STMT:
            lower_expr(n->as.expr_stmt.expr);
            return;
        case AST_RETURN: {
            int v = n->as.ret.expr ? lower_expr(n->as.ret.expr) : -1;
            term_ret(v, n->loc);
            return;
        }
        case AST_BLOCK: {
            sdepth++;
            for (size_t i = 0; i < n->as.block.stmt_count; i++) {
                lower_stmt(n->as.block.stmts[i]);
                if (block_closed()) break;
            }
            scope_pop_to(sdepth - 1);
            sdepth--;
            return;
        }
        case AST_IF: {
            int c = lower_expr(n->as.if_stmt.cond);
            int bt = new_block();
            int be = n->as.if_stmt.else_block ? new_block() : -1;
            int join = new_block();
            term_br(c, bt, be >= 0 ? be : join);

            CUR = bt;
            lower_stmt(n->as.if_stmt.then_block);
            term_jmp(join);

            if (be >= 0) {
                CUR = be;
                lower_stmt(n->as.if_stmt.else_block);
                term_jmp(join);
            }
            CUR = join;
            return;
        }
        case AST_FOR: {
            /* `for i in lo..hi` — header tests, body runs, latch increments. */
            AstNode *rng = n->as.for_stmt.iter;
            if (!rng || rng->kind != AST_BINARY) {
                F->lowered = 0;
                return;
            }
            int lo = lower_expr(rng->as.binary.left);
            int hi = lower_expr(rng->as.binary.right);
            int iv = new_local(ty_int(), n->as.for_stmt.var, 0);
            IrInst *init = emit(IR_MOVE, n->loc);
            init->dst = iv;
            init->a = lo;

            int head = new_block(), body = new_block(), latch = new_block(),
                exit = new_block();
            term_jmp(head);

            CUR = head;
            int c = new_local(ty_bool(), NULL, 0);
            IrInst *cmp = emit(IR_BIN, n->loc);
            cmp->dst = c;
            cmp->a = iv;
            cmp->b = hi;
            cmp->binop = (int)TOK_LT;
            term_br(c, body, exit);

            CUR = body;
            sdepth++;
            scope_push(n->as.for_stmt.var, iv);
            loop_push(latch, exit, F->nlocals);
            lower_stmt(n->as.for_stmt.body);
            if (nloop > 0) nloop--;
            scope_pop_to(sdepth - 1);
            sdepth--;
            term_jmp(latch);

            CUR = latch;
            int one = new_local(ty_int(), NULL, 0);
            IrInst *k = emit(IR_CONST_INT, n->loc);
            k->dst = one;
            k->imm = 1;
            k->ty = ty_int();
            int nx = new_local(ty_int(), NULL, 0);
            IrInst *add = emit(IR_BIN, n->loc);
            add->dst = nx;
            add->a = iv;
            add->b = one;
            add->binop = (int)TOK_PLUS;
            add->checked = 1;
            IrInst *st = emit(IR_MOVE, n->loc);
            st->dst = iv;
            st->a = nx;
            term_jmp(head);

            CUR = exit;
            return;
        }
        case AST_WHILE: {
            int head = new_block(), body = new_block(), exit = new_block();
            term_jmp(head);
            CUR = head;
            int c = lower_expr(n->as.if_stmt.cond);
            term_br(c, body, exit);
            CUR = body;
            loop_push(head, exit, F->nlocals);
            lower_stmt(n->as.if_stmt.then_block);
            if (nloop > 0) nloop--;
            term_jmp(head);
            CUR = exit;
            return;
        }
        case AST_BREAK:
            if (nloop <= 0) {
                F->lowered = 0;
                return;
            }
            emit_drops_from(loop_base[nloop - 1], n->loc);
            term_jmp(loop_brk[nloop - 1]);
            return;
        case AST_CONTINUE:
            if (nloop <= 0) {
                F->lowered = 0;
                return;
            }
            emit_drops_from(loop_base[nloop - 1], n->loc);
            term_jmp(loop_cont[nloop - 1]);
            return;
        case AST_MATCH: {
            int scrut = lower_expr(n->as.match_stmt.scrut);
            int join = new_block();
            Type *st = n->as.match_stmt.scrut ? n->as.match_stmt.scrut->ty : NULL;
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++) {
                AstNode *arm = n->as.match_stmt.arms[i];
                if (!arm || arm->kind != AST_MATCH_ARM) continue;
                int body = new_block();
                int next = new_block();
                if (arm->as.match_arm.is_wild) {
                    term_jmp(body);
                } else {
                    int acc = -1;
                    for (size_t p = 0; p < arm->as.match_arm.pat_count; p++) {
                        int pv = lower_expr(arm->as.match_arm.pats[p]);
                        int eq = new_local(ty_bool(), NULL, 0);
                        IrInst *cmp = emit(IR_BIN, arm->loc);
                        cmp->dst = eq;
                        cmp->a = scrut;
                        cmp->b = pv;
                        cmp->binop = (int)TOK_EQ_EQ;
                        cmp->ty = ty_bool();
                        if (st && st->kind == TY_STRING) cmp->checked = 0;
                        if (acc < 0) acc = eq;
                        else {
                            int o = new_local(ty_bool(), NULL, 0);
                            IrInst *orv = emit(IR_BIN, arm->loc);
                            orv->dst = o;
                            orv->a = acc;
                            orv->b = eq;
                            orv->binop = (int)TOK_PIPE_PIPE;
                            orv->ty = ty_bool();
                            acc = o;
                        }
                    }
                    if (acc >= 0) term_br(acc, body, next);
                    else term_jmp(next);
                }
                CUR = body;
                lower_stmt(arm->as.match_arm.body);
                term_jmp(join);
                CUR = next;
            }
            term_jmp(join);
            CUR = join;
            return;
        }
        default:
            F->lowered = 0;
            return;
    }
}

/* --- driver ------------------------------------------------------------- */

/* Every reactive prop is a thunk, so a real app has thousands of closures;
   this list grows rather than capping. */
static AstNode **clos_nodes;
static int *clos_mods;
static int nclos_nodes;
static int clos_cap;

static void collect_clos(AstNode *n) {
    if (!n) return;
    if (n->kind == AST_CLOSURE) {
        if (nclos_nodes >= clos_cap) {
            clos_cap = clos_cap ? clos_cap * 2 : 256;
            clos_nodes = (AstNode **)realloc(clos_nodes, (size_t)clos_cap * sizeof(AstNode *));
            clos_mods = (int *)realloc(clos_mods, (size_t)clos_cap * sizeof(int));
            if (!clos_nodes || !clos_mods) yuga_fatal("out of memory");
        }
        clos_mods[nclos_nodes] = Gcur;
        clos_nodes[nclos_nodes++] = n;
        collect_clos(n->as.fn.body);
        return;
    }
    switch (n->kind) {
        case AST_PROGRAM:
            for (size_t i = 0; i < n->as.program.decl_count; i++)
                collect_clos(n->as.program.decls[i]);
            break;
        case AST_FN_DECL:
            /* Closures inside a generic template are lowered per instance
               (with that instance's type subst), not from the template. */
            if (!n->as.fn.tparam_count) collect_clos(n->as.fn.body);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                collect_clos(n->as.block.stmts[i]);
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
            for (size_t i = 0; i < n->as.call.arg_count; i++)
                collect_clos(n->as.call.args[i]);
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
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                collect_clos(n->as.array_lit.elems[i]);
            break;
        default:
            break;
    }
}

static void bind_module_globals(void) {
    for (int g = 0; g < typecheck_global_count(); g++) {
        if (typecheck_global_mod(g) != Gcur) continue;
        AstNode *gv = typecheck_global_var(g);
        const char *cn = typecheck_global_cname(g);
        if (!gv || !gv->as.var.name) continue;
        int id = new_local(gv->ty, gv->as.var.name, 0);
        F->locals[id].is_global = 1;
        F->locals[id].cname = cn;
        F->locals[id].needs_drop = 0;
        scope_push(gv->as.var.name, id);
    }
}

/** IR local for a module `let`, including ones imported as `mod.name`. */
static int local_for_global(AstNode *gv) {
    if (!gv) return -1;
    const char *cn = NULL;
    Type *ty = gv->ty;
    for (int g = 0; g < typecheck_global_count(); g++) {
        if (typecheck_global_var(g) != gv) continue;
        cn = typecheck_global_cname(g);
        break;
    }
    if (!cn) return -1;
    for (int i = 0; i < F->nlocals; i++) {
        if (F->locals[i].is_global && F->locals[i].cname &&
            strcmp(F->locals[i].cname, cn) == 0)
            return i;
    }
    int id = new_local(ty, gv->as.var.name, 0);
    F->locals[id].is_global = 1;
    F->locals[id].cname = cn;
    F->locals[id].needs_drop = 0;
    return id;
}

static void lower_closure(IrModule *m, AstNode *d) {
    if (m->nfns >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 32;
        m->fns = (IrFn *)realloc(m->fns, (size_t)m->cap * sizeof(IrFn));
    }
    F = &m->fns[m->nfns++];
    memset(F, 0, sizeof(*F));
    {
        char buf[256];
        if (cname_override)
            snprintf(buf, sizeof buf, "yuga_clos_%d_%s", d->as.fn.clos_id, cname_override);
        else
            snprintf(buf, sizeof buf, "yuga_clos_%d", d->as.fn.clos_id);
        F->cname = yuga_dup(buf);
    }
    F->name = d->as.fn.name;
    F->sig = ir_subst(d->ty);
    F->lowered = 1;
    F->clos_id = d->as.fn.clos_id;
    F->env_local = -1;
    F->caps = d->as.fn.caps;
    F->ncaps = (int)d->as.fn.cap_count;
    if (F->ncaps) {
        Type **ct = (Type **)calloc((size_t)F->ncaps, sizeof(Type *));
        for (int k = 0; k < F->ncaps; k++)
            ct[k] = ir_subst(d->as.fn.cap_types ? d->as.fn.cap_types[k] : NULL);
        F->cap_types = ct;
    } else {
        F->cap_types = d->as.fn.cap_types;
    }

    scopes = NULL;
    sdepth = 0;
    nloop = 0;
    CUR = new_block();
    /* `_env` is allocated first so it stays a known local even when the
       module has globals; codegen casts it to the env struct. */
    int env = new_local(type_ptr(ty_void(), 1), "_env", 1);
    F->env_local = env;
    bind_module_globals();
    Type *ft = d->ty;
    for (size_t k = 0; k < d->as.fn.param_count; k++) {
        Type *pt = (ft && k < ft->param_count) ? ft->params[k] : NULL;
        int id = new_local(pt, d->as.fn.params[k].name, 1);
        scope_push(d->as.fn.params[k].name, id);
    }
    F->nparams = 1 + (int)d->as.fn.param_count;

    for (int k = 0; k < F->ncaps; k++) {
        Type *ct = ir_subst(d->as.fn.cap_types[k]);
        int id = new_local(ct, d->as.fn.caps[k], 0);
        scope_push(d->as.fn.caps[k], id);
        IrPlace *base = place_local(env, F->locals[env].ty);
        IrPlace *fp = (IrPlace *)calloc(1, sizeof(IrPlace));
        fp->kind = IR_PL_FIELD;
        fp->field = d->as.fn.caps[k];
        fp->base = base;
        fp->ty = ct;
        IrInst *ld = emit(IR_LOAD, d->loc);
        ld->dst = id;
        ld->place = fp;
        ld->ty = ct;
        /* Caps live in the env; the local is an alias. */
        F->locals[id].needs_drop = 0;
    }

    lower_stmt(d->as.fn.body);
    if (!block_closed()) term_ret(-1, d->loc);
    emit_drops_at_exits();
    scope_pop_to(-1);
}

/** Lower closures nested in a generic instance, with the instance's subst. */
static void lower_clos_in(IrModule *m, AstNode *n) {
    if (!n) return;
    if (n->kind == AST_CLOSURE) {
        lower_closure(m, n);
        lower_clos_in(m, n->as.fn.body);
        return;
    }
    switch (n->kind) {
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                lower_clos_in(m, n->as.block.stmts[i]);
            break;
        case AST_IF:
            lower_clos_in(m, n->as.if_stmt.cond);
            lower_clos_in(m, n->as.if_stmt.then_block);
            lower_clos_in(m, n->as.if_stmt.else_block);
            break;
        case AST_FOR:
            lower_clos_in(m, n->as.for_stmt.iter);
            lower_clos_in(m, n->as.for_stmt.body);
            break;
        case AST_WHILE:
            lower_clos_in(m, n->as.if_stmt.cond);
            lower_clos_in(m, n->as.if_stmt.then_block);
            break;
        case AST_MATCH:
            lower_clos_in(m, n->as.match_stmt.scrut);
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++)
                lower_clos_in(m, n->as.match_stmt.arms[i]);
            break;
        case AST_MATCH_ARM:
            lower_clos_in(m, n->as.match_arm.body);
            break;
        case AST_RETURN:
            lower_clos_in(m, n->as.ret.expr);
            break;
        case AST_EXPR_STMT:
            lower_clos_in(m, n->as.expr_stmt.expr);
            break;
        case AST_VAR_DECL:
            lower_clos_in(m, n->as.var.init);
            break;
        case AST_ASSIGN:
            lower_clos_in(m, n->as.assign.left);
            lower_clos_in(m, n->as.assign.right);
            break;
        case AST_BINARY:
            lower_clos_in(m, n->as.binary.left);
            lower_clos_in(m, n->as.binary.right);
            break;
        case AST_UNARY:
            lower_clos_in(m, n->as.unary.operand);
            break;
        case AST_INCDEC:
            lower_clos_in(m, n->as.incdec.operand);
            break;
        case AST_CAST:
            lower_clos_in(m, n->as.cast.expr);
            break;
        case AST_CALL:
            lower_clos_in(m, n->as.call.callee);
            for (size_t i = 0; i < n->as.call.arg_count; i++)
                lower_clos_in(m, n->as.call.args[i]);
            break;
        case AST_INDEX:
        case AST_FIELD:
        case AST_DEREF:
        case AST_ADDR:
            lower_clos_in(m, n->as.access.target);
            lower_clos_in(m, n->as.access.index);
            break;
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++)
                lower_clos_in(m, n->as.struct_lit.fields[i].init);
            break;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                lower_clos_in(m, n->as.array_lit.elems[i]);
            break;
        default:
            break;
    }
}

static void lower_fn(IrModule *m, AstNode *d) {
    if (m->nfns >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 32;
        m->fns = (IrFn *)realloc(m->fns, (size_t)m->cap * sizeof(IrFn));
    }
    F = &m->fns[m->nfns++];
    memset(F, 0, sizeof(*F));
    F->cname = cname_override ? cname_override : d->as.fn.cname;
    F->name = d->as.fn.name;
    F->sig = ir_subst(d->ty);
    F->lowered = 1;
    F->env_local = -1;
    F->is_main = Gcur == 0 && !cname_override && d->as.fn.name && strcmp(d->as.fn.name, "main") == 0;

    scopes = NULL;
    sdepth = 0;
    nloop = 0;
    CUR = new_block();
    bind_module_globals();

    Type *ft = d->ty;
    for (size_t k = 0; k < d->as.fn.param_count; k++) {
        Type *pt = (ft && k < ft->param_count) ? ft->params[k] : NULL;
        int id = new_local(pt, d->as.fn.params[k].name, 1);
        scope_push(d->as.fn.params[k].name, id);
    }
    F->nparams = (int)d->as.fn.param_count;

    lower_stmt(d->as.fn.body);
    if (!block_closed()) term_ret(-1, d->loc);
    emit_drops_at_exits();
    scope_pop_to(-1);
}

IrModule *ir_lower(YugaModule *mods, int nmods) {
    IrModule *m = (IrModule *)calloc(1, sizeof(IrModule));
    Gmods = mods;
    Gnmods = nmods;
    nclos_nodes = 0;
    for (int i = 0; i < nmods; i++) {
        Gcur = i;
        collect_clos(mods[i].ast);
    }
    /* A closure's module globals are its own, not the entry module's. */
    for (int i = 0; i < nclos_nodes; i++) {
        Gcur = clos_mods[i];
        lower_closure(m, clos_nodes[i]);
    }
    for (int i = 0; i < nmods; i++) {
        AstNode *p = mods[i].ast;
        if (!p) continue;
        Gcur = i;
        for (size_t k = 0; k < p->as.program.decl_count; k++) {
            AstNode *d = p->as.program.decls[k];
            if (d->kind != AST_FN_DECL) continue;
            if (d->as.fn.is_intrinsic || d->as.fn.tparam_count) continue;
            lower_fn(m, d);
        }
    }
    for (int mi = 0; mi < nmods; mi++) {
        AstNode *p = mods[mi].ast;
        if (!p) continue;
        int any = 0;
        for (size_t k = 0; k < p->as.program.decl_count; k++)
            if (p->as.program.decls[k]->kind == AST_VAR_DECL) any = 1;
        if (!any) continue;
        if (m->nfns >= m->cap) {
            m->cap = m->cap ? m->cap * 2 : 32;
            m->fns = (IrFn *)realloc(m->fns, (size_t)m->cap * sizeof(IrFn));
        }
        F = &m->fns[m->nfns++];
        memset(F, 0, sizeof(*F));
        {
            char buf[256];
            if (mi == 0)
                snprintf(buf, sizeof buf, "yuga__init");
            else
                snprintf(buf, sizeof buf, "yuga_%s__init", mods[mi].name);
            F->cname = yuga_dup(buf);
        }
        F->name = "__init";
        F->lowered = 1;
        Gcur = mi;
        scopes = NULL;
        sdepth = 0;
        nloop = 0;
        CUR = new_block();
        bind_module_globals();
        F->nparams = 0;
        for (size_t k = 0; k < p->as.program.decl_count; k++) {
            AstNode *d = p->as.program.decls[k];
            if (d->kind != AST_VAR_DECL) continue;
            int gid = scope_find(d->as.var.name);
            int v = d->as.var.init ? lower_expr(d->as.var.init) : -1;
            if (v >= 0 && gid >= 0) {
                IrInst *mv = emit(IR_MOVE, d->loc);
                mv->dst = gid;
                mv->a = v;
                mv->ty = ir_subst(d->ty);
            }
        }
        if (!block_closed()) term_ret(-1, p->loc);
        scope_pop_to(-1);
    }
    for (int i = 0; i < typecheck_mono_count(); i++) {
        AstNode *fn = typecheck_mono_fn(i);
        if (!fn) continue;
        subst_names = fn->as.fn.tparams;
        subst_args = typecheck_mono_args(i);
        subst_n = typecheck_mono_nargs(i);
        cname_override = typecheck_mono_cname(i);
        lower_fn(m, fn);
        lower_clos_in(m, fn->as.fn.body);
        subst_names = NULL;
        subst_args = NULL;
        subst_n = 0;
        cname_override = NULL;
    }
    return m;
}

/* --- printing ----------------------------------------------------------- */

static const char *op_name(int op) {
    switch ((TokenKind)op) {
        case TOK_PLUS: return "add";
        case TOK_MINUS: return "sub";
        case TOK_STAR: return "mul";
        case TOK_SLASH: return "div";
        case TOK_PERCENT: return "rem";
        case TOK_EQ_EQ: return "eq";
        case TOK_BANG_EQ: return "ne";
        case TOK_LT: return "lt";
        case TOK_GT: return "gt";
        case TOK_LT_EQ: return "le";
        case TOK_GT_EQ: return "ge";
        case TOK_BANG: return "not";
        default: return "?";
    }
}

static void print_place(FILE *o, const IrPlace *p) {
    if (!p) {
        fprintf(o, "<null>");
        return;
    }
    switch (p->kind) {
        case IR_PL_LOCAL: fprintf(o, "%%%d", p->local); break;
        case IR_PL_FIELD:
            print_place(o, p->base);
            fprintf(o, ".%s", p->field ? p->field : "?");
            break;
        case IR_PL_INDEX:
            print_place(o, p->base);
            fprintf(o, "[%%%d]", p->index);
            break;
        case IR_PL_DEREF:
            fprintf(o, "*");
            print_place(o, p->base);
            break;
    }
}

static void print_inst(FILE *o, const IrInst *i) {
    fprintf(o, "    ");
    if (i->dst >= 0) fprintf(o, "%%%d = ", i->dst);
    switch (i->op) {
        case IR_CONST_INT: fprintf(o, "const %lld", (long long)i->imm); break;
        case IR_CONST_FLOAT: fprintf(o, "const.f %g", i->fimm); break;
        case IR_CONST_BOOL: fprintf(o, "const %s", i->imm ? "true" : "false"); break;
        case IR_CONST_STR: fprintf(o, "str \"%s\"", i->str ? i->str : ""); break;
        case IR_LOAD:
            fprintf(o, "load ");
            print_place(o, i->place);
            break;
        case IR_STORE:
            fprintf(o, "store");
            if (i->binop == TOK_PLUS_EQ) fprintf(o, ".add");
            else if (i->binop == TOK_MINUS_EQ) fprintf(o, ".sub");
            else if (i->binop == TOK_STAR_EQ) fprintf(o, ".mul");
            else if (i->binop == TOK_SLASH_EQ) fprintf(o, ".div");
            fprintf(o, " ");
            print_place(o, i->place);
            fprintf(o, ", %%%d", i->a);
            break;
        case IR_ADDR:
            fprintf(o, "addr%s ", i->is_mut ? " mut" : "");
            print_place(o, i->place);
            break;
        case IR_MOVE: fprintf(o, "move %%%d", i->a); break;
        case IR_BIN:
            fprintf(o, "%s%s %%%d, %%%d", op_name(i->binop), i->checked ? ".chk" : "",
                    i->a, i->b);
            break;
        case IR_UN: fprintf(o, "%s %%%d", op_name(i->binop), i->a); break;
        case IR_CALL:
        case IR_CALL_VAL:
            if (i->op == IR_CALL)
                fprintf(o, "call %s(", i->callee ? i->callee : "?");
            else
                fprintf(o, "call.val %%%d(", i->a);
            for (int k = 0; k < i->nargs; k++)
                fprintf(o, "%s%%%d", k ? ", " : "", i->args[k]);
            fprintf(o, ")");
            break;
        case IR_ALLOC: fprintf(o, "alloc %%%d", i->a); break;
        case IR_DROP: fprintf(o, "drop %%%d", i->a); break;
        case IR_BOUND:
            if (i->b >= 0)
                fprintf(o, "bound %%%d, %%%d", i->a, i->b);
            else
                fprintf(o, "bound %%%d, %lld", i->a, (long long)i->imm);
            break;
        case IR_STRUCT_LIT:
        case IR_ARRAY_LIT:
            fprintf(o, "%s {", i->op == IR_STRUCT_LIT ? "struct" : "array");
            for (int k = 0; k < i->nargs; k++)
                fprintf(o, "%s%%%d", k ? ", " : "", i->args[k]);
            fprintf(o, "}");
            break;
        case IR_FN_VAL:
            fprintf(o, "fnval %s", i->callee ? i->callee : "?");
            break;
        case IR_CLOS:
            fprintf(o, "clos %s {", i->callee ? i->callee : "?");
            for (int k = 0; k < i->nargs; k++)
                fprintf(o, "%s%%%d", k ? ", " : "", i->args[k]);
            fprintf(o, "}");
            break;
        case IR_CAST:
            fprintf(o, "cast %%%d", i->a);
            break;
        case IR_SIZEOF:
            fprintf(o, "sizeof %s", i->ty ? type_name(i->ty) : "?");
            break;
    }
    fprintf(o, "\n");
}

void ir_print(FILE *o, const IrModule *m) {
    for (int f = 0; f < m->nfns; f++) {
        const IrFn *fn = &m->fns[f];
        fprintf(o, "fn %s  ; %d locals, %d blocks%s\n", fn->cname ? fn->cname : fn->name,
                fn->nlocals, fn->nblocks, fn->lowered ? "" : "  [PARTIAL]");
        for (int l = 0; l < fn->nlocals; l++)
            fprintf(o, "  %%%d%s%s%s\n", fn->locals[l].id,
                    fn->locals[l].name ? " " : "",
                    fn->locals[l].name ? fn->locals[l].name : "",
                    fn->locals[l].needs_drop ? " [drop]" : "");
        for (int b = 0; b < fn->nblocks; b++) {
            const IrBlock *bb = &fn->blocks[b];
            fprintf(o, "  bb%d:\n", bb->id);
            for (int i = 0; i < bb->ninsts; i++) print_inst(o, &bb->insts[i]);
            switch (bb->term) {
                case IR_TERM_RET:
                    if (bb->term_val >= 0)
                        fprintf(o, "    ret %%%d\n", bb->term_val);
                    else
                        fprintf(o, "    ret\n");
                    break;
                case IR_TERM_JMP: fprintf(o, "    jmp bb%d\n", bb->succ[0]); break;
                case IR_TERM_BR:
                    fprintf(o, "    br %%%d, bb%d, bb%d\n", bb->term_val, bb->succ[0],
                            bb->succ[1]);
                    break;
                case IR_TERM_UNREACHABLE: fprintf(o, "    unreachable\n"); break;
            }
        }
        fprintf(o, "\n");
    }
}

/* --- verification ------------------------------------------------------- */

/**
 * Operand-level checks apply only to functions that claim full lowering. A
 * partial function already carries an undefined operand by construction —
 * reporting that as a verifier failure would drown the real errors it exists
 * to catch.
 */
static int verify_fn(const IrFn *fn) {
    int bad = 0;
    if (!fn->lowered) return 0;
    for (int b = 0; b < fn->nblocks; b++) {
        const IrBlock *bb = &fn->blocks[b];
        if (bb->term == IR_TERM_UNREACHABLE) {
            fprintf(stderr, "ir: %s bb%d has no terminator\n", fn->cname, bb->id);
            bad = 1;
        }
        for (int s = 0; s < 2; s++) {
            int t = bb->succ[s];
            if (t >= 0 && (t >= fn->nblocks)) {
                fprintf(stderr, "ir: %s bb%d successor %d out of range\n", fn->cname,
                        bb->id, t);
                bad = 1;
            }
        }
        for (int i = 0; i < bb->ninsts; i++) {
            const IrInst *in = &bb->insts[i];
            if (in->dst >= fn->nlocals || in->a >= fn->nlocals || in->b >= fn->nlocals) {
                fprintf(stderr, "ir: %s bb%d inst %d references unknown local\n",
                        fn->cname, bb->id, i);
                bad = 1;
            }
            for (int k = 0; k < in->nargs; k++)
                if (in->args[k] >= fn->nlocals || in->args[k] < 0) {
                    fprintf(stderr, "ir: %s bb%d inst %d bad arg %d\n", fn->cname, bb->id,
                            i, k);
                    bad = 1;
                }
        }
    }
    return bad;
}

int ir_verify(const IrModule *m) {
    int bad = 0;
    for (int f = 0; f < m->nfns; f++) bad |= verify_fn(&m->fns[f]);
    return bad;
}

static void free_place(IrPlace *p) {
    if (!p) return;
    free_place(p->base);
    free(p);
}

void ir_free(IrModule *m) {
    if (!m) return;
    for (int f = 0; f < m->nfns; f++) {
        IrFn *fn = &m->fns[f];
        for (int b = 0; b < fn->nblocks; b++) {
            for (int i = 0; i < fn->blocks[b].ninsts; i++) {
                free_place(fn->blocks[b].insts[i].place);
                free(fn->blocks[b].insts[i].args);
            }
            free(fn->blocks[b].insts);
        }
        free(fn->blocks);
        free(fn->locals);
    }
    free(m->fns);
    free(m);
}
