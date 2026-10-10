/**
 * diagnostics.c — error reporting and interned string copies.
 *
 * Capture mode (loam_diag_capture) is for the LSP: errors are stored and
 * later taken as LoamDiag records instead of printed.
 */
#include "diagnostics.h"
#include <stdarg.h>
#include <string.h>

static int capturing;
static LoamDiag *cap;
static int ncap, cap_max;
/* Warnings, kept apart so a capture-mode build does not count them as errors. */
static LoamDiag *wcap;
static int nwcap, wcap_max;

/** When enable != 0, loam_error appends to the capture list. */
void loam_diag_capture(int enable) { capturing = enable; }

int loam_diag_capturing(void) { return capturing; }

void loam_diag_clear(void) {
    loam_diag_free(cap, ncap);
    cap = NULL;
    ncap = 0;
    cap_max = 0;
}

int loam_diag_count(void) { return ncap; }

void loam_diag_take(LoamDiag **out, int *n) {
    *out = cap;
    *n = ncap;
    cap = NULL;
    ncap = 0;
    cap_max = 0;
}

void loam_diag_free(LoamDiag *d, int n) {
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
        cap = (LoamDiag *)realloc(cap, (size_t)cap_max * sizeof(LoamDiag));
    }
    cap[ncap].file = loam_dup(loc.file ? loc.file : "");
    cap[ncap].line = loc.line;
    cap[ncap].col = loc.col;
    cap[ncap].end_line = loc.end_line;
    cap[ncap].end_col = loc.end_col;
    cap[ncap].msg = loam_dup(msg);
    ncap++;
}

/** Format and report an error; print or capture depending on mode. */
void loam_error(SourceLoc loc, const char *fmt, ...) {
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

void loam_warning(SourceLoc loc, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!capturing) {
        fprintf(stderr, "%s:%d:%d: warning: %s\n",
                loc.file ? loc.file : "<unknown>", loc.line, loc.col, buf);
        return;
    }
    if (nwcap >= wcap_max) {
        wcap_max = wcap_max ? wcap_max * 2 : 16;
        wcap = (LoamDiag *)realloc(wcap, (size_t)wcap_max * sizeof(LoamDiag));
    }
    wcap[nwcap].file = loam_dup(loc.file ? loc.file : "");
    wcap[nwcap].line = loc.line;
    wcap[nwcap].col = loc.col;
    wcap[nwcap].end_line = loc.end_line;
    wcap[nwcap].end_col = loc.end_col;
    wcap[nwcap].msg = loam_dup(buf);
    nwcap++;
}

void loam_warn_take(LoamDiag **out, int *n) {
    *out = wcap;
    *n = nwcap;
    wcap = NULL;
    nwcap = 0;
    wcap_max = 0;
}

/** Print to stderr and exit(1). */
void loam_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/** Heap copy of `n` bytes plus NUL. */
char *loam_dupn(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) loam_fatal("out of memory");
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/** Heap copy of a C string. */
char *loam_dup(const char *s) {
    if (!s) return NULL;
    return loam_dupn(s, strlen(s));
}
