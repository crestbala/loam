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
