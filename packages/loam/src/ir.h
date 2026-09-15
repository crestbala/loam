/**
 * ir.h — the checked, backend-neutral representation.
 *
 * Everything a backend would otherwise have to re-derive from the AST is
 * explicit here: control flow is blocks and terminators rather than nested
 * statements, drops are instructions rather than a scope stack, generics are
 * already monomorphized, and every operand is a numbered local with a type.
 *
 * A backend walking this emits code; it never makes a language decision.
 */
#ifndef LOAM_IR_H
#define LOAM_IR_H

#include "ast.h"
#include "module.h"
#include "sema/type.h"

#include <stdint.h>
#include <stdio.h>

typedef enum {
    IR_PL_LOCAL,
    IR_PL_FIELD,
    IR_PL_INDEX,
    IR_PL_DEREF,
} IrPlaceKind;

/** A memory location, as a path from a local. Borrowck's conflict rule reads
    these directly instead of re-parsing an expression tree. */
typedef struct IrPlace {
    IrPlaceKind kind;
    int local;              /* IR_PL_LOCAL */
    const char *field;      /* IR_PL_FIELD */
    int index;              /* IR_PL_INDEX: local holding the index */
    struct IrPlace *base;
    Type *ty;
} IrPlace;

typedef enum {
    IR_CONST_INT,
    IR_CONST_BOOL,
    IR_CONST_STR,
    IR_CONST_FLOAT,
    IR_LOAD,       /* dst = *place */
    IR_STORE,      /* place = a */
    IR_ADDR,       /* dst = &place (is_mut) */
    IR_MOVE,       /* dst = a, then a is dead (box transfer) */
    IR_BIN,        /* dst = a <binop> b */
    IR_UN,         /* dst = <unop> a */
    IR_CALL,       /* dst = callee(args...) */
    IR_CALL_VAL,   /* dst = a(args...) — indirect */
    IR_ALLOC,      /* dst = box of elem_ty */
    IR_DROP,       /* free local a if non-null */
    IR_BOUND,      /* trap unless 0 <= a < imm (or local b if b >= 0) */
    IR_STRUCT_LIT, /* dst = { args... } in field order */
    IR_ARRAY_LIT,
    IR_FN_VAL,     /* dst = (fn, NULL) fat pointer for a named function */
    IR_CLOS,       /* dst = { callee, env }; args = capture values (heap env) */
    IR_CAST,       /* dst = (ty) a */
    IR_SIZEOF,     /* dst = sizeof(ty) in bytes (Phase 11 bench) */
    IR_PANIC,      /* trap with local a (a string); source line from loc */
} IrOp;

typedef struct {
    IrOp op;
    int dst;            /* local id, -1 when none */
    int a, b;           /* operand local ids, -1 when unused */
    IrPlace *place;
    int64_t imm;
    double fimm;
    const char *str;
    const char *callee;
    int *args;
    int nargs;
    int binop;          /* AstBinOp for IR_BIN / IR_UN */
    int is_mut;         /* IR_ADDR */
    int conv_mode;      /* IR_CAST: 0 = trap, 1 = wrapping, 2 = saturating */
    int checked;        /* IR_BIN: overflow/div trap required */
    Type *ty;
    SourceLoc loc;
} IrInst;

typedef enum {
    IR_TERM_RET,
    IR_TERM_JMP,
    IR_TERM_BR,
    IR_TERM_UNREACHABLE,
} IrTermKind;

typedef struct {
    int id;
    IrInst *insts;
    int ninsts, cap;
    IrTermKind term;
    int term_val;       /* return value / branch condition; -1 for none */
    int succ[2];        /* JMP uses succ[0]; BR uses both */
    SourceLoc term_loc;
} IrBlock;

typedef struct {
    int id;
    Type *ty;
    const char *name;   /* NULL for compiler temps */
    int is_param;
    int is_global;
    const char *cname;  /* C symbol when is_global */
    int needs_drop;     /* owns a box: dropped on every exit path */
} IrLocal;

typedef struct {
    const char *cname;
    const char *name;
    Type *sig;
    IrLocal *locals;
    int nlocals, lcap;
    IrBlock *blocks;
    int nblocks, bcap;
    int nparams;
    int is_main;
    int lowered;        /* 0 when a construct was not representable yet */
    int clos_id;        /* non-zero: this IrFn is loam_clos_<id> */
    int env_local;      /* closure: the local holding `_env`; -1 otherwise */
    const char **caps;
    Type **cap_types;
    int ncaps;
} IrFn;

typedef struct {
    IrFn *fns;
    int nfns, cap;
} IrModule;

IrModule *ir_lower(LoamModule *mods, int nmods);
void ir_print(FILE *out, const IrModule *m);
int ir_verify(const IrModule *m);
void ir_free(IrModule *m);

#endif
