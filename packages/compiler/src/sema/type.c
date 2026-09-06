/**
 * type.c — interned Yuga types.
 *
 * void/int/float/bool/string are static singletons. Everything else is calloc'd
 * into a pool and released by type_pool_reset at the end of a compile.
 */
#include "type.h"
#include "../diagnostics.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Type t_void = {TY_VOID, 0, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 0};
static Type t_int = {TY_INT, 0, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 0};
static Type t_float = {TY_FLOAT, 0, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 0};
static Type t_bool = {TY_BOOL, 0, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 0};
static Type t_string = {TY_STRING, 0, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, 0};

#define POOL_MAX 4096
static Type *pool[POOL_MAX];
static int npool;

Type *ty_void(void) { return &t_void; }
Type *ty_int(void) { return &t_int; }
Type *ty_float(void) { return &t_float; }
Type *ty_bool(void) { return &t_bool; }
Type *ty_string(void) { return &t_string; }

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
    t->ret = ret ? ret : &t_void;
    return t;
}

Type *type_param(const char *name) {
    Type *t = type_new(TY_PARAM);
    t->name = yuga_dup(name);
    return t;
}

/** Structural equality. Named structs compare by name only. */
int type_eq(const Type *a, const Type *b) {
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
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
            if (!type_eq(a->ret ? a->ret : &t_void, b->ret ? b->ret : &t_void)) return 0;
            for (size_t i = 0; i < a->param_count; i++)
                if (!type_eq(a->params[i], b->params[i])) return 0;
            return 1;
        case TY_PARAM:
            return a->name && b->name && strcmp(a->name, b->name) == 0;
        default:
            return 1;
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
   no borrows. Strings are Send: immutable and never freed (yuga_rt.h), so
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
    if (t->kind == TY_ARRAY) return type_needs_drop(t->elem);
    if (t->kind == TY_STRUCT) {
        for (size_t i = 0; i < t->field_count; i++)
            if (type_needs_drop(t->field_types[i])) return 1;
        return 0;
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
        case TY_INT: return "int";
        case TY_FLOAT: return "float";
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
