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
static LoamModule *Gmods;
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

static const char *fn_cname_in(LoamModule *m, const char *name) {
    AstNode *p = m ? m->ast : NULL;
    if (!p) return NULL;
    for (size_t i = 0; i < p->as.program.decl_count; i++) {
        AstNode *d = p->as.program.decls[i];
        if (d->kind == AST_FN_DECL && d->as.fn.name && strcmp(d->as.fn.name, name) == 0)
            return d->as.fn.cname;
    }
    return NULL;
}

/** Module named `name`, preferring the one `cur` imported under that alias.
    Mirrors typecheck's find_mod so the IR walks the same module graph. */
static LoamModule *find_mod_here(LoamModule *cur, const char *name) {
    AstNode *prog = cur ? cur->ast : NULL;
    if (prog) {
        for (size_t i = 0; i < prog->as.program.import_count; i++) {
            AstNode *im = prog->as.program.imports[i];
            if (!im || !im->as.import.alias || !im->as.import.resolved) continue;
            if (strcmp(im->as.import.alias, name) != 0) continue;
            for (int m = 0; m < Gnmods; m++)
                if (Gmods[m].path && strcmp(Gmods[m].path, im->as.import.resolved) == 0)
                    return &Gmods[m];
        }
    }
    for (int i = 0; i < Gnmods; i++)
        if (Gmods[i].name && strcmp(Gmods[i].name, name) == 0) return &Gmods[i];
    return NULL;
}

/** C symbol for `name` as re-exported by `root`: root's own decls, then root's
    direct imports (reverse source order), then the imports of those imports.
    The same walk as typecheck's lookup_through, so a barrel module like `zeus`
    yields the C name of a fn declared in `zeusbase`. Without this the IR fell
    back to the bare fn name and the C backend emitted an undeclared symbol. */
static const char *fn_cname_through(LoamModule *root, const char *name) {
    if (!root || !root->ast || !name) return NULL;
    const char *c = fn_cname_in(root, name);
    if (c) return c;
    size_t i = root->ast->as.program.import_count;
    while (i > 0) {
        i--;
        AstNode *im = root->ast->as.program.imports[i];
        if (!im || !im->as.import.alias) continue;
        LoamModule *m = find_mod_here(root, im->as.import.alias);
        c = m ? fn_cname_in(m, name) : NULL;
        if (c) return c;
    }
    i = root->ast->as.program.import_count;
    while (i > 0) {
        i--;
        AstNode *im = root->ast->as.program.imports[i];
        if (!im || !im->as.import.alias) continue;
        LoamModule *m = find_mod_here(root, im->as.import.alias);
        if (!m || !m->ast) continue;
        size_t k = m->ast->as.program.import_count;
        while (k > 0) {
            k--;
            AstNode *im2 = m->ast->as.program.imports[k];
            if (!im2 || !im2->as.import.alias) continue;
            LoamModule *m2 = find_mod_here(m, im2->as.import.alias);
            c = m2 ? fn_cname_in(m2, name) : NULL;
            if (c) return c;
        }
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
        const char *c = fn_cname_through(&Gmods[Gcur], cal->as.ident.name);
        if (c) return c;
        for (int i = 0; i < Gnmods; i++) {
            c = fn_cname_through(&Gmods[i], cal->as.ident.name);
            if (c) return c;
        }
        return cal->as.ident.name;
    }
    if (cal->kind == AST_FIELD && cal->as.access.target &&
        cal->as.access.target->kind == AST_IDENT) {
        /* A module keeps the alias it first loaded under, so this file's alias
           has to go through this file's imports. */
        LoamModule *m = find_mod_here(&Gmods[Gcur], cal->as.access.target->as.ident.name);
        const char *c = m ? fn_cname_through(m, cal->as.access.field) : NULL;
        return c ? c : cal->as.access.field;
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
    /* Under string ownership a parameter is a borrowed reference: the caller
       owns the value and releases it, so the callee must not drop the param. A
       param that is returned takes its own reference instead (see the return
       path in codegen). */
    if (is_param && type_string_owns() && l->ty && l->ty->kind == TY_STRING)
        l->needs_drop = 0;
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
static int *loop_is_switch; /* a switch takes `break`; `continue` goes past it */
static int nloop, loop_cap;

static void loop_push(int cont, int brk, int base) {
    if (nloop == loop_cap) {
        loop_cap = loop_cap ? loop_cap * 2 : 16;
        loop_cont = (int *)realloc(loop_cont, (size_t)loop_cap * sizeof(int));
        loop_brk = (int *)realloc(loop_brk, (size_t)loop_cap * sizeof(int));
        loop_base = (int *)realloc(loop_base, (size_t)loop_cap * sizeof(int));
        loop_is_switch = (int *)realloc(loop_is_switch, (size_t)loop_cap * sizeof(int));
        if (!loop_cont || !loop_brk || !loop_base || !loop_is_switch)
            loam_fatal("out of memory");
    }
    loop_cont[nloop] = cont;
    loop_brk[nloop] = brk;
    loop_base[nloop] = base;
    loop_is_switch[nloop] = 0;
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

/** Dispatch on `val`. Takes ownership of `vals` and `blocks`. */
static void term_switch(int val, int64_t *vals, int *blocks, int n, int dflt) {
    IrBlock *b = &F->blocks[CUR];
    if (b->term != IR_TERM_UNREACHABLE) return;
    b->term = IR_TERM_SWITCH;
    b->term_val = val;
    b->sw_vals = vals;
    b->sw_blocks = blocks;
    b->sw_n = n;
    b->succ[0] = dflt;
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
            /* A global reached through an import is not bound in this
               function's scope (only this module's globals are), but typecheck
               resolved the name to its declaration: reference the static C
               symbol directly. */
            if (id < 0 && n->as.ident.resolved &&
                n->as.ident.resolved->kind == AST_VAR_DECL)
                id = local_for_global(n->as.ident.resolved);
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

static int state_sig_local(AstNode *n) {
    if (!n || n->kind != AST_IDENT || !n->as.ident.name) return -1;
    return scope_find(n->as.ident.name);
}

static int emit_state_get(int sid, SourceLoc loc) {
    int *args = (int *)calloc(1, sizeof(int));
    args[0] = sid;
    return emit_named_call("loam_zeus_get", args, 1, ty_int(), loc);
}

static void emit_state_set(int sid, int val, SourceLoc loc) {
    int *args = (int *)calloc(2, sizeof(int));
    args[0] = sid;
    args[1] = val;
    emit_named_call("loam_zeus_set", args, 2, ty_void(), loc);
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
    Variadic mixed types are not a Loam function; this is the language's
    compile-time expansion, not a C-backend special case. */
static int lower_println(AstNode *n) {
    /* is_println == 2 is `fmt.eprintln`: the same expansion, to stderr. */
    int to_err = n->as.call.is_println == 2;
    const char *w_str = to_err ? "loam_fmt_ewrite" : "loam_fmt_write";
    const char *w_bool = to_err ? "loam_fmt_ewrite_bool" : "loam_fmt_write_bool";
    const char *w_float = to_err ? "loam_fmt_ewrite_float" : "loam_fmt_write_float";
    const char *w_u64 = to_err ? "loam_fmt_ewrite_u64" : "loam_fmt_write_u64";
    const char *w_int = to_err ? "loam_fmt_ewrite_int" : "loam_fmt_write_int";
    for (size_t k = 0; k < n->as.call.arg_count; k++) {
        if (k) {
            int sp = new_local(ty_string(), NULL, 0);
            IrInst *c = emit(IR_CONST_STR, n->loc);
            c->dst = sp;
            c->str = " ";
            c->ty = ty_string();
            int *a = (int *)calloc(1, sizeof(int));
            a[0] = sp;
            emit_named_call(w_str, a, 1, ty_void(), n->loc);
        }
        AstNode *arg = n->as.call.args[k];
        int v = lower_expr(arg);
        int *a = (int *)calloc(1, sizeof(int));
        a[0] = v;
        Type *t = ir_subst(arg->ty);
        if (t && t->kind == TY_STRING)
            emit_named_call(w_str, a, 1, ty_void(), n->loc);
        else if (t && t->kind == TY_BOOL)
            emit_named_call(w_bool, a, 1, ty_void(), n->loc);
        else if (t && t->kind == TY_FLOAT)
            emit_named_call(w_float, a, 1, ty_void(), n->loc);
        else if (t && t->kind == TY_INT && t->is_unsigned && t->bits == 64)
            emit_named_call(w_u64, a, 1, ty_void(), n->loc);
        else
            emit_named_call(w_int, a, 1, ty_void(), n->loc);
    }
    emit_named_call(to_err ? "loam_fmt_ewriteln" : "loam_fmt_writeln", NULL, 0, ty_void(), n->loc);
    return -1;
}

/** A pushed element of type `t` is moved into the slot (its local no longer
 *  drops) when it holds a `Box`: the push retains refcounted leaves but takes
 *  a Box as-is, so dropping the local after would free the slot's Box. */
static int push_moves(const Type *t) {
    if (!t) return 0;
    if (t->kind == TY_BOX) return 1;
    if (t->kind == TY_ARRAY) return push_moves(t->elem);
    if (t->kind == TY_STRUCT)
        for (size_t i = 0; i < t->field_count; i++)
            if (push_moves(t->field_types[i])) return 1;
    return 0;
}

/** A borrow argument whose place goes through an index (`&mut v[i].kids`,
 *  `push(NODES[n].kids, …)`): its address is into a vec's buffer, which a
 *  later argument can reallocate (`push(NODES[n].kids, new_node())`). Such an
 *  argument is lowered after the others, right before the call, so the
 *  address is taken once nothing else can move the buffer. */
static int addr_through_index(const AstNode *a) {
    if (!a || a->kind != AST_ADDR) return 0;
    for (const AstNode *place = a->as.access.target; place;) {
        if (place->kind == AST_INDEX) return 1;
        if (place->kind != AST_FIELD) return 0;
        place = place->as.access.target;
    }
    return 0;
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
    if (n->as.call.value_key && n->as.call.arg_count == 1) {
        int arg = lower_expr(n->as.call.args[0]);
        int *args = (int *)calloc(1, sizeof(int));
        args[0] = arg;
        IrInst *i = emit(IR_CALL, n->loc);
        i->dst = dst;
        i->args = args;
        i->nargs = 1;
        /* ty is the value's type; codegen walks it. The dest local is a string. */
        i->ty = ir_subst(n->as.call.args[0]->ty);
        i->callee = "loam_value_key";
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
        if (n->as.call.sig_cell == 1) i->callee = "loam_sig_push";
        else if (n->as.call.sig_cell == 2) i->callee = "loam_sig_load";
        else i->callee = "loam_sig_store";
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
        if (n->as.call.fut_cell == 1) i->callee = "loam_fut_push";
        else if (n->as.call.fut_cell == 2) i->callee = "loam_fut_load";
        else if (n->as.call.fut_cell == 3) i->callee = "loam_fut_store";
        else if (n->as.call.fut_cell == 4) i->callee = "loam_fut_ready";
        else i->callee = "loam_fut_clear";
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
        if (n->as.call.ch_cell == 1) i->callee = "loam_ch_alloc";
        else if (n->as.call.ch_cell == 2) i->callee = "loam_ch_send";
        else if (n->as.call.ch_cell == 3) i->callee = "loam_ch_try_send";
        else if (n->as.call.ch_cell == 4) i->callee = "loam_ch_recv";
        else if (n->as.call.ch_cell == 5) i->callee = "loam_ch_pop";
        else i->callee = "loam_ch_ready";
        return no_dst ? -1 : dst;
    }
    if (n->as.call.is_println) return lower_println(n);
    if (n->as.call.is_vec_push) {
        int *args = (int *)calloc(2, sizeof(int));
        int vec_addr_last = n->as.call.arg_count > 1 && addr_through_index(n->as.call.args[0]);
        if (!vec_addr_last) args[0] = n->as.call.arg_count > 0 ? lower_expr(n->as.call.args[0]) : -1;
        args[1] = n->as.call.arg_count > 1 ? lower_expr(n->as.call.args[1]) : -1;
        if (vec_addr_last) args[0] = lower_expr(n->as.call.args[0]);
        IrInst *i = emit(IR_CALL, n->loc);
        i->callee = "loam_vec_push";
        i->args = args;
        i->nargs = 2;
        i->ty = n->as.call.arg_count > 1 && n->as.call.args[1]
                    ? ir_subst(n->as.call.args[1]->ty)
                    : ty_int();
        /* `loam_vec_push_owned` retains every refcounted leaf (`[]T`, an owned
           string) for the slot, so an element made of those keeps its own
           reference and still drops: `push(rows, row_of(i))` used to leak the
           row. A `Box` leaf is not retained, only moved in, so an element
           holding one is handed over whole. */
        if (args[1] >= 0 && args[1] < F->nlocals && type_needs_drop(F->locals[args[1]].ty) &&
            push_moves(F->locals[args[1]].ty))
            F->locals[args[1]].needs_drop = 0;
        return -1;
    }
    if (n->as.call.is_vec_pop) {
        int *args = (int *)calloc(1, sizeof(int));
        args[0] = n->as.call.arg_count > 0 ? lower_expr(n->as.call.args[0]) : -1;
        IrInst *i = emit(IR_CALL, n->loc);
        i->dst = dst;
        i->callee = "loam_vec_pop";
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
        i->callee = loam_dup(numeric_builtin_cname(n->as.call.num_builtin, n->ty));
        return dst;
    }
    if (n->as.call.is_sizeof) {
        IrInst *i = emit(IR_SIZEOF, n->loc);
        i->dst = dst;
        i->a = -1;
        i->ty = n->as.call.arg_count ? ir_subst(n->as.call.args[0]->ty) : ty_void();
        return dst;
    }
    if (n->as.call.is_panic) {
        /* Lower the message first: lowering can emit instructions and realloc
           the block, which would dangle the pointer emit() returns. */
        int msg = n->as.call.arg_count ? lower_expr(n->as.call.args[0]) : -1;
        IrInst *i = emit(IR_PANIC, n->loc);
        i->dst = -1;
        i->a = msg;
        i->ty = ty_void();
        return -1;
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
            if (a >= 0 && a < F->nlocals && type_arg_transfers(F->locals[a].ty))
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
    int bind_state = callee && strcmp(callee, "loam_zeus_bind_n") == 0 &&
                     n->as.call.arg_count >= 2 && ident_is_state(n->as.call.args[1]);

    int *args = n->as.call.arg_count
                    ? (int *)calloc(n->as.call.arg_count, sizeof(int))
                    : NULL;
    /* Pass 0 lowers every other argument; pass 1 the indexed borrows. */
    for (int indexed_borrow_pass = 0; indexed_borrow_pass < 2; indexed_borrow_pass++)
    for (size_t k = 0; k < n->as.call.arg_count; k++) {
        int is_indexed_borrow = !(bind_state && k == 1) && addr_through_index(n->as.call.args[k]);
        if (is_indexed_borrow != indexed_borrow_pass) continue;
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
    if (bind_state) callee = "loam_zeus_bind";

    IrInst *i = emit(n->as.call.is_fn_val ? IR_CALL_VAL : IR_CALL, n->loc);
    i->dst = dst;
    i->args = args;
    i->nargs = (int)n->as.call.arg_count;
    i->ty = ir_subst(n->ty);
    if (n->as.call.is_fn_val)
        i->a = callee_local;
    else
        i->callee = callee;
    /* The callee adopts the heap handles of the arguments it was passed
       without the codegen retaining a copy of them — a Box or an fn. A []T
       arrives retained instead, so the caller still owns a reference and has
       to run its drop; clearing the flag for one would leak the buffer, which
       is what this loop used to do for every argument of those types. */
    for (int k = 0; k < i->nargs; k++) {
        int a = i->args[k];
        if (a >= 0 && a < F->nlocals && type_arg_transfers(F->locals[a].ty))
            F->locals[a].needs_drop = 0;
    }
    return dst;
}

/** The non-generic fn a callee or fn-value names, when typecheck bound it. */
static const AstNode *bound_fn(const AstNode *e) {
    const AstNode *d = NULL;
    if (e && e->kind == AST_IDENT) d = e->as.ident.resolved;
    else if (e && e->kind == AST_FIELD) d = e->as.access.resolved;
    if (!d || d->kind != AST_FN_DECL || !d->as.fn.cname || d->as.fn.tparam_count) return NULL;
    return d;
}

/** `fn no_keys() -> []T { return []T {} }`: a call to it is the empty vec. */
static int returns_empty_vec(const AstNode *fn) {
    const AstNode *b = fn->as.fn.body;
    if (!b || b->kind != AST_BLOCK || b->as.block.stmt_count != 1) return 0;
    const AstNode *r = b->as.block.stmts[0];
    if (!r || r->kind != AST_RETURN || !r->as.ret.expr) return 0;
    const AstNode *a = r->as.ret.expr;
    return a->kind == AST_ARRAY_LIT && a->as.array_lit.count == 0 && a->ty &&
           a->ty->kind == TY_VEC;
}

const char *ir_const_fn_val(const AstNode *e) {
    if (!e || e->kind != AST_IDENT || !(e->flags & ASTF_FN_VAL)) return NULL;
    const AstNode *d = bound_fn(e);
    return d ? d->as.fn.cname : NULL;
}

int ir_const_empty_vec(const AstNode *e) {
    if (!e || e->kind != AST_CALL || e->as.call.arg_count || e->as.call.is_fn_val) return 0;
    if (!e->ty || e->ty->kind != TY_VEC) return 0;
    const AstNode *d = bound_fn(e->as.call.callee);
    return d && returns_empty_vec(d);
}

int ir_static_const(const AstNode *e) {
    if (!e) return 0;
    if (ir_const_fn_val(e) || ir_const_empty_vec(e)) return 1;
    switch (e->kind) {
        case AST_NUMBER:
        case AST_BOOL:
        case AST_STRING:
            return 1;
        case AST_FLOAT: /* finite only: %.17g of inf/nan is not C */
            return e->as.lit.f - e->as.lit.f == 0;
        case AST_STRUCT_LIT:
            if (!e->ty || e->ty->kind != TY_STRUCT) return 0;
            for (size_t k = 0; k < e->as.struct_lit.field_count; k++)
                if (!ir_static_const(e->as.struct_lit.fields[k].init)) return 0;
            return 1;
        default:
            return 0;
    }
}

/** A props literal written with few fields carries every other field's
 *  default, cloned in by typecheck — `Text(s)` builds six `none`s and a "" at
 *  each call site. When two or more of those defaults are constants, the
 *  literal starts from one static template of the struct (emitted once by the
 *  C backend) and only the remaining fields are stored. Returns field-indexed
 *  constant defaults, or NULL to lower every field. Generic instances are
 *  left alone: their C type can be registered after the templates go out. */
/* A `[]T` literal of integer constants, long enough to be worth a table. */
static int int_list_lit(const AstNode *n) {
    const int MIN_INT_TABLE = 8;
    Type *t = ir_subst(n->ty);
    if (!t || t->kind != TY_VEC || !t->elem || t->elem->kind != TY_INT) return 0;
    if ((int)n->as.array_lit.count < MIN_INT_TABLE) return 0;
    for (size_t k = 0; k < n->as.array_lit.count; k++)
        if (n->as.array_lit.elems[k]->kind != AST_NUMBER) return 0;
    return 1;
}

/* Plain data: no function value, so it can sit in a C initializer above the
   function prototypes. */
int ir_static_data(const AstNode *e) {
    if (!e || !e->ty) return 0;
    switch (e->kind) {
        case AST_NUMBER:
        case AST_BOOL:
        case AST_STRING:
            return 1;
        case AST_FLOAT:
            return e->as.lit.f - e->as.lit.f == 0;
        case AST_STRUCT_LIT:
            if (e->ty->kind != TY_STRUCT || e->ty->param_count) return 0;
            for (size_t k = 0; k < e->as.struct_lit.field_count; k++)
                if (!ir_static_data(e->as.struct_lit.fields[k].init)) return 0;
            return 1;
        default:
            return 0;
    }
}

static AstNode **lit_template(AstNode *n, Type *t) {
    if (!t || t->kind != TY_STRUCT || t->param_count || !t->field_count) return NULL;
    AstNode **d = (AstNode **)calloc(t->field_count, sizeof(AstNode *));
    int nd = 0;
    for (size_t f = 0; f < t->field_count; f++)
        for (size_t k = 0; k < n->as.struct_lit.field_count; k++) {
            FieldInit *fi = &n->as.struct_lit.fields[k];
            if (!t->field_names[f] || !fi->name || strcmp(t->field_names[f], fi->name) != 0)
                continue;
            if (fi->init && (fi->init->flags & ASTF_DEFAULT) && ir_static_const(fi->init)) {
                d[f] = fi->init;
                nd++;
            }
            break;
        }
    if (nd < 2) {
        free(d);
        return NULL;
    }
    return d;
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
        case AST_TRY: {
            /* `expr?` — evaluate the container, branch on its tag, return it
               whole on failure (Err / None == tag 1), else yield its `val`. */
            int v = lower_expr(n->as.try_expr.expr);
            Type *ct = (v >= 0 && v < F->nlocals) ? F->locals[v].ty : NULL;
            Type *vt = ir_subst(n->ty);

            /* tag = v.tag */
            IrPlace *tagp = (IrPlace *)calloc(1, sizeof(IrPlace));
            tagp->kind = IR_PL_FIELD;
            tagp->field = "tag";
            tagp->base = place_local(v, ct);
            tagp->ty = ty_int();
            int tagl = new_local(ty_int(), NULL, 0);
            IrInst *ldt = emit(IR_LOAD, n->loc);
            ldt->dst = tagl;
            ldt->place = tagp;
            ldt->ty = ty_int();

            /* cond = tag != 0  (1 == Err / None) */
            int zero = new_local(ty_int(), NULL, 0);
            IrInst *z = emit(IR_CONST_INT, n->loc);
            z->dst = zero;
            z->imm = 0;
            z->ty = ty_int();
            int cond = new_local(ty_bool(), NULL, 0);
            IrInst *cmp = emit(IR_BIN, n->loc);
            cmp->dst = cond;
            cmp->a = tagl;
            cmp->b = zero;
            cmp->binop = (int)TOK_BANG_EQ;
            cmp->ty = ty_bool();

            int fail = new_block(), join = new_block();
            term_br(cond, fail, join);

            /* Failure: return the whole container (the caller's failure). */
            CUR = fail;
            term_ret(v, n->loc);

            /* Success: load the payload, owning, out of the container. */
            CUR = join;
            IrPlace *valp = (IrPlace *)calloc(1, sizeof(IrPlace));
            valp->kind = IR_PL_FIELD;
            valp->field = "val";
            valp->base = place_local(v, ct);
            valp->ty = vt;
            int d = new_local(vt, NULL, 0);
            IrInst *ldv = emit(IR_LOAD, n->loc);
            ldv->dst = d;
            ldv->place = valp;
            ldv->ty = vt;
            return d;
        }
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
                /* Reading an owning value out of a place hands out an *owned*
                   copy: the codegen retains on the load, and `d` drops it at
                   the end of the scope unless a consumer moves it out first.
                   This used to mark the copy as "for reading only", which
                   left the reference with nobody to release: a `let` or a
                   struct field that stole it then leaked the buffer. */
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
            /* A literal op literal cannot overflow: fold it here and drop the
               trap. The proof is an exact evaluation in i64 (every operand
               type is at most 64-bit), so the check only stays when the true
               result genuinely leaves the type's range — which the checker has
               already rejected for a literal, so in practice it always folds. */
            if (i->checked && n->as.binary.left && n->as.binary.right &&
                n->as.binary.left->kind == AST_NUMBER && n->as.binary.right->kind == AST_NUMBER) {
                int64_t x = n->as.binary.left->as.lit.value;
                int64_t y = n->as.binary.right->as.lit.value;
                /* Keep the fold itself overflow-free: two i32 literals multiply
                   into i64 with room to spare. Wider literals stay checked. */
                if (x < -2147483647LL || x > 2147483647LL || y < -2147483647LL ||
                    y > 2147483647LL) {
                    return d;
                }
                Type *bt = i->ty;
                int bits = (bt && bt->bits) ? bt->bits : 64;
                int is_signed = (bt && bt->is_unsigned) ? 0 : 1;
                int64_t lo, hi;
                if (bits >= 64) {
                    lo = is_signed ? INT64_MIN : 0;
                    hi = is_signed ? INT64_MAX : (int64_t)-1; /* u64: always fits */
                } else if (is_signed) {
                    lo = -(int64_t)1 << (bits - 1);
                    hi = ((int64_t)1 << (bits - 1)) - 1;
                } else {
                    lo = 0;
                    hi = ((int64_t)1 << bits) - 1;
                }
                int64_t r = 0;
                int ok = 0;
                switch (op) {
                    case TOK_PLUS: r = x + y; ok = 1; break;
                    case TOK_MINUS: r = x - y; ok = 1; break;
                    case TOK_STAR: r = x * y; ok = 1; break;
                    case TOK_SLASH: if (y != 0) { r = x / y; ok = 1; } break;
                    case TOK_PERCENT: if (y != 0) { r = x % y; ok = 1; } break;
                    default: break;
                }
                /* Fits the type's range: the trap can never fire. Division by
                   zero stays checked (typecheck rejects a literal zero anyway). */
                if (ok && r >= lo && r <= hi) i->checked = 0;
            }
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
            AstNode **dflt = lit_template(n, t);
            if (t && t->kind == TY_STRUCT) {
                for (int f = 0; f < nf; f++) {
                    args[f] = -1;
                    if (dflt && dflt[f]) continue; /* from the template */
                    for (size_t k = 0; k < n->as.struct_lit.field_count; k++)
                        if (t->field_names[f] && n->as.struct_lit.fields[k].name &&
                            strcmp(t->field_names[f], n->as.struct_lit.fields[k].name) == 0) {
                            args[f] = lower_expr(n->as.struct_lit.fields[k].init);
                            break;
                        }
                    /* `none`'s payload: a zeroed temp of the field's type (the
                       template is already zero there). */
                    if (args[f] < 0 && n->as.struct_lit.zero_rest && !dflt) {
                        int z = new_local(t->field_types[f], NULL, 0);
                        F->locals[z].zero_init = 1;
                        args[f] = z;
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
            i->lit_dflt = dflt;
            for (int k = 0; k < nf; k++) {
                if (args[k] < 0 || args[k] >= F->nlocals) continue;
                if (!type_needs_drop(F->locals[args[k]].ty)) continue;
                /* An owned string (or a struct/array that holds one) is copied
                   in — the container retains and the source keeps its own
                   reference, so it must still drop. A struct that owns a string
                   retains every refcounted leaf (codegen's IR_STRUCT_LIT), so a
                   `[]T` beside the string keeps its drop too; clearing it
                   leaked the list once per literal. A Box is moved either way. */
                if (type_string_owns() && type_owns_string(F->locals[args[k]].ty))
                    continue;
                if (type_string_owns() && t && t->kind == TY_STRUCT && type_owns_string(t) &&
                    !push_moves(F->locals[args[k]].ty))
                    continue;
                F->locals[args[k]].needs_drop = 0;
            }
            return d;
        }
        case AST_ARRAY_LIT: {
            int nf = (int)n->as.array_lit.count;
            /* A run of integers is data. A push each costs some 30 bytes of
               code per element in a table that never changes. */
            if (int_list_lit(n)) {
                int64_t *vals = (int64_t *)calloc((size_t)nf, sizeof(int64_t));
                for (int k = 0; k < nf; k++) vals[k] = n->as.array_lit.elems[k]->as.lit.value;
                int d = new_local(n->ty, NULL, 0);
                IrInst *i = emit(IR_ARRAY_LIT, n->loc);
                i->dst = d;
                i->lit_ints = vals;
                i->nlit_ints = nf;
                i->ty = ir_subst(n->ty);
                return d;
            }
            int *args = nf ? (int *)calloc((size_t)nf, sizeof(int)) : NULL;
            for (int k = 0; k < nf; k++) args[k] = lower_expr(n->as.array_lit.elems[k]);
            int d = new_local(n->ty, NULL, 0);
            IrInst *i = emit(IR_ARRAY_LIT, n->loc);
            i->dst = d;
            i->args = args;
            i->nargs = nf;
            i->ty = ir_subst(n->ty);
            if (i->ty && i->ty->kind == TY_VEC) {
                for (int k = 0; k < nf; k++) {
                    if (args[k] < 0 || args[k] >= F->nlocals) continue;
                    if (!type_needs_drop(F->locals[args[k]].ty)) continue;
                    /* Same as `push`: each slot retains the element's
                       refcounted leaves, so its source keeps and drops its own
                       reference; only an element holding a Box moves in. */
                    if (!push_moves(F->locals[args[k]].ty)) continue;
                    F->locals[args[k]].needs_drop = 0;
                }
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
                    snprintf(buf, sizeof buf, "loam_clos_%d_%s", n->as.fn.clos_id, cname_override);
                else
                    snprintf(buf, sizeof buf, "loam_clos_%d", n->as.fn.clos_id);
                i->callee = loam_dup(buf);
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
 * because `loam_move_ptr` NULLs the source and `loam_drop` skips NULL — a
 * runtime check standing in for a static fact. Stating it here means a backend
 * that does not null on move is still correct.
 *
 * Only a move of a non-Copy value transfers. An IR_MOVE of a Copy value — a
 * Copy `[]T`, a string, a struct of Copy fields — is a copy: the backend
 * gives the destination its own reference and the source keeps its own, so
 * the source still needs its drop. Clearing it leaked one reference per
 * `let x = f()` on every function exit (block exits, which drop as they
 * close, were already right).
 */
static void transfer_moved(void) {
    for (int b = 0; b < F->nblocks; b++)
        for (int i = 0; i < F->blocks[b].ninsts; i++) {
            IrInst *in = &F->blocks[b].insts[i];
            if (in->op != IR_MOVE || in->a < 0 || in->a >= F->nlocals) continue;
            Type *ty = in->ty ? in->ty : (in->dst >= 0 && in->dst < F->nlocals ? F->locals[in->dst].ty : NULL);
            if (ty && type_is_copy(ty)) continue;
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
static void mark_touched(uint64_t *set, int l) {
    if (l >= 0 && l < F->nlocals) set[l / 64] |= (uint64_t)1 << (l % 64);
}

static void mark_place_touched(uint64_t *set, const IrPlace *p) {
    for (; p; p = p->base) {
        if (p->kind == IR_PL_LOCAL) mark_touched(set, p->local);
        if (p->kind == IR_PL_INDEX) mark_touched(set, p->index);
    }
}

/**
 * Which locals some path from the entry to the end of each block may have
 * touched. Every owning local starts as `{0}` (codegen declares it so), and
 * dropping a `{0}` is a no-op, so an exit only needs to drop what a path to
 * it could have set. "Touched" is deliberately coarse — any mention but a
 * drop (a dst, an operand, a call argument, a place's root or index) — so a
 * local the analysis misses can only be one no path ever names. Returns one
 * bitset of `words` words per block; the caller frees it.
 */
static uint64_t *touched_at_block_ends(int words) {
    int nb = F->nblocks;
    uint64_t *gen = (uint64_t *)calloc((size_t)nb * (size_t)words, sizeof(uint64_t));
    uint64_t *out = (uint64_t *)calloc((size_t)nb * (size_t)words, sizeof(uint64_t));
    uint64_t *in = (uint64_t *)calloc((size_t)nb * (size_t)words, sizeof(uint64_t));
    int *work = (int *)malloc((size_t)(nb + 1) * sizeof(int));
    char *queued = (char *)calloc((size_t)nb + 1, 1);
    if (!gen || !out || !in || !work || !queued) loam_fatal("out of memory");
    for (int b = 0; b < nb; b++) {
        uint64_t *g = gen + (size_t)b * words;
        const IrBlock *bb = &F->blocks[b];
        for (int i = 0; i < bb->ninsts; i++) {
            const IrInst *in_i = &bb->insts[i];
            if (in_i->op == IR_DROP) continue;
            mark_touched(g, in_i->dst);
            mark_touched(g, in_i->a);
            mark_touched(g, in_i->b);
            for (int k = 0; k < in_i->nargs; k++)
                if (in_i->args) mark_touched(g, in_i->args[k]);
            mark_place_touched(g, in_i->place);
        }
    }
    /* Parameters and globals hold a value from the start. */
    for (int l = 0; l < F->nlocals; l++)
        if (F->locals[l].is_param || F->locals[l].is_global) mark_touched(in, l);
    int nwork = 0;
    if (nb > 0) {
        work[nwork++] = 0;
        queued[0] = 1;
    }
    while (nwork > 0) {
        int b = work[--nwork];
        queued[b] = 0;
        uint64_t *o = out + (size_t)b * words;
        const uint64_t *i = in + (size_t)b * words, *g = gen + (size_t)b * words;
        for (int w = 0; w < words; w++) o[w] = i[w] | g[w];
        const IrBlock *bb = &F->blocks[b];
        int succ[2] = {-1, -1};
        int nsucc = 0;
        if (bb->term == IR_TERM_JMP) succ[nsucc++] = bb->succ[0];
        if (bb->term == IR_TERM_BR || bb->term == IR_TERM_SWITCH) {
            succ[nsucc++] = bb->succ[0];
            if (bb->term == IR_TERM_BR) succ[nsucc++] = bb->succ[1];
        }
        for (int k = 0; k < nsucc + (bb->term == IR_TERM_SWITCH ? bb->sw_n : 0); k++) {
            int s = k < nsucc ? succ[k] : bb->sw_blocks[k - nsucc];
            if (s < 0 || s >= nb) continue;
            uint64_t *si = in + (size_t)s * words;
            int grew = 0;
            for (int w = 0; w < words; w++) {
                uint64_t merged = si[w] | o[w];
                if (merged != si[w]) {
                    si[w] = merged;
                    grew = 1;
                }
            }
            if (grew && !queued[s]) {
                queued[s] = 1;
                work[nwork++] = s;
            }
        }
    }
    free(gen);
    free(in);
    free(work);
    free(queued);
    return out;
}

static void emit_drops_at_exits(void) {
    transfer_moved();
    int words = (F->nlocals + 63) / 64;
    uint64_t *touched = words ? touched_at_block_ends(words) : NULL;
    for (int b = 0; b < F->nblocks; b++) {
        IrBlock *bb = &F->blocks[b];
        if (bb->term != IR_TERM_RET) continue;
        int saved = CUR;
        CUR = b;
        const uint64_t *t = touched + (size_t)b * words;
        for (int i = F->nlocals - 1; i >= 0; i--) {
            if (!F->locals[i].needs_drop) continue;
            if (i == bb->term_val) continue;
            /* No path here names it: it is still `{0}`. */
            if (!(t[i / 64] >> (i % 64) & 1)) continue;
            IrInst *d = emit(IR_DROP, bb->term_loc);
            d->a = i;
        }
        CUR = saved;
    }
    free(touched);
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
                /* One rule: a captured `let mut int` is a signal cell, in a component
                   (owned by its frame) as anywhere else. */
                const char *callee = "loam_zeus_signal";
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
            /* A module global is never dropped, so returning one hands the
               caller an alias that it would free. Materialize an owned copy:
               the load takes a reference and the return value is exempt from
               the exit drops, so that reference transfers to the caller. */
            if (v >= 0 && v < F->nlocals && F->locals[v].is_global &&
                type_needs_drop(F->locals[v].ty)) {
                Type *vt = F->locals[v].ty;
                int d = new_local(vt, NULL, 0);
                IrInst *ld = emit(IR_LOAD, n->loc);
                ld->dst = d;
                ld->place = place_local(v, vt);
                ld->ty = vt;
                v = d;
            }
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
            int base = F->nlocals;
            loop_push(latch, exit, base);
            lower_stmt(n->as.for_stmt.body);
            if (nloop > 0) nloop--;
            scope_pop_to(sdepth - 1);
            sdepth--;
            /* The body's `let`s are re-initialized on the next pass, so this
               pass has to release what they own before looping. `break` and
               `continue` already do this; falling through to the latch did
               not, so a body local that owns a buffer leaked one per pass. */
            if (!block_closed()) emit_drops_from(base, n->loc);
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
            /* Everything a pass creates — the condition's temporaries and the
               body's `let`s — is re-created on the next pass, so the per-pass
               drop range starts here and is released at the end of the pass. */
            int base = F->nlocals;
            term_jmp(head);
            CUR = head;
            int c = lower_expr(n->as.if_stmt.cond);
            term_br(c, body, exit);
            CUR = body;
            loop_push(head, exit, base);
            lower_stmt(n->as.if_stmt.then_block);
            if (nloop > 0) nloop--;
            if (!block_closed()) emit_drops_from(base, n->loc);
            term_jmp(head);
            CUR = exit;
            return;
        }
        case AST_BREAK:
            if (nloop <= 0) {
                F->lowered = 0;
                return;
            }
            /* Leaving a switch needs no drops: what a case owns lives until the
               enclosing pass or the function ends, as it does when a case runs
               off its end. */
            if (!loop_is_switch[nloop - 1]) emit_drops_from(loop_base[nloop - 1], n->loc);
            term_jmp(loop_brk[nloop - 1]);
            return;
        case AST_CONTINUE: {
            int li = nloop - 1;
            while (li >= 0 && loop_is_switch[li]) li--;
            if (li < 0) {
                F->lowered = 0;
                return;
            }
            emit_drops_from(loop_base[li], n->loc);
            term_jmp(loop_cont[li]);
            return;
        }
        case AST_SWITCH: {
            /* A single indirect dispatch, not a chain of compares: the labels
               are constants, so the backend can build a jump table. */
            int scrut = lower_expr(n->as.switch_stmt.scrut);
            size_t nc = n->as.switch_stmt.case_count;
            int join = new_block();
            int *bodies = (int *)malloc((nc ? nc : 1) * sizeof(int));
            if (!bodies) loam_fatal("out of memory");
            int dflt = join, nvals = 0;
            int64_t *vals = NULL;
            int *targets = NULL;
            for (size_t i = 0; i < nc; i++) {
                AstNode *cs = n->as.switch_stmt.cases[i];
                bodies[i] = new_block();
                if (!cs || cs->kind != AST_CASE) continue;
                if (cs->as.switch_case.is_default) dflt = bodies[i];
                for (size_t p = 0; p < cs->as.switch_case.pat_count; p++) {
                    int64_t v = 0;
                    if (!ast_case_value(cs->as.switch_case.pats[p], &v)) {
                        F->lowered = 0;
                        continue;
                    }
                    vals = (int64_t *)realloc(vals, (size_t)(nvals + 1) * sizeof(int64_t));
                    targets = (int *)realloc(targets, (size_t)(nvals + 1) * sizeof(int));
                    if (!vals || !targets) loam_fatal("out of memory");
                    vals[nvals] = v;
                    targets[nvals++] = bodies[i];
                }
            }
            term_switch(scrut, vals, targets, nvals, dflt);
            loop_push(-1, join, 0);
            loop_is_switch[nloop - 1] = 1;
            for (size_t i = 0; i < nc; i++) {
                AstNode *cs = n->as.switch_stmt.cases[i];
                CUR = bodies[i];
                if (cs && cs->kind == AST_CASE) lower_stmt(cs->as.switch_case.body);
                /* Running off a case's end falls into the next one. */
                term_jmp(i + 1 < nc ? bodies[i + 1] : join);
            }
            if (nloop > 0) nloop--;
            free(bodies);
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
            if (!clos_nodes || !clos_mods) loam_fatal("out of memory");
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
        case AST_SWITCH:
            collect_clos(n->as.switch_stmt.scrut);
            for (size_t i = 0; i < n->as.switch_stmt.case_count; i++)
                collect_clos(n->as.switch_stmt.cases[i]);
            break;
        case AST_CASE:
            for (size_t i = 0; i < n->as.switch_case.pat_count; i++)
                collect_clos(n->as.switch_case.pats[i]);
            collect_clos(n->as.switch_case.body);
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
        case AST_TRY:
            collect_clos(n->as.try_expr.expr);
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
            snprintf(buf, sizeof buf, "loam_clos_%d_%s", d->as.fn.clos_id, cname_override);
        else
            snprintf(buf, sizeof buf, "loam_clos_%d", d->as.fn.clos_id);
        F->cname = loam_dup(buf);
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
        case AST_SWITCH:
            lower_clos_in(m, n->as.switch_stmt.scrut);
            for (size_t i = 0; i < n->as.switch_stmt.case_count; i++)
                lower_clos_in(m, n->as.switch_stmt.cases[i]);
            break;
        case AST_CASE:
            lower_clos_in(m, n->as.switch_case.body);
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
        case AST_TRY:
            lower_clos_in(m, n->as.try_expr.expr);
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

/* --- borrowed loads ------------------------------------------------------ */

static void free_place(IrPlace *p);

/** 1 if place `p` reads local `l`, as its root or as an index. */
static int place_reads_local(const IrPlace *p, int l) {
    for (; p; p = p->base) {
        if (p->kind == IR_PL_LOCAL && p->local == l) return 1;
        if (p->kind == IR_PL_INDEX && p->index == l) return 1;
    }
    return 0;
}

/** The local a place is rooted in, or -1. */
static int place_root_local(const IrPlace *p) {
    while (p && p->base) p = p->base;
    return p && p->kind == IR_PL_LOCAL ? p->local : -1;
}

/** 1 if instruction `in` reads local `l` anywhere. */
static int inst_reads_local(const IrInst *in, int l) {
    if (in->a == l || in->b == l) return 1;
    for (int k = 0; k < in->nargs; k++)
        if (in->args && in->args[k] == l) return 1;
    return place_reads_local(in->place, l);
}

/** 1 if `in` can run between a borrowed load and its last read: it frees,
    writes and calls nothing, so the place the load read from keeps the value
    (and its storage) alive. */
static int inst_keeps_places(const IrInst *in) {
    switch (in->op) {
        case IR_CONST_INT:
        case IR_CONST_BOOL:
        case IR_CONST_STR:
        case IR_CONST_FLOAT:
        case IR_LOAD:
        case IR_BIN:
        case IR_UN:
        case IR_BOUND:
        case IR_CAST:
        case IR_SIZEOF:
        case IR_ADDR:
        case IR_FN_VAL:
            return 1;
        default:
            return 0;
    }
}

/** 1 if `in` only reads string temp `t` in place: a `==` / `!=` operand. */
static int inst_reads_string_in_place(const IrInst *in, int t) {
    if (in->dst == t) return 0;
    if (in->op != IR_BIN) return 0;
    if (in->binop != TOK_EQ_EQ && in->binop != TOK_BANG_EQ) return 0;
    for (int k = 0; k < in->nargs; k++)
        if (in->args && in->args[k] == t) return 0;
    return !place_reads_local(in->place, t);
}

/**
 * A string read out of a place is an owned copy: the temp takes a reference
 * and drops it at the end of its scope. When the temp is only compared before
 * anything could release what the place holds, the place's own reference
 * keeps the bytes alive for the whole read, so the pair is pure overhead.
 * Such a load is marked `borrowed` (no retain) and its temp loses its drops.
 *
 * Conditions, all checked here: the temp is a compiler temp loaded exactly
 * once; every read of it is an `==` / `!=` operand in the load's own block;
 * no terminator reads it; and every instruction from the load to its last
 * read is one that frees, writes and calls nothing (`inst_keeps_places`) and
 * does not overwrite the local the place is rooted in.
 */
static void borrow_read_only_loads(IrFn *fn) {
    if (!fn->lowered || !type_string_owns()) return;
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock *bb = &fn->blocks[b];
        for (int i = 0; i < bb->ninsts; i++) {
            IrInst *ld = &bb->insts[i];
            int t = ld->dst;
            if (ld->op != IR_LOAD || t < 0 || t >= fn->nlocals) continue;
            IrLocal *tl = &fn->locals[t];
            if (tl->name || tl->is_param || tl->is_global || !tl->needs_drop) continue;
            if (!tl->ty || tl->ty->kind != TY_STRING) continue;
            int root = place_root_local(ld->place);
            if (root == t) continue;

            /* Reads and drops of `t` in this block, after the load. */
            int last_read = -1, ok = 1;
            for (int j = i + 1; j < bb->ninsts && ok; j++) {
                const IrInst *in = &bb->insts[j];
                if (in->op == IR_DROP && in->a == t) continue;
                if (in->dst == t) ok = 0;
                else if (inst_reads_local(in, t)) {
                    if (inst_reads_string_in_place(in, t)) last_read = j;
                    else ok = 0;
                }
            }
            if (!ok || last_read < 0) continue;
            if (bb->term_val == t) continue;
            for (int j = i + 1; j <= last_read && ok; j++) {
                const IrInst *in = &bb->insts[j];
                if (in->op == IR_DROP && in->a == t) {
                    ok = 0; /* dropped before its last read: not a plain temp */
                    break;
                }
                if (!inst_keeps_places(in)) ok = 0;
                if (root >= 0 && in->dst == root) ok = 0;
            }
            if (!ok) continue;

            /* Elsewhere (other blocks, and this one before the load) `t` may
               only be dropped, never read or set. */
            for (int ob = 0; ob < fn->nblocks && ok; ob++) {
                const IrBlock *o = &fn->blocks[ob];
                if (o->term_val == t) ok = 0;
                int end = ob == b ? i : o->ninsts;
                for (int j = 0; j < end && ok; j++) {
                    const IrInst *in = &o->insts[j];
                    if (in->op == IR_DROP && in->a == t) continue;
                    if (in->dst == t || inst_reads_local(in, t)) ok = 0;
                }
            }
            if (!ok) continue;

            ld->borrowed = 1;
            tl->needs_drop = 0;
            /* Its drops go: the temp holds no reference of its own. */
            for (int ob = 0; ob < fn->nblocks; ob++) {
                IrBlock *o = &fn->blocks[ob];
                int w = 0;
                for (int j = 0; j < o->ninsts; j++) {
                    if (o->insts[j].op == IR_DROP && o->insts[j].a == t) {
                        free_place(o->insts[j].place);
                        free(o->insts[j].args);
                        continue;
                    }
                    o->insts[w++] = o->insts[j];
                }
                o->ninsts = w;
            }
            /* `bb` may have shrunk; restart its scan from the load's new spot. */
            for (int j = 0; j < bb->ninsts; j++)
                if (bb->insts[j].op == IR_LOAD && bb->insts[j].dst == t) {
                    i = j;
                    break;
                }
        }
    }
}

/* --- which owning locals need a `{0}` ----------------------------------- */

/** The runtime builtins codegen (`emit_ir_inst`, IR_CALL) writes the result
    of in place rather than as `dst = f(args);`. Keep in sync with it. */
static const char *const CALLS_WRITING_DST_IN_PLACE[] = {
    "loam_ch_alloc", "loam_ch_pop", "loam_ch_ready", "loam_ch_recv", "loam_ch_send",
    "loam_ch_try_send", "loam_fut_load", "loam_fut_push", "loam_fut_store",
    "loam_sig_load", "loam_sig_push", "loam_sig_store", "loam_value_key",
    "loam_vec_pop", "loam_vec_push",
};

static int call_sets_dst_whole(const char *callee) {
    if (!callee) return 0;
    if (strncmp(callee, "loam_zeus_sig", 13) == 0) return 0;
    for (size_t i = 0; i < sizeof CALLS_WRITING_DST_IN_PLACE / sizeof *CALLS_WRITING_DST_IN_PLACE; i++)
        if (strcmp(callee, CALLS_WRITING_DST_IN_PLACE[i]) == 0) return 0;
    return 1;
}

/** 1 if `in` sets its dst whole, without reading the old value. */
static int inst_defines_fresh(const IrInst *in) {
    switch (in->op) {
        case IR_CONST_INT:
        case IR_CONST_BOOL:
        case IR_CONST_STR:
        case IR_CONST_FLOAT:
        case IR_LOAD:
        case IR_BIN:
        case IR_UN:
        case IR_SIZEOF:
        case IR_MOVE:
        case IR_CALL_VAL:
            return 1;
        case IR_CALL:
            return call_sets_dst_whole(in->callee);
        default:
            return 0;
    }
}

static int bit_has(const uint64_t *set, int l) { return (int)(set[l / 64] >> (l % 64) & 1); }

/** A use of `l` (anything but a fresh definition): it must hold a value. */
static void use_local(IrFn *fn, const uint64_t *assigned, int l) {
    if (l < 0 || l >= fn->nlocals) return;
    if (!bit_has(assigned, l)) fn->locals[l].needs_zero = 1;
}

static void use_place(IrFn *fn, const uint64_t *assigned, const IrPlace *p) {
    for (; p; p = p->base) {
        if (p->kind == IR_PL_LOCAL) use_local(fn, assigned, p->local);
        if (p->kind == IR_PL_INDEX) use_local(fn, assigned, p->index);
    }
}

/** Walk block `b` from the assigned set `cur` (updated in place); with
    `record`, mark every local some use reaches unassigned. */
static void assigned_through_block(IrFn *fn, int b, uint64_t *cur, int record) {
    const IrBlock *bb = &fn->blocks[b];
    for (int i = 0; i < bb->ninsts; i++) {
        const IrInst *in = &bb->insts[i];
        int fresh = in->dst >= 0 && inst_defines_fresh(in);
        if (record) {
            use_local(fn, cur, in->a);
            use_local(fn, cur, in->b);
            for (int k = 0; k < in->nargs; k++)
                if (in->args) use_local(fn, cur, in->args[k]);
            use_place(fn, cur, in->place);
            if (in->dst >= 0 && !fresh) use_local(fn, cur, in->dst);
        }
        if (in->dst >= 0 && in->dst < fn->nlocals)
            cur[in->dst / 64] |= (uint64_t)1 << (in->dst % 64);
    }
    if (record) use_local(fn, cur, bb->term_val);
}

/**
 * Codegen used to declare every owning local `= {0}`, because a drop at an
 * exit may meet one a path never assigned. In a fn with many such locals that
 * zeroing runs on every call. A local only needs it if some use — a read, a
 * drop, a store into it, any write that is not a whole fresh value — can run
 * before every path has assigned it: a forward "surely assigned" analysis
 * (intersection over predecessors) finds those, and the rest are declared
 * bare, as C would. Unsure means zeroed.
 */
static void mark_needs_zero(IrFn *fn) {
    int nb = fn->nblocks, words = (fn->nlocals + 63) / 64;
    if (!fn->lowered || words == 0 || nb == 0) {
        for (int l = 0; l < fn->nlocals; l++) fn->locals[l].needs_zero = 1;
        return;
    }
    uint64_t *in = (uint64_t *)malloc((size_t)nb * (size_t)words * sizeof(uint64_t));
    uint64_t *cur = (uint64_t *)malloc((size_t)words * sizeof(uint64_t));
    int *work = (int *)malloc((size_t)nb * sizeof(int));
    char *queued = (char *)calloc((size_t)nb, 1);
    char *reached = (char *)calloc((size_t)nb, 1);
    if (!in || !cur || !work || !queued || !reached) loam_fatal("out of memory");
    /* Top (all assigned) everywhere but the entry: the meet only shrinks it. */
    memset(in, 0xff, (size_t)nb * (size_t)words * sizeof(uint64_t));
    memset(in, 0, (size_t)words * sizeof(uint64_t));
    for (int l = 0; l < fn->nlocals; l++)
        if (fn->locals[l].is_param || fn->locals[l].is_global)
            in[l / 64] |= (uint64_t)1 << (l % 64);
    int nwork = 0;
    work[nwork++] = 0;
    queued[0] = 1;
    reached[0] = 1;
    while (nwork > 0) {
        int b = work[--nwork];
        queued[b] = 0;
        memcpy(cur, in + (size_t)b * words, (size_t)words * sizeof(uint64_t));
        assigned_through_block(fn, b, cur, 0);
        const IrBlock *bb = &fn->blocks[b];
        int nsucc = bb->term == IR_TERM_JMP ? 1 : bb->term == IR_TERM_BR ? 2
                    : bb->term == IR_TERM_SWITCH ? 1 + bb->sw_n : 0;
        for (int k = 0; k < nsucc; k++) {
            int s = k < (bb->term == IR_TERM_SWITCH ? 1 : nsucc) ? bb->succ[k] : bb->sw_blocks[k - 1];
            if (s < 0 || s >= nb) continue;
            uint64_t *si = in + (size_t)s * words;
            int shrank = !reached[s];
            reached[s] = 1;
            for (int w = 0; w < words; w++) {
                uint64_t met = si[w] & cur[w];
                if (met != si[w]) {
                    si[w] = met;
                    shrank = 1;
                }
            }
            if (shrank && !queued[s]) {
                queued[s] = 1;
                work[nwork++] = s;
            }
        }
    }
    for (int b = 0; b < nb; b++) {
        if (!reached[b]) continue; /* never runs */
        memcpy(cur, in + (size_t)b * words, (size_t)words * sizeof(uint64_t));
        assigned_through_block(fn, b, cur, 1);
    }
    free(in);
    free(cur);
    free(work);
    free(queued);
    free(reached);
}


IrModule *ir_lower(LoamModule *mods, int nmods) {
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
                snprintf(buf, sizeof buf, "loam__init");
            else
                snprintf(buf, sizeof buf, "loam_%s__init", mods[mi].name);
            F->cname = loam_dup(buf);
        }
        F->name = "__init";
        F->init_mod = mi;
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
            /* Codegen writes it as the global's initializer: a table of
               constants costs data, not startup code. */
            if (ir_static_data(d->as.var.init)) continue;
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
    for (int f = 0; f < m->nfns; f++) borrow_read_only_loads(&m->fns[f]);
    for (int f = 0; f < m->nfns; f++) mark_needs_zero(&m->fns[f]);
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
            fprintf(o, i->borrowed ? "load.borrow " : "load ");
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
            for (int k = 0; k < i->nlit_ints; k++)
                fprintf(o, "%s%lld", k ? ", " : "", (long long)i->lit_ints[k]);
            for (int k = 0; k < i->nargs; k++) {
                if (i->args[k] < 0 && i->lit_dflt)
                    fprintf(o, "%sdefault", k ? ", " : "");
                else
                    fprintf(o, "%s%%%d", k ? ", " : "", i->args[k]);
            }
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
        case IR_PANIC:
            fprintf(o, "panic %%%d", i->a);
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
                case IR_TERM_SWITCH:
                    fprintf(o, "    switch %%%d [", bb->term_val);
                    for (int c = 0; c < bb->sw_n; c++)
                        fprintf(o, "%s%lld: bb%d", c ? ", " : "", (long long)bb->sw_vals[c],
                                bb->sw_blocks[c]);
                    fprintf(o, "], default bb%d\n", bb->succ[0]);
                    break;
                case IR_TERM_UNREACHABLE: fprintf(o, "    unreachable\n"); break;
            }
        }
        fprintf(o, "\n");
    }
}

/* --- the full dump (`--dump-ir`) ----------------------------------------- */

/* One line per local, instruction and terminator, every field codegen reads,
   so the self-hosted lowering can be diffed exactly. Types, strings and node
   references are spelled as `--dump-checked` spells them. A field at its zero
   value is left out; operands (`dst a b`) are always printed. A line's source
   file is printed only where it changes within a fn. */

void ast_dump_put_str(const char *s);
void ast_dump_put_type(const Type *t);
void ast_dump_put_f64(double v);

static const char *const IR_OP_NAMES[] = {
    "const.int", "const.bool", "const.str", "const.float", "load", "store", "addr", "move",
    "bin", "un", "call", "call.val", "alloc", "drop", "bound", "struct", "array", "fnval",
    "clos", "cast", "sizeof", "panic",
};

static const char *const IR_TERM_NAMES[] = {"ret", "jmp", "br", "switch", "unreachable"};

static const char *dump_last_file;

static void dump_line(SourceLoc l) {
    printf(" @%d", l.line);
    const char *f = l.file ? l.file : "";
    if (!dump_last_file || strcmp(dump_last_file, f) != 0) {
        fputs(" file=", stdout);
        ast_dump_put_str(f);
        dump_last_file = f;
    }
}

static void dump_place(const IrPlace *p) {
    if (!p) {
        fputs("-", stdout);
        return;
    }
    switch (p->kind) {
        case IR_PL_LOCAL: printf("%%%d", p->local); break;
        case IR_PL_FIELD:
            dump_place(p->base);
            putchar('.');
            fputs(p->field ? p->field : "?", stdout);
            break;
        case IR_PL_INDEX:
            dump_place(p->base);
            printf("[%%%d]", p->index);
            break;
        case IR_PL_DEREF:
            fputs("*(", stdout);
            dump_place(p->base);
            putchar(')');
            break;
    }
    putchar('{');
    ast_dump_put_type(p->ty);
    putchar('}');
}

static void dump_inst(const IrInst *i) {
    printf("    %s %d %d %d", IR_OP_NAMES[i->op], i->dst, i->a, i->b);
    dump_line(i->loc);
    if (i->place) {
        fputs(" place=", stdout);
        dump_place(i->place);
    }
    if (i->imm) printf(" imm=%lld", (long long)i->imm);
    if (i->fimm != 0.0) {
        fputs(" f=", stdout);
        ast_dump_put_f64(i->fimm);
    }
    if (i->str) {
        fputs(" str=", stdout);
        ast_dump_put_str(i->str);
    }
    if (i->callee) printf(" callee=%s", i->callee);
    if (i->nargs) {
        fputs(" args=", stdout);
        for (int k = 0; k < i->nargs; k++) printf("%s%d", k ? "," : "", i->args[k]);
    }
    if (i->binop) printf(" op=%d", i->binop);
    if (i->is_mut) fputs(" mut=1", stdout);
    if (i->conv_mode) printf(" conv=%d", i->conv_mode);
    if (i->checked) fputs(" chk=1", stdout);
    if (i->borrowed) fputs(" borrowed=1", stdout);
    if (i->nlit_ints) {
        fputs(" ints=", stdout);
        for (int k = 0; k < i->nlit_ints; k++)
            printf("%s%lld", k ? "," : "", (long long)i->lit_ints[k]);
    }
    if (i->lit_dflt) {
        /* `lit_dflt[k]` is always the literal's own value for field k (a
           constant default), so which fields the template supplies is all
           there is to compare. */
        fputs(" dflt=", stdout);
        int first = 1;
        for (int k = 0; k < i->nargs; k++)
            if (i->lit_dflt[k]) {
                printf("%s%d", first ? "" : ",", k);
                first = 0;
            }
    }
    if (i->ty) {
        fputs(" ty=", stdout);
        ast_dump_put_type(i->ty);
    }
    putchar('\n');
}

void ir_dump(const IrModule *m) {
    for (int f = 0; f < m->nfns; f++) {
        const IrFn *fn = &m->fns[f];
        dump_last_file = NULL;
        fputs("fn ", stdout);
        fputs(fn->cname ? fn->cname : "-", stdout);
        if (fn->name) {
            fputs(" name=", stdout);
            ast_dump_put_str(fn->name);
        }
        fputs(" sig=", stdout);
        ast_dump_put_type(fn->sig);
        printf(" params=%d", fn->nparams);
        if (fn->is_main) fputs(" main=1", stdout);
        if (!fn->lowered) fputs(" partial=1", stdout);
        if (fn->clos_id) printf(" clos=%d env=%d", fn->clos_id, fn->env_local);
        if (fn->init_mod) printf(" init_mod=%d", fn->init_mod);
        putchar('\n');
        for (int c = 0; c < fn->ncaps; c++) {
            fputs("  cap ", stdout);
            ast_dump_put_str(fn->caps[c] ? fn->caps[c] : "");
            putchar(' ');
            ast_dump_put_type(fn->cap_types ? fn->cap_types[c] : NULL);
            putchar('\n');
        }
        for (int l = 0; l < fn->nlocals; l++) {
            const IrLocal *lo = &fn->locals[l];
            printf("  local %d", lo->id);
            if (lo->name) {
                fputs(" name=", stdout);
                ast_dump_put_str(lo->name);
            }
            fputs(" ty=", stdout);
            ast_dump_put_type(lo->ty);
            if (lo->is_param) fputs(" param=1", stdout);
            if (lo->is_global) fputs(" global=1", stdout);
            if (lo->cname) printf(" cname=%s", lo->cname);
            if (lo->needs_drop) fputs(" drop=1", stdout);
            if (lo->zero_init) fputs(" zero_init=1", stdout);
            if (lo->needs_zero) fputs(" needs_zero=1", stdout);
            putchar('\n');
        }
        for (int b = 0; b < fn->nblocks; b++) {
            const IrBlock *bb = &fn->blocks[b];
            printf("  bb%d\n", bb->id);
            for (int i = 0; i < bb->ninsts; i++) dump_inst(&bb->insts[i]);
            printf("    %s %d %d %d", IR_TERM_NAMES[bb->term], bb->term_val, bb->succ[0],
                   bb->succ[1]);
            for (int c = 0; c < bb->sw_n; c++)
                printf("%s%lld:%d", c ? "," : " cases=", (long long)bb->sw_vals[c],
                       bb->sw_blocks[c]);
            if (bb->term != IR_TERM_UNREACHABLE) dump_line(bb->term_loc);
            putchar('\n');
        }
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
        for (int c = 0; c < bb->sw_n; c++) {
            if (bb->sw_blocks[c] < 0 || bb->sw_blocks[c] >= fn->nblocks) {
                fprintf(stderr, "ir: %s bb%d switch target %d out of range\n", fn->cname,
                        bb->id, bb->sw_blocks[c]);
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
                if (in->args[k] >= fn->nlocals ||
                    (in->args[k] < 0 && !(in->op == IR_STRUCT_LIT && in->lit_dflt))) {
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
            free(fn->blocks[b].sw_vals);
            free(fn->blocks[b].sw_blocks);
        }
        free(fn->blocks);
        free(fn->locals);
    }
    free(m->fns);
    free(m);
}
