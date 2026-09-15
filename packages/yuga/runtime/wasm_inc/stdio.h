#ifndef YUGA_WASM_STDIO_H
#define YUGA_WASM_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct YugaFile {
    int fd;
} FILE;
extern FILE *stdout;
extern FILE *stderr;
int fprintf(FILE *f, const char *fmt, ...);
int fflush(FILE *f);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int printf(const char *fmt, ...);
#endif
