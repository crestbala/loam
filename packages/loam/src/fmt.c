/**
 * fmt.c — `loam-fmt`, the Loam formatter (§1.7).
 *
 * One style, no options. Comments (`//`, `///`, `//!`, block) and string
 * literals are copied verbatim; the formatter normalizes indentation, spacing,
 * and blank lines around them. It is a scanner, not the parser: the parser
 * discards comments, and a formatter that deletes them is worse than none.
 *
 *     loam-fmt <file>     formatted source to stdout
 *     loam-fmt -w <file>  formatted source written back in place
 *     loam-fmt            read stdin, write stdout
 *
 * Style: 4-space indent by bracket depth; single spaces between tokens; no
 * space before `,` `;` `)` `]` `:` or after `(` `[`; one space before `{`; at
 * most one blank line; one trailing newline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define INDENT 4

static int is_opener(int c) { return c == '(' || c == '[' || c == '{'; }
static int is_closer(int c) { return c == ')' || c == ']' || c == '}'; }

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void bput(Buf *b, char c) {
    if (b->n + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->p = (char *)realloc(b->p, b->cap);
        if (!b->p) {
            fprintf(stderr, "loam-fmt: out of memory\n");
            exit(2);
        }
    }
    b->p[b->n++] = c;
}

static void bputs(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) bput(b, s[i]);
}

/**
 * Emit one line's content (`s[0..n)`, already stripped of leading indent),
 * applying the spacing rules and folding bracket depth into `*depth`.
 * Returns 1 if the line ended inside a string (so callers skip trailing-trim).
 */
static int emit_line(Buf *out, const char *s, size_t n, int *depth) {
    int pending = 0;   /* whitespace seen since the last token */
    int prev = 0;      /* last non-space char emitted */
    size_t i = 0;
    int in_string = 0;

    while (i < n) {
        char c = s[i];

        /* Line comment: copy verbatim (one space before it if mid-line). */
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            if (prev != 0) bput(out, ' ');
            bputs(out, s + i, n - i);
            return 0;
        }
        /* Block comment: copy verbatim to its close (may not close on line). */
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            if (prev != 0 && pending) bput(out, ' ');
            size_t j = i;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) j++;
            size_t end = (j < n) ? j + 2 : n;
            bputs(out, s + i, end - i);
            prev = 'x';
            pending = 0;
            i = end;
            continue;
        }
        /* String literal: copy verbatim, honoring backslash escapes. */
        if (c == '"') {
            if (prev != 0 && pending && prev != '(' && prev != '[') bput(out, ' ');
            size_t j = i + 1;
            while (j < n && s[j] != '"') {
                if (s[j] == '\\' && j + 1 < n) j += 2;
                else j++;
            }
            if (j < n) j++; /* closing quote */
            else in_string = 1;
            bputs(out, s + i, j - i);
            prev = 'x';
            pending = 0;
            i = j;
            continue;
        }
        if (isspace((unsigned char)c)) {
            pending = 1;
            i++;
            continue;
        }

        int need = pending;
        if (is_closer(c) || c == ',' || c == ';') need = 0;
        if (c == ':') need = 0;                 /* no space before `:` / `::` */
        if (prev == '(' || prev == '[') need = 0;
        if (c == '{' && prev != 0 && prev != '(' && prev != '[' && prev != '{') need = 1;
        if (prev == ',' || prev == ';') need = 1;
        if (c == '-' && i + 1 < n && s[i + 1] == '>') need = 1; /* `) -> T` */
        if (c == '>' && prev == '-') need = 0;                  /* arrow, not cmp */

        int simple_eq = 0;
        if (c == '=') {
            char nxt = (i + 1 < n) ? s[i + 1] : 0;
            int compound_prev = (prev == '+' || prev == '-' || prev == '*' || prev == '/' ||
                                 prev == '%' || prev == '&' || prev == '|' || prev == '^' ||
                                 prev == '!' || prev == '<' || prev == '>' || prev == '=');
            if (nxt != '=' && nxt != '>' && !compound_prev) {
                simple_eq = 1;
                need = 1; /* `x=1` -> `x = 1` */
            }
        }

        if (need) bput(out, ' ');
        bput(out, c);

        if (is_opener(c)) (*depth)++;
        else if (is_closer(c) && *depth > 0) (*depth)--;

        int prev_was = prev;

        prev = c;
        i++;
        pending = 0;
        /* A single `:` or `=` opens a value; `::` and `==`/`=>` do not. */
        if (c == ',' || c == ';' || c == '}') pending = 1;
        else if (c == '>') {
            if (prev_was == '-') pending = 1; /* after `->` */
        } else if (c == ':') {
            if (prev_was != ':' && !(i < n && s[i] == ':')) pending = 1;
        } else if (c == '=' && simple_eq) pending = 1;
    }
    return in_string;
}

static void trim_trailing(Buf *b, size_t from) {
    while (b->n > from && (b->p[b->n - 1] == ' ' || b->p[b->n - 1] == '\t')) b->n--;
}

/** Format `src` and return a malloc'd, NUL-terminated result. */
static char *format_source(const char *src) {
    Buf out = {0};
    int depth = 0;
    int blanks = 0;      /* pending blank lines */
    int wrote_any = 0;

    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        /* Strip a trailing CR and the leading indent. */
        if (len > 0 && p[len - 1] == '\r') len--;
        size_t f = 0;
        while (f < len && (p[f] == ' ' || p[f] == '\t')) f++;

        if (f == len) { /* blank line */
            blanks = 1;
            p = nl ? nl + 1 : p + len;
            continue;
        }

        if (wrote_any && blanks) bput(&out, '\n');
        blanks = 0;

        int lead = depth;
        if (is_closer((unsigned char)p[f]) && lead > 0) lead--;
        for (int k = 0; k < lead; k++) {
            for (int s = 0; s < INDENT; s++) bput(&out, ' ');
        }
        size_t start = out.n;
        int open_string = emit_line(&out, p + f, len - f, &depth);
        if (!open_string) trim_trailing(&out, start);
        bput(&out, '\n');
        wrote_any = 1;

        p = nl ? nl + 1 : p + len;
    }

    if (!wrote_any) {
        out.p = (char *)realloc(out.p, 1);
        out.p[0] = '\0';
        return out.p;
    }
    bput(&out, '\0');
    return out.p;
}

static char *read_all(FILE *f) {
    Buf b = {0};
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bputs(&b, chunk, n);
    bput(&b, '\0');
    return b.p ? b.p : (char *)calloc(1, 1);
}

static void die(const char *msg, const char *arg) {
    fprintf(stderr, "loam-fmt: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(2);
}

int main(int argc, char **argv) {
    int write_back = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) write_back = 1;
        else if (argv[i][0] == '-' && argv[i][1]) die("unknown option", argv[i]);
        else path = argv[i];
    }
    if (write_back && !path) die("no file given", NULL);

    char *src;
    if (path) {
        FILE *f = fopen(path, "rb");
        if (!f) die("cannot open", path);
        src = read_all(f);
        fclose(f);
    } else {
        src = read_all(stdin);
    }

    char *out = format_source(src);
    free(src);

    if (write_back) {
        FILE *f = fopen(path, "wb");
        if (!f) die("cannot write", path);
        fputs(out, f);
        fclose(f);
    } else {
        fputs(out, stdout);
    }
    free(out);
    return 0;
}
