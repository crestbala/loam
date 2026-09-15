/**
 * diagnostics.h — source locations, compile errors, and string copies.
 *
 * `loam_error` either prints immediately or is captured (LSP). `loam_fatal`
 * is for allocator failure: print and exit.
 */
#ifndef LOAM_DIAGNOSTICS_H
#define LOAM_DIAGNOSTICS_H

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
} LoamDiag;

/** Report a compile error at `loc`. Captured if loam_diag_capture(1). */
void loam_error(SourceLoc loc, const char *fmt, ...);

/** Unrecoverable failure (typically OOM). Prints to stderr and exits. */
void loam_fatal(const char *fmt, ...);

/** malloc a copy of `n` bytes of `s` plus NUL. */
char *loam_dupn(const char *s, size_t n);

/** malloc a copy of `s`. NULL in, NULL out. */
char *loam_dup(const char *s);

/** When enable != 0, loam_error stores instead of printing. */
void loam_diag_capture(int enable);

/** 1 if loam_error is capturing (LSP). Incomplete `.` is recovered then. */
int loam_diag_capturing(void);

/** Free the capture buffer without taking it. */
void loam_diag_clear(void);

int loam_diag_count(void);

/** Steal the capture buffer; caller must loam_diag_free. */
void loam_diag_take(LoamDiag **out, int *n);

void loam_diag_free(LoamDiag *d, int n);

#endif
