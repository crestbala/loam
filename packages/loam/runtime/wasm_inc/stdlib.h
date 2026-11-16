#ifndef LOAM_WASM_STDLIB_H
#define LOAM_WASM_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void *realloc(void *p, size_t n);
void free(void *p);
__attribute__((noreturn)) void abort(void);
#endif
