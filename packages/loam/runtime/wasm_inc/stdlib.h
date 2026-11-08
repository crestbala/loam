#ifndef LOAM_WASM_STDLIB_H
#define LOAM_WASM_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void *realloc(void *p, size_t n);
void *calloc(size_t n, size_t sz);
void free(void *p);
void abort(void);
void exit(int c);
char *getenv(const char *name);
int atoi(const char *s);
/* Exact, round-to-nearest-even (zeus_wasm_libc.c). */
double strtod(const char *s, char **end);
#endif
