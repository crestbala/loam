/**
 * diagnostics.c — error reporting and interned string copies.
 *
 * Capture mode (yuga_diag_capture) is for the LSP: errors are stored and
 * later taken as YugaDiag records instead of printed.
 */
#include "diagnostics.h"
#include <stdarg.h>
#include <string.h>

static int capturing;
static YugaDiag *cap;
static int ncap, cap_max;

/** When enable != 0, yuga_error appends to the capture list. */
void yuga_diag_capture(int enable) { capturing = enable; }

int yuga_diag_capturing(void) { return capturing; }

void yuga_diag_clear(void) {
    yuga_diag_free(cap, ncap);
    cap = NULL;
    ncap = 0;
    cap_max = 0;
}

int yuga_diag_count(void) { return ncap; }

void yuga_diag_take(YugaDiag **out, int *n) {
    *out = cap;
    *n = ncap;
    cap = NULL;
    ncap = 0;
    cap_max = 0;
}

void yuga_diag_free(YugaDiag *d, int n) {
    if (!d) return;
    for (int i = 0; i < n; i++) {
        free(d[i].file);
        free(d[i].msg);
    }
    free(d);
}

/** Append one diagnostic to the capture buffer. */
static void capture_one(SourceLoc loc, const char *msg) {
    if (ncap >= cap_max) {
        cap_max = cap_max ? cap_max * 2 : 16;
        cap = (YugaDiag *)realloc(cap, (size_t)cap_max * sizeof(YugaDiag));
    }
    cap[ncap].file = yuga_dup(loc.file ? loc.file : "");
    cap[ncap].line = loc.line;
    cap[ncap].col = loc.col;
    cap[ncap].end_line = loc.end_line;
    cap[ncap].end_col = loc.end_col;
    cap[ncap].msg = yuga_dup(msg);
    ncap++;
}

/** Format and report an error; print or capture depending on mode. */
void yuga_error(SourceLoc loc, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (capturing) {
        capture_one(loc, buf);
    } else {
        fprintf(stderr, "%s:%d:%d: error: %s\n",
                loc.file ? loc.file : "<unknown>", loc.line, loc.col, buf);
    }
}

/** Print to stderr and exit(1). */
void yuga_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/** Heap copy of `n` bytes plus NUL. */
char *yuga_dupn(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) yuga_fatal("out of memory");
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/** Heap copy of a C string. */
char *yuga_dup(const char *s) {
    if (!s) return NULL;
    return yuga_dupn(s, strlen(s));
}
