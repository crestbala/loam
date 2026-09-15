/**
 * diagnostics.h — source locations, compile errors, and string copies.
 *
 * `yuga_error` either prints immediately or is captured (LSP). `yuga_fatal`
 * is for allocator failure: print and exit.
 */
#ifndef YUGA_DIAGNOSTICS_H
#define YUGA_DIAGNOSTICS_H

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

/** 1-based line/column in `file` (may be NULL). `end_*` is the first
 *  character after the span (also 1-based); 0 means “one character”. */
typedef struct {
    const char *file;
    int line;
    int col;
    int end_line;
    int end_col;
} SourceLoc;

/** Heap-owned diagnostic; file and msg are malloc'd. */
typedef struct {
    char *file;
    int line;
    int col;
    int end_line;
    int end_col;
    char *msg;
} YugaDiag;

/** Report a compile error at `loc`. Captured if yuga_diag_capture(1). */
void yuga_error(SourceLoc loc, const char *fmt, ...);

/** Unrecoverable failure (typically OOM). Prints to stderr and exits. */
void yuga_fatal(const char *fmt, ...);

/** malloc a copy of `n` bytes of `s` plus NUL. */
char *yuga_dupn(const char *s, size_t n);

/** malloc a copy of `s`. NULL in, NULL out. */
char *yuga_dup(const char *s);

/** When enable != 0, yuga_error stores instead of printing. */
void yuga_diag_capture(int enable);

/** 1 if yuga_error is capturing (LSP). Incomplete `.` is recovered then. */
int yuga_diag_capturing(void);

/** Free the capture buffer without taking it. */
void yuga_diag_clear(void);

int yuga_diag_count(void);

/** Steal the capture buffer; caller must yuga_diag_free. */
void yuga_diag_take(YugaDiag **out, int *n);

void yuga_diag_free(YugaDiag *d, int n);

#endif
