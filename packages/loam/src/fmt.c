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
 * Style, in full:
 *
 *   - 4 spaces per **block** (`{`), never per call paren. A trailing block
 *     passed as an argument therefore indents one level from its statement:
 *
 *         zeus.App("x", fn() {
 *             body
 *         })
 *
 *   - A line that continues a `(` or `[` group aligns under the token after the
 *     opener, and a closer on its own line aligns with the opener itself.
 *   - Single spaces between tokens; none before `,` `;` `)` `]` or after
 *     `(` `[`; one before `{`; one space around `=` and `->`.
 *   - A one-line `{ ... }` keeps the space its author wrote after the brace on
 *     both sides (`{ x, y }`, never `{ x, y}`). Blocks stay as written.
 *   - At most one blank line; one trailing newline.
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

#define MAX_OPEN 64

/**
 * The bracket groups still open, oldest first, so the next line can be indented
 * the way its group was written: a `{` block indents by level, a `(`/`[` group
 * aligns its continuation under the opener. `spaced` records that the author put
 * a space after that brace, which its matching `}` mirrors — the two sides of a
 * one-line `{ ... }` must agree, or the formatter emits `{ x, y}`.
 *
 * `braces` is counted separately from the stack so block indentation stays
 * exact even if a line nests deeper than the stack holds. `empty` marks a group
 * that no token has followed yet: when a line ends there, the next line is a
 * fresh block (`g(` / `{`) and indents by level, not under the opener.
 */
typedef struct {
    char kind[MAX_OPEN];
    int col[MAX_OPEN];
    int line_lead[MAX_OPEN];
    int spaced[MAX_OPEN];
    int empty[MAX_OPEN];
    /* 0 = undecided, 1 = align under the opener, -1 = indent by level. Decided
       by the first continuation line and kept, so `g(` on its own line stays a
       block rather than aligning its second line under the opener's column. */
    int aligned[MAX_OPEN];
    int n;
    int braces;
} Groups;

static int groups_push(Groups *g, char c, int col, int line_lead) {
    if (c == '{') g->braces++;
    if (g->n >= MAX_OPEN) return 0;
    g->kind[g->n] = c;
    g->col[g->n] = col;
    g->line_lead[g->n] = line_lead;
    g->spaced[g->n] = 0;
    g->empty[g->n] = 1;
    g->aligned[g->n] = 0;
    g->n++;
    return 1;
}

static void groups_pop(Groups *g, char c) {
    /* `c` is the *closer* here: `}` ends a block, `)` and `]` end a group. */
    if (c == '}' && g->braces > 0) g->braces--;
    if (g->n > 0) g->n--;
}

/**
 * Note that a token is being emitted, so no open group is waiting for its first
 * one any more. `spaced` says whether that token sits one space after an opener,
 * which is what the opener's `}` mirrors.
 */
static void groups_first_token(Groups *g, int spaced) {
    for (int k = 0; k < g->n; k++) {
        if (!g->empty[k]) continue;
        if (spaced && g->kind[k] == '{') g->spaced[k] = 1;
        g->empty[k] = 0;
    }
}

/**
 * Emit one line's content (`s[0..n)`, already stripped of leading indent),
 * applying the spacing rules and recording open groups in `g`. `line_begin` is
 * where this line starts in `out`, so an opener's column can be remembered for
 * the next line's alignment. Returns 1 if the line ended inside a string (so
 * callers skip trailing-trim).
 */
static int emit_line(Buf *out, const char *s, size_t n, size_t line_begin, int lead, Groups *g, int *in_comment) {
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
        /* Block comment: copy verbatim to its close (may not close on line).
           Braces and brackets inside a comment are text, not structure, so the
           group stack is left alone — a comment saying `{ x }` must not indent
           the next line. */
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            if (prev != 0 && pending) bput(out, ' ');
            size_t j = i;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) j++;
            size_t end = (j < n) ? j + 2 : n;
            if (j >= n) *in_comment = 1; /* the close is on a later line */
            bputs(out, s + i, end - i);
            prev = 'x';
            pending = 0;
            i = end;
            continue;
        }
        /* String literal: copy verbatim, honoring backslash escapes. */
        if (c == '"') {
            int sp = (prev != 0 && pending && prev != '(' && prev != '[');
            if (sp) bput(out, ' ');
            groups_first_token(g, sp);
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
        /* A comma or semicolon wants a space after it — but not before the
           closer that ends the list (`f(a,)`, not `f(a, )`). */
        if ((prev == ',' || prev == ';') && !is_closer(c)) need = 1;
        if (c == '-' && i + 1 < n && s[i + 1] == '>') need = 1; /* `) -> T` */
        if (c == '>' && prev == '-') need = 0;                  /* arrow, not cmp */
        /* `{ x }`: the closer mirrors the space its opener was written with, so
           a one-line brace group keeps both of its spaces. A block's `}` is at
           line start (`prev == 0`), where there is nothing to separate. */
        if (c == '}' && prev != 0 && prev != '{' && prev != '(' && prev != '[' &&
            g->n > 0 && g->kind[g->n - 1] == '{' && g->spaced[g->n - 1])
            need = 1;

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

        if (is_opener(c)) {
            groups_push(g, c, (int)(out->n - line_begin), lead);
        } else {
            groups_first_token(g, need);
            if (is_closer(c)) groups_pop(g, c);
        }
        if (need) bput(out, ' ');
        bput(out, c);

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
    Groups g = {0};
    int blanks = 0;      /* pending blank lines */
    int wrote_any = 0;
    int in_comment = 0;  /* a block comment is still open */

    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        /* Strip a trailing CR and the leading indent. */
        if (len > 0 && p[len - 1] == '\r') len--;

        /* Inside a block comment every line is text, not code: copy it byte
           for byte, its own indent included. Re-indenting one would strip the
           ` *` column that makes the comment readable, and the spacing rules
           have no business inside prose. */
        if (in_comment) {
            bputs(&out, p, len);
            bput(&out, '\n');
            wrote_any = 1;
            for (size_t k = 0; k + 1 < len; k++) {
                if (p[k] == '*' && p[k + 1] == '/') {
                    in_comment = 0;
                    break;
                }
            }
            p = nl ? nl + 1 : p + len;
            continue;
        }

        size_t f = 0;
        while (f < len && (p[f] == ' ' || p[f] == '\t')) f++;

        if (f == len) { /* blank line */
            blanks = 1;
            p = nl ? nl + 1 : p + len;
            continue;
        }

        if (wrote_any && blanks) bput(&out, '\n');
        blanks = 0;

        /* Where the line's own indent goes: the innermost group still open
           decides. A `{` block indents by level; a `(`/`[` group aligns its
           continuation under the first token after the opener, which is what
           the source does by hand. A line that *closes* a group returns to the
           group's own level — a block to the statement it belongs to, a paren
           back under the opener. */
        int lead = 0;
        if (g.n > 0) {
            int top = g.n - 1;
            if (is_closer((unsigned char)p[f])) {
                /* Return to the level the group opened at: a block to its
                   statement, a paren under the opener (or to the block level
                   when the opener is alone on its line). */
                if (g.kind[top] == '{') {
                    lead = INDENT * (g.braces - 1);
                } else {
                    if (g.aligned[top] == 0) g.aligned[top] = g.empty[top] ? -1 : 1;
                    lead = (g.aligned[top] > 0) ? g.col[top] : g.line_lead[top];
                }
            } else if (g.kind[top] == '{') {
                lead = INDENT * g.braces;
            } else {
                /* The first continuation decides the group's shape, once:
                   aligning under the opener's next token for `f(a,\n  b)`, one
                   level in from the line the opener sat on for `f(` alone at
                   end of line. Measured from that line, so nested calls step
                   in one level each. */
                if (g.aligned[top] == 0) g.aligned[top] = g.empty[top] ? -1 : 1;
                lead = (g.aligned[top] > 0) ? g.col[top] + 1 : g.line_lead[top] + INDENT;
            }
        }
        if (lead < 0) lead = 0;
        size_t line_begin = out.n;
        for (int k = 0; k < lead; k++) bput(&out, ' ');
        size_t start = out.n;
        int open_string = emit_line(&out, p + f, len - f, line_begin, lead, &g, &in_comment);
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
