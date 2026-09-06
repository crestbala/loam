/**
 * type.h — Yuga types used by typecheck, borrowck, and codegen.
 *
 * Singletons for void/int/float/bool/string. Compound types come from type_new
 * and live in a pool until type_pool_reset (end of a compile).
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
    const char *name;     /* struct name or type-param name */
    Type *elem;           /* ptr/box/array/vec element */
    Type *ret;            /* proc return */
    size_t param_count;   /* proc params, or generic struct type args */
    Type **params;
    size_t field_count;
    const char **field_names;
    Type **field_types;
    int64_t array_len;
};

Type *type_new(TypeKind k);
Type *type_ptr(Type *elem, int is_mut);
Type *type_box(Type *elem);
Type *type_array(Type *elem, int64_t n);
Type *type_vec(Type *elem);
Type *type_proc(Type **params, size_t n, Type *ret);
Type *type_param(const char *name);

/** Structural equality. Generic structs compare name + type arguments. */
int type_eq(const Type *a, const Type *b);

/** 1 if the value is copied on use (not moved). Box and &mut are not Copy.
 *  []T is Copy when T is Copy (refcounted header). fn values are Copy handles. */
int type_is_copy(const Type *t);

/** 1 if a local of this type must run a destructor (Box, []T, or a struct/array of those). */
int type_needs_drop(const Type *t);

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
Type *ty_int(void);
Type *ty_float(void);
Type *ty_bool(void);
Type *ty_string(void);

#endif
