/**
 * fmt.c — `loam-fmt`, the Loam formatter (§1.7).
 *
 * One style, no options, in the manner of Prettier and gofmt: the output
 * depends on the code, not on how it was laid out.
 *
 *     loam-fmt <file>        formatted source to stdout
 *     loam-fmt -w <file>...  formatted source written back in place
 *     loam-fmt --check <f>.. exit 1 and name each file that is not formatted
 *     loam-fmt               read stdin, write stdout
 *
 * It works on tokens, not on the AST: the parser drops comments, and a
 * formatter that loses them is worse than none. The tokens are grouped by
 * their brackets and printed against a 100-column line:
 *
 *   - A `(` / `[` group stays on one line when it fits. When it does not,
 *     each item goes on its own line, one level in, with a trailing comma
 *     (not in a `fn` parameter list, which takes none), and the closer comes
 *     back to the opening line's indent:
 *
 *         return Box(
 *             style = Style(width = pct(100), height = px(tall)),
 *             label = "Frame",
 *         ) {
 *
 *   - A call whose last argument is a block (`fn() { ... }`) keeps that
 *     block on the call's line when the arguments before it fit:
 *     `Each(items, fn(i: int, x: string) -> Node {`.
 *   - A `{` written across lines is a block: one statement per line, at most
 *     one blank line between them. A `{ ... }` written on one line stays on
 *     one line when it fits (`if on { a } else { b }`); an `if` / `else`
 *     chain fits or breaks as a whole.
 *   - A long `&&` / `||` chain breaks after the operator: the parser ends a
 *     statement at an operator that starts a line.
 *   - One space around binary operators, `=`, `->`, `=>`, and after `,` and
 *     `:`. None inside `()` / `[]`, around `.`, `::`, `..`, after a unary
 *     operator, or inside generics (`Option<Signal<T>>`) and closure pipes
 *     (`|x| { ... }`). `{ x }` keeps a space inside each brace.
 *   - Strings and comments are copied verbatim. 4-space indent, one
 *     trailing newline.
 *
 * Before anything is written the output is lexed again and compared with
 * the input: the tokens must be the same, save commas before `)` / `}`.
 * A mismatch is a formatter bug, and the file is left alone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define WIDTH 100
#define INDENT 4
#define INF 1000000

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void die(const char *msg, const char *arg) {
    fprintf(stderr, "loam-fmt: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(2);
}

static void bput(Buf *b, char c) {
    if (b->n + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->p = (char *)realloc(b->p, b->cap);
        if (!b->p) die("out of memory", NULL);
    }
    b->p[b->n++] = c;
}

static void bputs(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) bput(b, s[i]);
}

/* ------------------------------------------------------------------------ */
/* Tokens                                                                    */

typedef enum { K_WORD, K_NUM, K_STR, K_OP, K_OPEN, K_CLOSE, K_LCOM, K_BCOM } Kind;

typedef struct {
    Kind k;
    const char *s;
    int len;
    int nl;     /* newlines between the previous token and this one */
    int sp;     /* any whitespace before it in the source */
    int mate;   /* the matching bracket */
    int unary;  /* `-` `*` `&` `!` `~` as a prefix */
    int gen;    /* `<` `>` `>>` as generic angles */
    int pipe;   /* closure `|`: 1 opens params, 2 closes them, 3 is `||` */
    int width;  /* display columns; INF if it spans lines */
    int depth;  /* bracket depth it sits at */
} Tok;

typedef struct {
    Tok *t;
    int n, cap;
} Toks;

static int is(const Tok *t, const char *s) {
    int n = (int)strlen(s);
    return t->len == n && memcmp(t->s, s, (size_t)n) == 0;
}

static int text_width(const char *s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n') return INF;
        if (((unsigned char)s[i] & 0xC0) != 0x80) w++;
    }
    return w;
}

static void push(Toks *ts, Kind k, const char *s, int len, int nl, int sp) {
    if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 1024;
        ts->t = (Tok *)realloc(ts->t, (size_t)ts->cap * sizeof(Tok));
        if (!ts->t) die("out of memory", NULL);
    }
    Tok *t = &ts->t[ts->n++];
    memset(t, 0, sizeof *t);
    t->k = k;
    t->s = s;
    t->len = len;
    t->nl = nl;
    t->sp = sp;
    t->mate = -1;
    t->width = text_width(s, len);
}

/* Longest first, so `>>=` is not read as `>>` then `=`. */
static const char *OPS[] = {
    "<<=", ">>=", "::", "->", "=>", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=",
    "%=", "&=", "|=", "^=", "++", "--", "<<", ">>", "..", NULL,
};

/** Scan `src` into tokens. Returns 0 on success, or a message. */
static const char *lex(const char *src, Toks *ts) {
    const char *p = src;
    int nl = 0, sp = 0;
    while (*p) {
        char c = *p;
        if (c == '\n') {
            nl++;
            sp = 1;
            p++;
            continue;
        }
        if (isspace((unsigned char)c)) {
            sp = 1;
            p++;
            continue;
        }
        const char *s = p;
        Kind k;
        if (c == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            const char *e = p;
            while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
            push(ts, K_LCOM, s, (int)(e - s), nl, sp);
            nl = sp = 0;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (!*p) return "unterminated block comment";
            p += 2;
            k = K_BCOM;
        } else if (c == '"') {
            p++;
            while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
            if (!*p) return "unterminated string";
            p++;
            k = K_STR;
        } else if (isdigit((unsigned char)c)) {
            if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while (isxdigit((unsigned char)*p) || *p == '_') p++;
            } else {
                while (isdigit((unsigned char)*p) || *p == '_') p++;
                if (*p == '.' && isdigit((unsigned char)p[1])) {
                    p++;
                    while (isdigit((unsigned char)*p)) p++;
                }
                if (*p == 'e' || *p == 'E') {
                    const char *q = p + 1;
                    if (*q == '+' || *q == '-') q++;
                    if (isdigit((unsigned char)*q)) {
                        p = q;
                        while (isdigit((unsigned char)*p)) p++;
                    }
                }
            }
            k = K_NUM;
        } else if (isalpha((unsigned char)c) || c == '_' || ((unsigned char)c & 0x80)) {
            while (isalnum((unsigned char)*p) || *p == '_' || ((unsigned char)*p & 0x80)) p++;
            k = K_WORD;
        } else if (c == '(' || c == '[' || c == '{') {
            p++;
            k = K_OPEN;
        } else if (c == ')' || c == ']' || c == '}') {
            p++;
            k = K_CLOSE;
        } else {
            int len = 1;
            for (int i = 0; OPS[i]; i++) {
                int n = (int)strlen(OPS[i]);
                if (strncmp(p, OPS[i], (size_t)n) == 0) {
                    len = n;
                    break;
                }
            }
            p += len;
            k = K_OP;
        }
        push(ts, k, s, (int)(p - s), nl, sp);
        nl = sp = 0;
    }
    /* Match brackets. */
    int *stack = (int *)malloc(sizeof(int) * (size_t)(ts->n + 1));
    int top = 0;
    for (int i = 0; i < ts->n; i++) {
        Tok *t = &ts->t[i];
        t->depth = top;
        if (t->k == K_OPEN) {
            stack[top++] = i;
        } else if (t->k == K_CLOSE) {
            if (!top) {
                free(stack);
                return "unbalanced closing bracket";
            }
            Tok *o = &ts->t[stack[--top]];
            char want = o->s[0] == '(' ? ')' : o->s[0] == '[' ? ']' : '}';
            if (t->s[0] != want) {
                free(stack);
                return "mismatched brackets";
            }
            o->mate = i;
            t->mate = (int)(o - ts->t);
            t->depth = top;
        }
    }
    free(stack);
    if (top) return "unclosed bracket";
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Classification                                                            */

static Tok *T;
static int NT;

static int is_kw(const Tok *t) {
    static const char *kw[] = {"fn", "let", "mut", "return", "if", "else", "for", "in",
                               "while", "switch", "case", "default", "struct", "enum", "import", "pub", "const",
                               "break", "continue", "as", "loop", "type", "use", NULL};
    if (t->k != K_WORD) return 0;
    for (int i = 0; kw[i]; i++)
        if (is(t, kw[i])) return 1;
    return 0;
}

/** Ends an operand: what precedes a binary (not a unary) operator. */
static int ends_operand(const Tok *t) {
    if (t->k == K_WORD) return !is_kw(t);
    if (t->k == K_NUM || t->k == K_STR) return 1;
    if (t->k == K_CLOSE) return 1;
    if (t->k == K_OP && ((t->gen && !is(t, "<")) || is(t, "?"))) return 1;
    if (t->k == K_OP && t->pipe == 2) return 0;
    return 0;
}

static int prev_code(int i) {
    for (int k = i - 1; k >= 0; k--)
        if (T[k].k != K_LCOM && T[k].k != K_BCOM) return k;
    return -1;
}

static int looks_like_type(const Tok *t) {
    static const char *prim[] = {"int", "float", "bool", "string", "void", "i8", "i16", "i32",
                                 "i64", "u8", "u16", "u32", "u64", "f32", "f64", "fn", NULL};
    if (t->k == K_OPEN && t->s[0] == '[') return 1;
    if (t->k == K_OP && (is(t, "&") || is(t, "*"))) return 1;
    if (t->k != K_WORD) return 0;
    if (isupper((unsigned char)t->s[0])) return 1;
    for (int i = 0; prim[i]; i++)
        if (is(t, prim[i])) return 1;
    return 0;
}

static void classify(void) {
    int gdepth = 0;
    for (int i = 0; i < NT; i++) {
        Tok *t = &T[i];
        int pi = prev_code(i);
        Tok *pv = pi >= 0 ? &T[pi] : NULL;
        if (t->k == K_OPEN && t->s[0] == '{') gdepth = 0;
        if (t->k != K_OP) continue;
        if (is(t, "=") || is(t, ";")) gdepth = 0;
        if (is(t, "-") || is(t, "*") || is(t, "&") || is(t, "!") || is(t, "~")) {
            /* An operator that starts a line never continues an expression. */
            if (!pv || t->nl > 0 || !ends_operand(pv)) t->unary = 1;
            continue;
        }
        if (is(t, "<")) {
            if (!t->sp && pv && pv->k == K_WORD && !is_kw(pv) && i + 1 < NT &&
                !T[i + 1].sp && looks_like_type(&T[i + 1])) {
                t->gen = 1;
                gdepth++;
            }
            continue;
        }
        if (is(t, ">") && gdepth > 0 && !t->sp) {
            t->gen = 1;
            gdepth--;
            continue;
        }
        if (is(t, ">>") && gdepth > 1 && !t->sp) {
            t->gen = 1;
            gdepth -= 2;
            continue;
        }
        if (is(t, "||") && (!pv || !ends_operand(pv))) {
            t->pipe = 3;
            continue;
        }
        if (is(t, "|") && (!pv || !ends_operand(pv))) {
            t->pipe = 1;
            for (int j = i + 1; j < NT; j++) {
                if (T[j].depth < t->depth) break;
                if (T[j].depth == t->depth && T[j].k == K_OP && is(&T[j], "|")) {
                    T[j].pipe = 2;
                    i = j;
                    break;
                }
            }
        }
    }
}

/** One space between tokens `a` and `b` when they share a line? */
static int space(int a, int b) {
    Tok *p = &T[a], *n = &T[b];
    if (n->k == K_LCOM || n->k == K_BCOM || p->k == K_BCOM) return 1;
    if (n->k == K_CLOSE) return n->s[0] == '}' && !(p->k == K_OPEN && p->s[0] == '{');
    if (p->k == K_OPEN) return p->s[0] == '{';
    if (n->k == K_OP && (is(n, ",") || is(n, ";") || is(n, ".") || is(n, "?") || is(n, "::") ||
                         is(n, ":") || is(n, "..")))
        return 0;
    if (p->k == K_OP && (is(p, ".") || is(p, "::") || is(p, "..") || is(p, "#"))) return 0;
    if (p->unary) return 0;
    if (p->gen) {
        if (is(p, "<")) return 0;
        if (n->k == K_OPEN && n->s[0] == '(') return 0;
        if (n->gen) return 0;
        return 1;
    }
    if (n->gen) return 0;
    if (p->pipe == 1 || n->pipe == 2) return 0;
    if (n->k == K_OPEN && n->s[0] == '(') {
        if (p->k == K_WORD && (!is_kw(p) || is(p, "fn"))) return 0;
        if (p->k == K_CLOSE && p->s[0] != '}') return 0;
        if (p->k == K_OP && is(p, "?")) return 0;
        return 1;
    }
    if (n->k == K_OPEN && n->s[0] == '[') {
        if (p->k == K_WORD && !is_kw(p)) return 0;
        if (p->k == K_CLOSE && p->s[0] != '}') return 0;
        return 1;
    }
    /* `[]int`, `[4]u8`, `[]fn() -> T`: a type after its brackets. */
    if (p->k == K_CLOSE && p->s[0] == ']' && n->k == K_WORD && (!is_kw(n) || is(n, "fn"))) {
        int q = prev_code(p->mate);
        while (q >= 0 && T[q].k == K_CLOSE && T[q].s[0] == ']') q = prev_code(T[q].mate);
        if (q < 0 || !ends_operand(&T[q])) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Printing                                                                  */

static Buf OUT;
static int COL;      /* column of the next byte */
static int LAST;     /* last token written, -1 at the start of a line */
static int *BLOCK;   /* per `{`: print as a block (forced or as written) */

static int is_open(int i, char c) { return T[i].k == K_OPEN && T[i].s[0] == c; }

static void newline(int ind) {
    while (OUT.n && OUT.p[OUT.n - 1] == ' ') OUT.n--;
    bput(&OUT, '\n');
    for (int k = 0; k < ind; k++) bput(&OUT, ' ');
    COL = ind;
    LAST = -1;
}

static void put(int i) {
    if (LAST >= 0 && space(LAST, i)) {
        bput(&OUT, ' ');
        COL++;
    }
    bputs(&OUT, T[i].s, (size_t)T[i].len);
    if (T[i].width >= INF) {
        const char *nlp = T[i].s + T[i].len;
        while (nlp > T[i].s && nlp[-1] != '\n') nlp--;
        COL = text_width(nlp, (int)(T[i].s + T[i].len - nlp));
    } else {
        COL += T[i].width;
    }
    LAST = i;
}

static void put_text(const char *s) {
    bputs(&OUT, s, strlen(s));
    COL += (int)strlen(s);
}

/** A `{` that prints as a block: written across lines, or forced. */
static int is_block(int i) { return is_open(i, '{') && BLOCK[i]; }

/** Width of `[a, b)` on one line, INF if something in it must break. */
static int flat_w(int a, int b) {
    int w = 0;
    for (int i = a; i < b; i++) {
        if (T[i].k == K_LCOM || T[i].width >= INF) return INF;
        if (is_block(i) && T[i].mate > i + 1) return INF;
        if (T[i].k == K_OP && is(&T[i], ",") && i + 1 < b && T[i + 1].k == K_CLOSE) continue;
        if (i > a && space(i - 1, i)) w++;
        w += T[i].width;
        if (w >= INF) return INF;
    }
    return w;
}

/** Width from `k` up to the next place a line can break: through the next
 *  opener, or to `b`. `*all` says the run reached `b`. */
static int rest_w(int k, int b, int *all) {
    int w = 0;
    *all = 1;
    for (int i = k; i < b; i++) {
        if (T[i].k == K_LCOM) break;
        if (i > 0 && space(i - 1, i)) w++;
        w += T[i].width >= INF ? 0 : T[i].width;
        if (T[i].k == K_OPEN) {
            *all = 0;
            break;
        }
    }
    return w;
}

static void emit_range(int a, int b, int ind, int tail);
static void emit_block(int i, int ind);

/** One line. A trailing comma (`g(1, 2,)`) only belongs to a broken list. */
static void emit_flat(int a, int b) {
    for (int i = a; i < b; i++) {
        if (T[i].k == K_OP && is(&T[i], ",") && i + 1 < b && T[i + 1].k == K_CLOSE) continue;
        put(i);
    }
}

/** Item bounds of the group `(i, mate)`, split at commas at its own level. */
typedef struct {
    int s, e;      /* tokens, the comma excluded */
    int comma;     /* index of the comma after it, or -1 */
} Item;

static int split_items(int i, Item *items, int max) {
    int j = T[i].mate, n = 0, s = i + 1, depth = 0;
    for (int k = i + 1; k < j; k++) {
        Tok *t = &T[k];
        if (t->k == K_OPEN) {
            k = t->mate;
            continue;
        }
        if (t->gen) depth += is(t, "<") ? 1 : is(t, ">>") ? -2 : -1;
        if (t->pipe == 1) depth++;
        if (t->pipe == 2) depth--;
        if (depth <= 0 && t->k == K_OP && is(t, ",")) {
            if (n < max) items[n++] = (Item){s, k, k};
            s = k + 1;
            depth = 0;
        }
    }
    if (s < j && n < max) items[n++] = (Item){s, j, -1};
    return n;
}

/** A `fn` parameter list, which takes no trailing comma. */
static int is_params(int i) {
    int p = prev_code(i);
    if (p < 0) return 0;
    if (T[p].k == K_WORD && is(&T[p], "fn")) return 1;
    if (T[p].k == K_OP && T[p].gen) {
        /* `fn name<T>(`: walk back over the generics. */
        int q = p;
        while (q >= 0 && !(T[q].k == K_OP && T[q].gen && is(&T[q], "<"))) q--;
        p = prev_code(q);
    }
    if (p < 0 || T[p].k != K_WORD) return 0;
    int q = prev_code(p);
    return q >= 0 && T[q].k == K_WORD && is(&T[q], "fn");
}

/** A line comment among the items of a group, not inside one of them. */
/** A call's argument list: `(` right after the callee. Any other `(` groups
 *  an expression or makes a tuple, where a trailing comma changes meaning. */
static int is_call(int i) {
    if (!is_open(i, '(')) return 0;
    int p = prev_code(i);
    return p >= 0 && ends_operand(&T[p]) && !T[i].sp;
}

static int has_comment(int a, int b) {
    for (int k = a; k < b; k++) {
        if (T[k].k == K_OPEN) k = T[k].mate;
        else if (T[k].k == K_LCOM) return 1;
    }
    return 0;
}

/** Print a group that does not fit on the line. */
static void emit_broken(int i, int ind) {
    int j = T[i].mate;
    static Item stack_items[4096];
    static int used = 0;
    if (used + 512 > 4096) die("groups nested too deeply", NULL);
    Item *items = stack_items + used;
    int n = split_items(i, items, 512);
    used += n;
    int brace = is_open(i, '{');

    /* A one-statement `{ ... }` that does not fit becomes a block. */
    if (brace && n <= 1) {
        used -= n;
        BLOCK[i] = 1;
        emit_block(i, ind);
        return;
    }
    /* The last argument ends in a block or a literal (`fn() { ... }`,
       `RPoint { ... }`), or is the only one and ends in a call
       (`Box(style = Style(...))`): keep its opening on this line when the
       arguments before it fit, and let it break on its own. */
    if (!brace && n >= 1 && !has_comment(i + 1, j)) {
        Item *l = &items[n - 1];
        int endc = l->e - 1;
        int hug = T[endc].k == K_CLOSE && T[endc].mate >= l->s &&
                  (T[endc].s[0] == '}' || n == 1);
        int head = 1, ok = hug;
        for (int k = 0; ok && k + 1 < n; k++) {
            int w = flat_w(items[k].s, items[k].e);
            if (w >= INF) ok = 0;
            head += w + 2;
        }
        int all;
        if (ok) {
            int first = rest_w(l->s, l->e, &all);
            ok = COL + 1 + head + first <= WIDTH;
        }
        if (ok) {
            put(i);
            for (int k = 0; k + 1 < n; k++) {
                emit_flat(items[k].s, items[k].e);
                put(items[k].comma);
            }
            emit_range(l->s, l->e, ind, 1);
            put(j);
            used -= n;
            return;
        }
    }
    /* A grouping `( expr )`: keep it on the line and let the expression
       break inside. */
    if (is_open(i, '(') && !is_call(i) && !is_params(i) && n == 1 && !has_comment(i + 1, j)) {
        put(i);
        emit_range(items[0].s, items[0].e, ind, 1);
        put(j);
        used -= n;
        return;
    }
    int trailing = brace || (is_call(i) && !is_params(i));
    put(i);
    for (int k = 0; k < n; k++) {
        Item *it = &items[k];
        int s = it->s;
        /* Comments written on their own lines before the item. */
        while (s < it->e && T[s].k == K_LCOM) {
            if (T[s].nl > 0 || k == 0) newline(ind + INDENT);
            put(s);
            s++;
        }
        if (s < it->e) {
            newline(ind + INDENT);
            emit_range(s, it->e, ind + INDENT, 1);
        }
        if (it->comma >= 0) put(it->comma);
        else if (trailing && s < it->e) put_text(",");
        /* A comment after the comma, on the same line, stays with it. */
        if (it->comma >= 0 && k + 1 < n && T[items[k + 1].s].k == K_LCOM &&
            T[items[k + 1].s].nl == 0) {
            put(items[k + 1].s);
            items[k + 1].s++;
        }
    }
    newline(ind);
    put(j);
    used -= n;
}

/** Last `}` of an `if` / `else` chain whose first block opens at `i`. */
static int chain_end(int i) {
    int k = T[i].mate;
    while (k + 1 < NT && T[k + 1].k == K_WORD && is(&T[k + 1], "else")) {
        int m = k + 2;
        while (m < NT && !is_open(m, '{') && T[m].k != K_CLOSE) {
            if (T[m].k == K_OPEN) m = T[m].mate;
            m++;
        }
        if (m >= NT || !is_open(m, '{')) break;
        k = T[m].mate;
    }
    return k;
}

/** Rank of an operator a long line may break after, loosest first: `||`,
 *  `&&`, a pattern's `|`, then `+` / `-`. 0 for anything else. */
static int chain_rank(int k) {
    Tok *t = &T[k];
    if (t->k != K_OP || t->pipe || t->unary) return 0;
    if (is(t, "||")) return 1;
    if (is(t, "&&")) return 2;
    if (is(t, "|")) return 3;
    if (is(t, "+") || is(t, "-")) return 4;
    /* A row of a list written across lines (`[]int {` ... `}`). */
    if (is(t, ",")) return 5;
    return 0;
}

/** Break a long chain after each of its loosest operators: `a &&` /
 *  `b &&` / `c`. Returns 1 if it did. */
static int emit_chain(int a, int b, int ind, int tail) {
    /* Only the part before the first `{` is the chain: an `if` / `else`
       body breaks on its own before the condition does. */
    int end = b;
    for (int k = a; k < b; k++) {
        if (T[k].k == K_LCOM || is_open(k, '{')) {
            end = k;
            break;
        }
        if (T[k].k == K_OPEN) k = T[k].mate;
    }
    /* An operator that already ends the range is where it was cut: a piece
       of a chain is not split again. */
    int rank = 0;
    for (int k = a; k + 1 < end; k++) {
        if (T[k].k == K_OPEN) {
            k = T[k].mate;
            continue;
        }
        int r = chain_rank(k);
        if (r && (!rank || r < rank)) rank = r;
    }
    if (!rank) return 0;
    int w = flat_w(a, end);
    int more = end < b ? 2 : tail;
    if (w < INF && COL + 1 + w + more <= WIDTH) return 0;
    int s = a, first = 1;
    for (int k = a; k <= end; k++) {
        int cut = k == end;
        if (!cut && T[k].k == K_OPEN) {
            k = T[k].mate;
            continue;
        }
        if (!cut && chain_rank(k) == rank) cut = 2;
        if (!cut) continue;
        int e = cut == 2 ? k + 1 : k;
        if (e > s) {
            /* `&&` / `||`: one operand a line. A pattern's `|` and a sum
               fill each line as far as it goes. */
            if (!first) {
                int pw = flat_w(s, e);
                int fill = rank >= 3 && pw < INF && COL + 1 + pw + (e == end ? (end < b ? 2 : tail) : 0) <= WIDTH;
                /* A list row wraps to the list's own indent: each line of it
                   is a row again next time. */
                if (!fill) newline(rank == 5 ? ind : ind + INDENT);
            }
            emit_range(s, e, first || rank == 5 ? ind : ind + INDENT, e == b ? tail : 0);
            first = 0;
        }
        s = e;
    }
    if (end < b) emit_range(end, b, ind, tail);
    return 1;
}

/** Print tokens `[a, b)` from the current column. `ind` is the indent of
 *  the line they started on; `tail` is what must still fit after them. */
static void emit_range(int a, int b, int ind, int tail) {
    if (emit_chain(a, b, ind, tail)) return;
    for (int i = a; i < b;) {
        Tok *t = &T[i];
        if (t->k == K_LCOM) {
            put(i);
            i++;
            if (i < b) newline(ind + INDENT);
            continue;
        }
        if (t->k != K_OPEN) {
            put(i);
            i++;
            continue;
        }
        int j = t->mate;
        if (is_block(i)) {
            emit_block(i, ind);
            i = j + 1;
            continue;
        }
        int lead = LAST >= 0 && space(LAST, i);
        /* An `if` / `else` chain fits or breaks as a whole. */
        if (is_open(i, '{')) {
            int ce = chain_end(i);
            if (ce > j && ce < b) {
                int all, rest = rest_w(ce + 1, b, &all) + (all ? tail : 0);
                int w = flat_w(i, ce + 1);
                if (w < INF && COL + lead + w + rest <= WIDTH) {
                    emit_flat(i, ce + 1);
                } else {
                    for (int k = i; k <= ce; k++)
                        if (is_open(k, '{') && T[k].depth == t->depth) BLOCK[k] = 1;
                    emit_range(i, ce + 1, ind, 0);
                }
                i = ce + 1;
                continue;
            }
        }
        int all, rest = rest_w(j + 1, b, &all) + (all ? tail : 0);
        int w = flat_w(i, j + 1);
        /* `[...]` is an index or a type, never a list: it stays on its line. */
        if (is_open(i, '[') || (w < INF && COL + lead + w + rest <= WIDTH)) emit_flat(i, j + 1);
        else emit_broken(i, ind);
        i = j + 1;
    }
}

/** Does the token at `k` (a line start inside a block) continue the
 *  statement before it rather than begin one? */
static int continues(int k, int a) {
    if (k <= a) return 0;
    Tok *p = &T[k - 1], *t = &T[k];
    if (p->k == K_LCOM || p->k == K_BCOM) return 0;
    if (t->k == K_OP && (is(t, ".") || is(t, "?"))) return 1;
    if (t->k == K_WORD && is(t, "else")) return 1;
    if (p->k == K_OP && !is(p, ",") && !is(p, ";") && !is(p, ":") && !is(p, "?") && !is(p, "++") &&
        !is(p, "--") && !p->gen && p->pipe != 2)
        return 1;
    return 0;
}

static int is_case_label(int k) {
    return T[k].k == K_WORD && (is(&T[k], "case") || is(&T[k], "default"));
}

/** Statements of `[a, b)`, one per line at `ind`. `top` is the file level.
 *  In a switch body, `case` / `default` sit at `ind` and the statements
 *  under each sit one level in. */
static void emit_stmts(int a, int b, int ind, int top) {
    int s = a;
    int first = 1;
    int in_case = 0;
    while (s < b) {
        int e = s + 1;
        for (; e < b; e++) {
            if (T[e - 1].k == K_OPEN) e = T[e - 1].mate + 1;
            if (e >= b) break;
            if (T[e].nl > 0 && !continues(e, a)) break;
        }
        if (e > b) e = b;
        if (first && !top && T[s].k == K_LCOM && T[s].nl == 0) {
            /* `{ // note`: a comment on the opening line stays there. */
            put(s);
        } else {
            int at = ind;
            if (is_case_label(s)) in_case = 1;
            else if (in_case) at = ind + INDENT;
            if (!first && T[s].nl >= 2) bput(&OUT, '\n');
            if (!(first && top)) newline(at);
            emit_range(s, e, at, 0);
        }
        first = 0;
        s = e;
    }
}

static void emit_block(int i, int ind) {
    int j = T[i].mate;
    put(i);
    if (j == i + 1) {
        put(j);
        return;
    }
    emit_stmts(i + 1, j, ind + INDENT, 0);
    newline(ind);
    put(j);
}

/* ------------------------------------------------------------------------ */
/* Driver                                                                    */

/** A comma right before `}` or a call's `)`: the one a broken list adds or
 *  a flat one drops. Grouping parens and `[...]` never take one. */
static int list_comma(Toks *x, int i) {
    if (!is(&x->t[i], ",") || i + 1 >= x->n || x->t[i + 1].k != K_CLOSE) return 0;
    Tok *c = &x->t[i + 1];
    if (c->s[0] == '}') return 1;
    if (c->s[0] != ')') return 0;
    /* A call's `(`: right after a name, `)`, `]`, or `>`. */
    int o = c->mate;
    if (o <= 0 || x->t[o].sp) return 0;
    Tok *p = &x->t[o - 1];
    return (p->k == K_WORD && !is_kw(p)) || (p->k == K_CLOSE) || (p->k == K_OP && is(p, ">"));
}

/** Tokens equal, ignoring trailing list commas. */
static int same_tokens(Toks *x, Toks *y) {
    int i = 0, j = 0;
    for (;;) {
        while (i < x->n && list_comma(x, i)) i++;
        while (j < y->n && list_comma(y, j)) j++;
        if (i >= x->n || j >= y->n) return i >= x->n && j >= y->n;
        Tok *a = &x->t[i], *b = &y->t[j];
        if (a->k != b->k || a->len != b->len || memcmp(a->s, b->s, (size_t)a->len) != 0) return 0;
        i++;
        j++;
    }
}

/** Format `src`; NULL and `*err` on failure. */
static char *format_source(const char *src, const char **err) {
    Toks ts = {0};
    *err = lex(src, &ts);
    if (*err) {
        free(ts.t);
        return NULL;
    }
    T = ts.t;
    NT = ts.n;
    BLOCK = (int *)calloc((size_t)NT + 1, sizeof(int));
    for (int i = 0; i < NT; i++) {
        if (!is_open(i, '{')) continue;
        for (int k = i + 1; k <= T[i].mate; k++)
            if (T[k].nl > 0) {
                BLOCK[i] = 1;
                break;
            }
    }
    classify();
    OUT = (Buf){0};
    COL = 0;
    LAST = -1;
    if (NT) {
        emit_stmts(0, NT, 0, 1);
        while (OUT.n && OUT.p[OUT.n - 1] == ' ') OUT.n--;
        bput(&OUT, '\n');
    }
    bput(&OUT, '\0');
    free(BLOCK);

    Toks back = {0};
    const char *e2 = lex(OUT.p, &back);
    int ok = !e2 && same_tokens(&ts, &back);
    free(back.t);
    free(ts.t);
    if (!ok) {
        *err = "internal error: output does not match the input's tokens (file left as is)";
        free(OUT.p);
        return NULL;
    }
    return OUT.p;
}

static char *read_all(FILE *f) {
    Buf b = {0};
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bputs(&b, chunk, n);
    bput(&b, '\0');
    return b.p;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open", path);
    char *src = read_all(f);
    fclose(f);
    return src;
}

int main(int argc, char **argv) {
    int write_back = 0, check = 0, nfiles = 0, status = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) write_back = 1;
        else if (strcmp(argv[i], "--check") == 0) check = 1;
        else if (argv[i][0] == '-' && argv[i][1]) die("unknown option", argv[i]);
        else nfiles++;
    }
    if ((write_back || check) && !nfiles) die("no file given", NULL);

    if (!nfiles) {
        char *src = read_all(stdin);
        const char *err;
        char *out = format_source(src, &err);
        if (!out) die(err, "<stdin>");
        fputs(out, stdout);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        const char *path = argv[i];
        char *src = read_file(path);
        const char *err;
        char *out = format_source(src, &err);
        if (!out) {
            fprintf(stderr, "loam-fmt: %s: %s\n", path, err);
            status = 2;
            free(src);
            continue;
        }
        if (check) {
            if (strcmp(src, out) != 0) {
                printf("%s\n", path);
                if (status == 0) status = 1;
            }
        } else if (write_back) {
            if (strcmp(src, out) != 0) {
                FILE *f = fopen(path, "wb");
                if (!f) die("cannot write", path);
                fputs(out, f);
                fclose(f);
            }
        } else {
            fputs(out, stdout);
        }
        free(src);
        free(out);
    }
    return status;
}
