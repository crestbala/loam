// Vendored C for compile_pass/c_ffi.loam, built with -DC_FFI_BASE=10.
#include "c_ffi.h"
#include <string.h>

int32_t c_ffi_add(int32_t a, int32_t b) { return C_FFI_BASE + a + b; }

// An opaque handle: the shape of sqlite3_open's out parameter.
void c_ffi_open(const char *name, int64_t *handle) { *handle = (int64_t)strlen(name) * 2; }

const char *c_ffi_tail(const char *text) { return text[0] ? text + 1 : NULL; }

double c_ffi_half(double x) { return x / 2; }

float c_ffi_quarter(float x) { return x / 4; }

_Bool c_ffi_even(int64_t n) { return n % 2 == 0; }

// Bytes and a length: the shape of sqlite3_bind_text.
int64_t c_ffi_sum(const uint8_t *bytes, int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) sum += bytes[i];
    return sum;
}

// A pointer into C memory with NULs inside: the shape of sqlite3_column_text.
const char *c_ffi_blob(void) { return "ab\0cd"; }

int64_t c_ffi_addr(const uint8_t *bytes) { return (int64_t)(intptr_t)bytes; }

static int calls;

void c_ffi_tick(void) { calls++; }

int32_t c_ffi_calls(void) { return calls; }

// Plain-data structs by value: the shape of raylib's `Rectangle` and `Color`.
// Fields of mixed width, a bool, and a nested struct, so a wrong layout or a
// struct passed by pointer gives a wrong answer.
typedef struct { float x, y, width, height; } CFfiRect;
typedef struct { uint8_t r, g, b, a; } CFfiRgba;
typedef struct { CFfiRect rect; CFfiRgba tint; _Bool on; int64_t id; double weight; } CFfiItem;

double c_ffi_area(CFfiRect r) { return (double)r.width * r.height + r.x - r.y; }

int64_t c_ffi_rgba(CFfiRgba c) { return ((int64_t)c.r << 24) | (c.g << 16) | (c.b << 8) | c.a; }

int64_t c_ffi_item(int32_t base, CFfiItem item, int32_t after) {
    return base + (int64_t)c_ffi_area(item.rect) + c_ffi_rgba(item.tint) % 1000 + (item.on ? 100 : 0) +
           item.id + (int64_t)(item.weight * 4) + after;
}

// A `bool *` C writes in place: the shape of raygui's `GuiCheckBox`.
int32_t c_ffi_flip(_Bool *flags, int32_t n) {
    int32_t set = 0;
    for (int32_t i = 0; i < n; i++) {
        flags[i] = !flags[i];
        set += flags[i];
    }
    return set;
}
