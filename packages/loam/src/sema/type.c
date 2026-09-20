/**
 * type.c — interned Loam types.
 *
 * Numeric scalars are static singletons keyed by (kind, bits, is_unsigned).
 * Everything else is calloc'd into a pool released by type_pool_reset.
 */
#include "type.h"
#include "../diagnostics.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* kind, is_mut, bits, is_unsigned, name, elem, ret, param_count, params,
   field_count, field_names, field_types, must_check, array_len */
#define SCALAR(K, B, U) {K, 0, B, U, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 0, 0}

static Type t_void = SCALAR(TY_VOID, 0, 0);
static Type t_i8 = SCALAR(TY_INT, 8, 0);
static Type t_i16 = SCALAR(TY_INT, 16, 0);
static Type t_i32 = SCALAR(TY_INT, 32, 0);
static Type t_i64 = SCALAR(TY_INT, 64, 0);
static Type t_u8 = SCALAR(TY_INT, 8, 1);
static Type t_u16 = SCALAR(TY_INT, 16, 1);
static Type t_u32 = SCALAR(TY_INT, 32, 1);
static Type t_u64 = SCALAR(TY_INT, 64, 1);
static Type t_f32 = SCALAR(TY_FLOAT, 32, 0);
static Type t_f64 = SCALAR(TY_FLOAT, 64, 0);
static Type t_bool = SCALAR(TY_BOOL, 0, 0);
static Type t_string = SCALAR(TY_STRING, 0, 0);
#undef SCALAR

#define POOL_MAX 4096
static Type *pool[POOL_MAX];
static int npool;

/** Phase 10: 0 (default) = `int`/`float` are 32-bit; 1 = i64/f64 for compat. */
static int g_int64_compat = 0;

void type_set_int64_compat(int on) { g_int64_compat = on ? 1 : 0; }
int type_int64_compat(void) { return g_int64_compat; }

/** 0 (default) = `string` keeps the old Copy, never-freed behavior: every
 *  `{{ }}`/`str_of_*` allocation lives for the process. 1 = `string` is an
 *  owned, move-only value (Rust `String`): it is dropped at the end of its
 *  binding, a callee adopts a by-value `string`, and a read-only parameter is
 *  `&string`. Gated so the tree migrates to borrow-by-value signatures a file
 *  at a time (see `--string-owns`), mirroring the Phase 10 numeric flip. */
static int g_string_owns = 0;

void type_set_string_owns(int on) { g_string_owns = on ? 1 : 0; }
int type_string_owns(void) { return g_string_owns; }

Type *ty_void(void) { return &t_void; }
Type *ty_int(void) { return g_int64_compat ? &t_i64 : &t_i32; }
Type *ty_float(void) { return g_int64_compat ? &t_f64 : &t_f32; }
Type *ty_bool(void) { return &t_bool; }
Type *ty_string(void) { return &t_string; }

/** Static singleton for an integer width. Unknown widths fall back to i64. */
Type *ty_int_bits(int bits, int is_unsigned) {
    switch (bits * 2 + (is_unsigned ? 1 : 0)) {
        case 16: return &t_i8;
        case 32: return &t_i16;
        case 64: return &t_i32;
        case 128: return &t_i64;
        case 17: return &t_u8;
        case 33: return &t_u16;
        case 65: return &t_u32;
        case 129: return &t_u64;
        default: return &t_i64;
    }
}

Type *ty_float_bits(int bits) { return bits == 32 ? &t_f32 : &t_f64; }

/** Fresh pooled type of kind `k` (zeroed). */
Type *type_new(TypeKind k) {
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) return NULL;
    t->kind = k;
    if (npool < POOL_MAX) pool[npool++] = t;
    return t;
}

Type *type_ptr(Type *elem, int is_mut) {
    Type *t = type_new(TY_PTR);
    t->elem = elem;
    t->is_mut = is_mut;
    return t;
}

Type *type_box(Type *elem) {
    Type *t = type_new(TY_BOX);
    t->elem = elem;
    return t;
}

Type *type_array(Type *elem, int64_t n) {
    Type *t = type_new(TY_ARRAY);
    t->elem = elem;
    t->array_len = n;
    return t;
}

Type *type_vec(Type *elem) {
    Type *t = type_new(TY_VEC);
    t->elem = elem;
    return t;
}

Type *type_proc(Type **params, size_t n, Type *ret) {
    Type *t = type_new(TY_PROC);
    t->params = params;
    t->param_count = n;
    t->ret = ret ? ret : ty_void();
    return t;
}

Type *type_param(const char *name) {
    Type *t = type_new(TY_PARAM);
    t->name = loam_dup(name);
    return t;
}

/** Structural equality. Named structs compare by name only. */
int type_eq(const Type *a, const Type *b) {
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
        case TY_INT:
            return a->bits == b->bits && a->is_unsigned == b->is_unsigned;
        case TY_FLOAT:
            return a->bits == b->bits;
        case TY_PTR:
            return a->is_mut == b->is_mut && type_eq(a->elem, b->elem);
        case TY_BOX:
            return type_eq(a->elem, b->elem);
        case TY_ARRAY:
            return a->array_len == b->array_len && type_eq(a->elem, b->elem);
        case TY_VEC:
            return type_eq(a->elem, b->elem);
        case TY_STRUCT:
            if (!a->name || !b->name || strcmp(a->name, b->name) != 0) return 0;
            if (a->param_count != b->param_count) return 0;
            for (size_t i = 0; i < a->param_count; i++)
                if (!type_eq(a->params[i], b->params[i])) return 0;
            return 1;
        case TY_PROC:
            if (a->param_count != b->param_count) return 0;
            if (!type_eq(a->ret ? a->ret : ty_void(), b->ret ? b->ret : ty_void())) return 0;
            for (size_t i = 0; i < a->param_count; i++)
                if (!type_eq(a->params[i], b->params[i])) return 0;
            return 1;
        case TY_PARAM:
            return a->name && b->name && strcmp(a->name, b->name) == 0;
        default:
            return 1;
    }
}

int type_is_int_kind(const Type *t) { return t && t->kind == TY_INT; }
int type_is_float_kind(const Type *t) { return t && t->kind == TY_FLOAT; }
int type_is_numeric(const Type *t) { return t && (t->kind == TY_INT || t->kind == TY_FLOAT); }

const char *type_int_suffix(const Type *t) {
    if (!t || t->kind != TY_INT) return NULL;
    switch (t->bits * 2 + (t->is_unsigned ? 1 : 0)) {
        case 16: return "i8";
        case 32: return "i16";
        case 64: return "i32";
        case 128: return "i64";
        case 17: return "u8";
        case 33: return "u16";
        case 65: return "u32";
        case 129: return "u64";
        default: return "i64";
    }
}

const char *numeric_builtin_cname(NumericBuiltin b, const Type *t) {
    static char bufs[4][48];
    static int rot;
    char *buf = bufs[rot++ & 3];
    const char *base = NULL;
    switch (b) {
        case NUMB_WRAP_ADD: base = "wrapping_add"; break;
        case NUMB_WRAP_SUB: base = "wrapping_sub"; break;
        case NUMB_WRAP_MUL: base = "wrapping_mul"; break;
        case NUMB_WRAP_NEG: base = "wrapping_neg"; break;
        case NUMB_WRAP_SHL: base = "wrapping_shl"; break;
        case NUMB_WRAP_SHR: base = "wrapping_shr"; break;
        case NUMB_WRAP_AND: base = "wrapping_and"; break;
        case NUMB_WRAP_OR: base = "wrapping_or"; break;
        case NUMB_WRAP_XOR: base = "wrapping_xor"; break;
        case NUMB_SAT_ADD: base = "saturating_add"; break;
        case NUMB_SAT_SUB: base = "saturating_sub"; break;
        case NUMB_SAT_MUL: base = "saturating_mul"; break;
        default: base = "wrapping_add"; break;
    }
    snprintf(buf, 48, "loam_%s_%s", base, type_int_suffix(t) ? type_int_suffix(t) : "i64");
    return buf;
}

/** C scalar name (int32_t, uint8_t, float, double). NULL for non-numeric. */
const char *type_c_scalar(const Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_FLOAT) return t->bits == 32 ? "float" : "double";
    if (t->kind != TY_INT) return NULL;
    switch (t->bits * 2 + (t->is_unsigned ? 1 : 0)) {
        case 16: return "int8_t";
        case 32: return "int16_t";
        case 64: return "int32_t";
        case 128: return "int64_t";
        case 17: return "uint8_t";
        case 33: return "uint16_t";
        case 65: return "uint32_t";
        case 129: return "uint64_t";
        default: return "int64_t";
    }
}

/** 1 if uses copy the value (int, bool, string, fn, &T, Copy structs). */
int type_is_copy(const Type *t) {
    if (!t) return 0;
    if (t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_BOOL || t->kind == TY_STRING ||
        t->kind == TY_VOID)
        return 1;
    if (t->kind == TY_PARAM) return 1;
    /* fn values are Copy handles (shared env). */
    if (t->kind == TY_PROC) return 1;
    if (t->kind == TY_PTR && !t->is_mut) return 1;
    if (t->kind == TY_ARRAY) return type_is_copy(t->elem);
    if (t->kind == TY_VEC) return type_is_copy(t->elem);
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++)
            if (!type_is_copy(t->field_types[i])) return 0;
        return 1;
    }
    return 0;
}

/* Send = Copy minus shared-mutable escape: no heap handles ([]T, fn, Box),
   no borrows. Strings are Send: immutable and never freed (loam_rt.h), so
   sharing one with a worker is a read-only alias. Structs recurse. */
int type_is_send(const Type *t) {
    if (!t) return 0;
    if (t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_BOOL || t->kind == TY_STRING ||
        t->kind == TY_VOID)
        return 1;
    if (t->kind == TY_PARAM) return 1; /* concrete at the check sites */
    if (t->kind == TY_ARRAY) return type_is_send(t->elem);
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++)
            if (!type_is_send(t->field_types[i])) return 0;
        return 1;
    }
    return 0; /* PTR, BOX, PROC, VEC */
}

/** 1 if this type can carry a fn out of a call (escape of capturing clos). */
int type_can_hold_fn(const Type *t) {
    if (!t) return 1;
    switch (t->kind) {
        case TY_PROC:
        case TY_BOX:
        case TY_PTR:
        case TY_STRUCT:
        case TY_PARAM:
        case TY_UNKNOWN:
            return 1;
        case TY_ARRAY:
        case TY_VEC:
            return type_can_hold_fn(t->elem);
        default:
            return 0;
    }
}

int type_needs_drop(const Type *t) {
    if (!t) return 0;
    if (t->kind == TY_BOX || t->kind == TY_VEC) return 1;
    if (t->kind == TY_STRING) return g_string_owns ? 1 : 0;
    if (t->kind == TY_ARRAY) return type_needs_drop(t->elem);
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++)
            if (type_needs_drop(t->field_types[i])) return 1;
        return 0;
    }
    return 0;
}

int type_owns_string(const Type *t) {
    if (!t) return 0;
    if (t->kind == TY_STRING) return 1;
    if (t->kind == TY_ARRAY) return type_owns_string(t->elem);
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++)
            if (type_owns_string(t->field_types[i])) return 1;
        return 0;
    }
    return 0;
}

int type_arg_transfers(const Type *t) {
    if (!t) return 0;
    /* Passed through as-is: the callee's parameter drop frees it. */
    if (t->kind == TY_BOX || t->kind == TY_PROC) return 1;
    /* Retained at the call site, so the caller still owns a reference. */
    if (t->kind == TY_VEC) return 0;
    /* An owned string is a refcounted Copy value like []T: the caller keeps
       its reference and the argument is retained across the call. */
    if (t->kind == TY_STRING) return 0;
    if (t->kind == TY_ARRAY) return type_arg_transfers(t->elem);
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++) {
            if (!type_needs_drop(t->field_types[i])) continue;
            /* One leaf that stays with the caller is enough: the caller has to
               run the destructor for that leaf, and the codegen nulls the
               leaves the callee adopted before it does. */
            if (!type_arg_transfers(t->field_types[i])) return 0;
        }
        return 1;
    }
    return 0;
}

void type_c_name(const Type *t, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (!t || t->kind != TY_STRUCT) {
        snprintf(buf, cap, "int64_t");
        return;
    }
    snprintf(buf, cap, "%s", t->name ? t->name : "struct");
    /* Signal<T> / Future<T> / Chan<T> are typed handles; the C layout is
       always { id }. */
    if (t->name && (strcmp(t->name, "Signal") == 0 || strcmp(t->name, "Future") == 0 ||
                    strcmp(t->name, "Chan") == 0))
        return;
    for (size_t i = 0; i < t->param_count; i++) {
        size_t used = strlen(buf);
        if (used + 3 >= cap) break;
        snprintf(buf + used, cap - used, "__");
        used = strlen(buf);
        const char *nm = type_name(t->params[i]);
        for (const char *p = nm; *p && used + 1 < cap; p++) {
            char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                buf[used++] = c;
            else if (used && buf[used - 1] != '_')
                buf[used++] = '_';
        }
        buf[used] = '\0';
    }
}

/** Rotating buffer: not safe to hold across another type_name call. */
const char *type_name(const Type *t) {
    static char bufs[8][160];
    static int rot;
    char *buf = bufs[rot++ & 7];
    if (!t) return "<unknown>";
    switch (t->kind) {
        case TY_VOID: return "void";
        case TY_INT:
            switch (t->bits * 2 + (t->is_unsigned ? 1 : 0)) {
                case 16: return "i8";
                case 32: return "i16";
                case 64: return g_int64_compat ? "i32" : "int";
                case 128: return g_int64_compat ? "int" : "i64";
                case 17: return "u8";
                case 33: return "u16";
                case 65: return "u32";
                case 129: return "u64";
                default: return g_int64_compat ? "int" : "i32";
            }
        case TY_FLOAT:
            if (t->bits == 32) return g_int64_compat ? "f32" : "float";
            return g_int64_compat ? "float" : "f64";
        case TY_BOOL: return "bool";
        case TY_STRING: return "string";
        case TY_PTR:
            snprintf(buf, 160, "&%s%s", t->is_mut ? "mut " : "", type_name(t->elem));
            return buf;
        case TY_BOX:
            snprintf(buf, 160, "Box<%s>", type_name(t->elem));
            return buf;
        case TY_ARRAY:
            snprintf(buf, 160, "[%lld]%s", (long long)t->array_len, type_name(t->elem));
            return buf;
        case TY_VEC:
            snprintf(buf, 160, "[]%s", type_name(t->elem));
            return buf;
        case TY_STRUCT:
            if (!t->param_count) return t->name ? t->name : "struct";
            {
                size_t used = (size_t)snprintf(buf, 160, "%s<", t->name ? t->name : "struct");
                for (size_t i = 0; i < t->param_count && used < 150; i++)
                    used += (size_t)snprintf(buf + used, 160 - used, "%s%s", i ? ", " : "",
                                             type_name(t->params[i]));
                snprintf(buf + used, 160 - used, ">");
                return buf;
            }
        case TY_PARAM:
            return t->name ? t->name : "T";
        case TY_PROC: {
            size_t used = 0;
            used += (size_t)snprintf(buf, 160, "fn(");
            for (size_t i = 0; i < t->param_count && used < 150; i++) {
                used += (size_t)snprintf(buf + used, 160 - used, "%s%s",
                                         i ? ", " : "", type_name(t->params[i]));
            }
            if (t->ret && t->ret->kind != TY_VOID) {
                const char *r = type_name(t->ret);
                snprintf(buf + used, 160 - used, ") -> %s", r);
            } else {
                snprintf(buf + used, 160 - used, ")");
            }
            return buf;
        }
        default:
            return "<unknown>";
    }
}

/** Free every type_new allocation from this compile. */
void type_pool_reset(void) {
    for (int i = 0; i < npool; i++) {
        if (pool[i]->field_names) {
            for (size_t f = 0; f < pool[i]->field_count; f++)
                free((void *)pool[i]->field_names[f]);
        }
        free((void *)pool[i]->name);
        free(pool[i]->params);
        free(pool[i]->field_names);
        free(pool[i]->field_types);
        free(pool[i]);
        pool[i] = NULL;
    }
    npool = 0;
}
