/**
 * type.h — Yuga types used by typecheck, borrowck, and codegen.
 *
 * Numeric scalars are static singletons keyed by (kind, bits, is_unsigned):
 * i8..i64, u8..u64, f32/f64. `int` is the i64 singleton and `float` the f64
 * one during the additive phase. Compound types come from type_new and live in
 * a pool until type_pool_reset (end of a compile).
 */
#ifndef YUGA_TYPE_H
#define YUGA_TYPE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TY_UNKNOWN = 0,
    TY_VOID,
    TY_INT,
    TY_FLOAT,
    TY_BOOL,
    TY_STRING,
    TY_PTR,     /* &T or &mut T (`is_mut`) */
    TY_BOX,
    TY_STRUCT,
    TY_PROC,    /* fn(T, U) -> R */
    TY_ARRAY,   /* [N]T */
    TY_VEC,     /* []T — owning growable array */
    TY_PARAM,   /* generic T on a fn or struct */
} TypeKind;

typedef struct Type Type;

struct Type {
    TypeKind kind;
    int is_mut;           /* TY_PTR: 1 = &mut T */
    unsigned char bits;   /* TY_INT: 8/16/32/64; TY_FLOAT: 32/64 */
    unsigned char is_unsigned; /* TY_INT: 1 = u8/u16/u32/u64 */
    const char *name;     /* struct name or type-param name */
    Type *elem;           /* ptr/box/array/vec element */
    Type *ret;            /* proc return */
    size_t param_count;   /* proc params, or generic struct type args */
    Type **params;
    size_t field_count;
    const char **field_names;
    Type **field_types;
    int must_check; /* TY_STRUCT: `#[must_check]` — see borrowck */
    int64_t array_len;
};

Type *type_new(TypeKind k);
Type *type_ptr(Type *elem, int is_mut);
Type *type_box(Type *elem);
Type *type_array(Type *elem, int64_t n);
Type *type_vec(Type *elem);
Type *type_proc(Type **params, size_t n, Type *ret);
Type *type_param(const char *name);

/** Singleton scalar by width: ty_int_bits(64,0) is `int`. */
Type *ty_int_bits(int bits, int is_unsigned);
Type *ty_float_bits(int bits);

/** Structural equality. Generic structs compare name + type arguments.
 *  Numeric scalars compare kind + bits + signedness. */
int type_eq(const Type *a, const Type *b);

/** 1 if the value is copied on use (not moved). Box and &mut are not Copy.
 *  []T is Copy when T is Copy (refcounted header). fn values are Copy handles. */
int type_is_copy(const Type *t);

/** 1 if a local of this type must run a destructor (Box, []T, or a struct/array of those). */
int type_needs_drop(const Type *t);

/** 1 if every heap handle a call argument of this type carries is *adopted* by
 *  the callee, so the caller must not drop its own binding.
 *
 *  The codegen keeps a `[]T` alive across a call by retaining each `[]T` leaf
 *  the argument reaches (see `emit_nested_keeps`), which leaves that reference
 *  with the caller; a `Box`/`fn` leaf is passed through untouched, so the
 *  callee's own parameter drop is what frees it. The two halves have to agree
 *  with the drop insertion in ir.c, or an argument either leaks its reference
 *  or is released twice, which is why both read this predicate. */
int type_arg_transfers(const Type *t);

/** 1 if a value of this type may cross an OS-thread boundary (channel<T>
 *  payloads, thread.spawn captures). Plain data only: scalars, immutable
 *  strings, and structs/arrays of Send fields. Not Send: []T (non-atomic
 *  refcount), fn values (C-seam/global reachability is unchecked), Box, and
 *  any borrow — sharing those with a worker thread races the UI thread. */
int type_is_send(const Type *t);

/** C identifier for a struct type (`Pair__int` for `Pair<int>`). */
void type_c_name(const Type *t, char *buf, size_t cap);

/**
 * 1 if a value of this type can carry a fn after a call returns
 * (fn, Box, pointer, struct, array of those). Used to keep capturing
 * closures from escaping the function that created them.
 */
int type_can_hold_fn(const Type *t);

/** Diagnostic name (`"&mut int"`, `"fn(int) -> bool"`, …). Not thread-safe. */
const char *type_name(const Type *t);

/** Free pooled compound types. Call at end of a compile. */
void type_pool_reset(void);

Type *ty_void(void);
Type *ty_int(void);   /* `int` alias: i32 by default, i64 under --int64-compat */
Type *ty_float(void); /* `float` alias: f32 by default, f64 under --int64-compat */
Type *ty_bool(void);
Type *ty_string(void);

/** Phase 10: when on, `int` means i64 and `float` means f64 (the old default).
 *  Set once, before any type is resolved. */
void type_set_int64_compat(int on);
int type_int64_compat(void);

/** 1 if t is any integer width (signed or unsigned). */
int type_is_int_kind(const Type *t);
/** 1 if t is f32 or f64. */
int type_is_float_kind(const Type *t);
/** 1 if t is any integer or float width. */
int type_is_numeric(const Type *t);

/** Suffix naming an integer width in a runtime helper (`i8`..`u64`), NULL if
 *  t is not an integer width. */
const char *type_int_suffix(const Type *t);

/** Numeric builtins whose width comes from the operand type (§3.4). */
typedef enum {
    NUMB_NONE = 0,
    NUMB_WRAP_ADD,
    NUMB_WRAP_SUB,
    NUMB_WRAP_MUL,
    NUMB_WRAP_NEG,
    NUMB_WRAP_SHL,
    NUMB_WRAP_SHR,
    NUMB_WRAP_AND,
    NUMB_WRAP_OR,
    NUMB_WRAP_XOR,
    NUMB_SAT_ADD,
    NUMB_SAT_SUB,
    NUMB_SAT_MUL,
} NumericBuiltin;

/** Runtime helper name for `b` at width `t` (`yuga_wrapping_add_u8`), in a
 *  rotating static buffer. */
const char *numeric_builtin_cname(NumericBuiltin b, const Type *t);

/** C scalar name for a numeric type (`int32_t`, `uint8_t`, `float`, `double`). */
const char *type_c_scalar(const Type *t);

#endif
