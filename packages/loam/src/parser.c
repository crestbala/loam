/**
 * parser.c — recursive-descent parser (one token lookahead).
 *
 * Pratt parser for binary ops. Struct literals are disabled in `if`/`for`
 * conditions so `{` starts a block. Closures are `|x| { ... }` / `|| { ... }`.
 * Semicolons are optional. `///` attaches to the next item; `//!` to the module.
 */
#include "parser.h"
#include "diagnostics.h"
#include "ext.h"
#include <stdlib.h>
#include <string.h>

static void advance(Parser *p) {
    p->previous = p->current;
    p->current = p->peek;
    p->peek = lexer_next(p->lex);
}

static int check(Parser *p, TokenKind k) { return p->current.kind == k; }

/** Consume current if it matches `k`. */
static int match(Parser *p, TokenKind k) {
    if (check(p, k)) {
        advance(p);
        return 1;
    }
    return 0;
}

static void error(Parser *p, const char *m) {
    if (p->had_error) return;
    loam_error(p->current.loc, "%s (got %s)", m, token_kind_name(p->current.kind));
    p->had_error = 1;
}

static void consume(Parser *p, TokenKind k, const char *m) {
    if (match(p, k)) return;
    error(p, m);
}

static void optional_semi(Parser *p) { match(p, TOK_SEMICOLON); }

/** Text after `///` / `//!`, dropping one leading space. */
static char *doc_line_text(Token t) {
    int skip = 3;
    if (t.len < skip) return loam_dup("");
    const char *s = t.start + skip;
    int n = t.len - skip;
    if (n > 0 && s[0] == ' ') {
        s++;
        n--;
    }
    return loam_dupn(s, n > 0 ? (size_t)n : 0);
}

static char *doc_append(char *acc, const char *line) {
    if (!line) line = "";
    if (!acc) return loam_dup(line);
    size_t a = strlen(acc), b = strlen(line);
    char *n = (char *)malloc(a + b + 2);
    if (!n) loam_fatal("out of memory");
    memcpy(n, acc, a);
    n[a] = '\n';
    memcpy(n + a + 1, line, b + 1);
    free(acc);
    return n;
}

/** Consecutive `///` lines. */
static char *take_docs(Parser *p) {
    char *acc = NULL;
    while (check(p, TOK_DOC)) {
        char *line = doc_line_text(p->current);
        acc = doc_append(acc, line);
        free(line);
        advance(p);
    }
    return acc;
}

/** Consecutive `//!` lines. */
static char *take_mod_docs(Parser *p) {
    char *acc = NULL;
    while (check(p, TOK_MOD_DOC)) {
        char *line = doc_line_text(p->current);
        acc = doc_append(acc, line);
        free(line);
        advance(p);
    }
    return acc;
}

static char *doc_join(char *a, char *b) {
    if (!a) return b;
    if (!b) return a;
    char *n = doc_append(a, b);
    free(b);
    return n;
}

static char *tok_text(Token t) { return loam_dupn(t.start, (size_t)t.len); }

/** A `TOK_NUMBER` lexeme: decimal, or `0x`/`0X` hex (e.g. a packed RGB color).
 * Never base 0 — that would read a bare leading-zero decimal as octal. */
static int64_t tok_int(Token t) {
    if (t.len > 2 && t.start[0] == '0' && (t.start[1] == 'x' || t.start[1] == 'X')) {
        return strtoll(t.start, NULL, 16);
    }
    return strtoll(t.start, NULL, 10);
}

/** Decode a quoted token into a heap string (`\n` `\t` `\r` `\"` `\\`). */
static char *unescape_string(Token t) {
    /* t includes quotes */
    int len = t.len;
    const char *s = t.start;
    char *out = (char *)malloc((size_t)len);
    if (!out) loam_fatal("out of memory");
    int j = 0;
    for (int i = 1; i < len - 1; i++) {
        if (s[i] == '\\' && i + 1 < len - 1) {
            char n = s[++i];
            if (n == 'n') out[j++] = '\n';
            else if (n == 't') out[j++] = '\t';
            else if (n == 'r') out[j++] = '\r';
            else if (n == '"') out[j++] = '"';
            else if (n == '\\') out[j++] = '\\';
            else out[j++] = n;
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* Import alias for a path: the file name with a source extension removed.
   Dots before the extension belong to the module name, so `v1.2.loam` → `v1.2`. */
static char *file_stem(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return loam_dupn(base, loam_stem_len_ext(base, strlen(base)));
}

/** Prime current and peek from the lexer. */
void parser_init(Parser *p, Lexer *l) {
    p->lex = l;
    p->had_error = 0;
    p->allow_struct_lit = 1;
    p->current = lexer_next(l);
    p->peek = lexer_next(l);
}

static AstNode *parse_type(Parser *p);
static AstNode *parse_expr(Parser *p);
static AstNode *parse_cond_expr(Parser *p);
static AstNode *parse_block(Parser *p);
static AstNode *parse_stmt(Parser *p);
static Param *parse_params(Parser *p, size_t *out);

/** One `{{ expr }}` hole, parsed from its own lexer over `src`. The text is
 * kept alive for the process (the AST holds spans into no buffer, but
 * diagnostics name `file`), which matches how module sources are retained. */
static AstNode *parse_sub_expr(char *src, const char *file, SourceLoc loc) {
    Lexer *sub = (Lexer *)malloc(sizeof(Lexer));
    if (!sub) loam_fatal("out of memory");
    lexer_init(sub, src, file);
    sub->line = loc.line;
    Parser q;
    parser_init(&q, sub);
    q.allow_struct_lit = 1;
    AstNode *e = parse_expr(&q);
    if (q.had_error || q.current.kind != TOK_EOF) {
        if (!q.had_error) loam_error(loc, "unexpected token in {{ }} interpolation");
        return NULL;
    }
    return e;
}

/** A string literal containing `{{ ... }}` becomes `__interp(part, ...)`:
 * alternating literal chunks and parsed expressions, folded to string
 * concatenation in typecheck. `text` is consumed. */
static AstNode *interp_node(char *text, SourceLoc loc, int *had_holes) {
    *had_holes = 0;
    if (!strstr(text, "{{")) return NULL;
    AstNode **parts = NULL;
    size_t np = 0;
    size_t n = strlen(text);
    size_t i = 0, lit = 0;
    for (;;) {
        if (i + 1 < n && text[i] == '{' && text[i + 1] == '{') {
            size_t close = i + 2;
            int depth = 1;
            while (close + 1 < n && depth) {
                if (text[close] == '{' && text[close + 1] == '{') { depth++; close += 2; continue; }
                if (text[close] == '}' && text[close + 1] == '}') { depth--; if (!depth) break; close += 2; continue; }
                close++;
            }
            if (depth) break; /* unterminated: treat the rest as literal text */
            if (i > lit) {
                parts = (AstNode **)realloc(parts, (np + 1) * sizeof(AstNode *));
                parts[np++] = ast_string(loam_dupn(text + lit, i - lit), loc);
            }
            char *ex = loam_dupn(text + i + 2, close - (i + 2));
            AstNode *e = parse_sub_expr(ex, loc.file, loc);
            if (!e) { free(parts); return NULL; }
            parts = (AstNode **)realloc(parts, (np + 1) * sizeof(AstNode *));
            parts[np++] = e;
            i = close + 2;
            lit = i;
            *had_holes = 1;
            continue;
        }
        if (i >= n) break;
        i++;
    }
    if (!*had_holes) { free(parts); return NULL; }
    if (n > lit) {
        parts = (AstNode **)realloc(parts, (np + 1) * sizeof(AstNode *));
        parts[np++] = ast_string(loam_dupn(text + lit, n - lit), loc);
    }
    AstNode *call = ast_call(ast_ident(loam_dup("__interp"), loc), parts, np, loc);
    free(text);
    return call;
}

/** Parse a type: `&`/`&mut`, `Box<T>`, `[N]T`, `[]T`, `fn(...)`, or a name. */
static AstNode *parse_type(Parser *p) {
    SourceLoc loc = p->current.loc;
    if (match(p, TOK_AMP)) {
        int mut = match(p, TOK_MUT);
        AstNode *e = parse_type(p);
        return ast_type(NULL, mut ? 1 : 0, e, 0, loc);
    }
    if (match(p, TOK_LBRACKET)) {
        if (match(p, TOK_RBRACKET)) {
            AstNode *e = parse_type(p);
            return ast_type(NULL, 3, e, -1, loc);
        }
        if (!check(p, TOK_NUMBER)) {
            error(p, "expected array length or []T");
            return NULL;
        }
        int64_t n = strtoll(p->current.start, NULL, 10);
        advance(p);
        consume(p, TOK_RBRACKET, "expected ]");
        AstNode *e = parse_type(p);
        return ast_type(NULL, 3, e, n, loc);
    }
    if (match(p, TOK_FN)) {
        consume(p, TOK_LPAREN, "expected ( after fn");
        AstNode **ps = NULL;
        size_t n = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                AstNode *t = parse_type(p);
                ps = (AstNode **)realloc(ps, (n + 1) * sizeof(AstNode *));
                ps[n++] = t;
            } while (match(p, TOK_COMMA));
        }
        consume(p, TOK_RPAREN, "expected )");
        AstNode *ret = NULL;
        if (match(p, TOK_ARROW)) ret = parse_type(p);
        AstNode *t = ast_type(NULL, 5, ret, 0, loc);
        t->as.type.fn_params = ps;
        t->as.type.fn_param_count = n;
        return t;
    }
    if (!match(p, TOK_IDENT)) {
        error(p, "expected type");
        return NULL;
    }
    char *nm = tok_text(p->previous);
    if (strcmp(nm, "Box") == 0 && match(p, TOK_LT)) {
        AstNode *e = parse_type(p);
        consume(p, TOK_GT, "expected >");
        free(nm);
        return ast_type(NULL, 4, e, 0, loc);
    }
    AstNode *t = ast_type(nm, 2, NULL, 0, loc);
    if (match(p, TOK_LT)) {
        AstNode **targs = NULL;
        size_t nt = 0;
        if (!check(p, TOK_GT)) {
            do {
                AstNode *a = parse_type(p);
                targs = (AstNode **)realloc(targs, (nt + 1) * sizeof(AstNode *));
                targs[nt++] = a;
            } while (match(p, TOK_COMMA));
        }
        consume(p, TOK_GT, "expected >");
        t->as.type.targs = targs;
        t->as.type.targ_count = nt;
    }
    return t;
}

/** `|x, y|` parameter list. Types are optional (`|x: int|`). */
static Param *parse_closure_params(Parser *p, size_t *out) {
    Param *params = NULL;
    size_t count = 0;
    if (check(p, TOK_PIPE)) {
        *out = 0;
        return NULL;
    }
    do {
        if (!match(p, TOK_IDENT)) {
            error(p, "expected closure parameter name");
            break;
        }
        char *name = tok_text(p->previous);
        SourceLoc nloc = p->previous.loc;
        AstNode *type = NULL;
        if (match(p, TOK_COLON)) type = parse_type(p);
        params = (Param *)realloc(params, (count + 1) * sizeof(Param));
        params[count].name = name;
        params[count].type = type;
        params[count].def = NULL;
        params[count].loc = nloc;
        count++;
    } while (match(p, TOK_COMMA));
    *out = count;
    return params;
}

/** `|x| { ... }` or `|| { ... }`. Opening `|` / `||` already consumed. */
static AstNode *parse_closure(Parser *p, SourceLoc loc, int zero_arg) {
    Param *ps = NULL;
    size_t pc = 0;
    if (!zero_arg) {
        ps = parse_closure_params(p, &pc);
        consume(p, TOK_PIPE, "expected | after closure parameters");
    }
    AstNode *ret = NULL;
    if (match(p, TOK_ARROW)) ret = parse_type(p);
    if (!check(p, TOK_LBRACE)) {
        error(p, "expected { after closure — bodies use braces, e.g. |x| { x + 1 }");
        return NULL;
    }
    AstNode *body = parse_block(p);
    AstNode *n = ast_fn(NULL, ps, pc, ret, body, loc);
    n->kind = AST_CLOSURE;
    return n;
}

/** Pure lookahead: does `p->current` (a `(`) start `(x, y) => { ... }`?
 * Never advances the real parser and never reports an error — on a `no`,
 * normal parenthesized-expr/tuple parsing runs completely unaffected. Only
 * matches a bare identifier list (no `: Type`, unlike `|x: int|`) so the
 * lookahead never has to reason about a type's own nested parens. `lex` is
 * plain data (no owned pointers), so copying it is a safe, cheap probe. */
static int arrow_closure_ahead(Parser *p) {
    if (p->current.kind != TOK_LPAREN) return 0;
    if (p->peek.kind == TOK_RPAREN) {
        Lexer probe = *p->lex;
        return lexer_next(&probe).kind == TOK_FAT_ARROW;
    }
    if (p->peek.kind != TOK_IDENT) return 0;
    Lexer probe = *p->lex;
    Token t = lexer_next(&probe);
    for (;;) {
        if (t.kind == TOK_COMMA) {
            t = lexer_next(&probe);
            if (t.kind != TOK_IDENT) return 0;
            t = lexer_next(&probe);
            continue;
        }
        if (t.kind == TOK_RPAREN) return lexer_next(&probe).kind == TOK_FAT_ARROW;
        return 0;
    }
}

/** `(x, y) => { ... }` / `() => { ... }` — sugar for `|x, y| { ... }` /
 * `|| { ... }`, same closure AST either way. `(` not yet consumed. */
static AstNode *parse_arrow_closure(Parser *p, SourceLoc loc) {
    advance(p); /* '(' */
    Param *params = NULL;
    size_t count = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            match(p, TOK_IDENT); /* arrow_closure_ahead guaranteed this holds */
            char *name = tok_text(p->previous);
            SourceLoc nloc = p->previous.loc;
            params = (Param *)realloc(params, (count + 1) * sizeof(Param));
            params[count].name = name;
            params[count].type = NULL;
            params[count].def = NULL;
            params[count].loc = nloc;
            count++;
        } while (match(p, TOK_COMMA));
    }
    consume(p, TOK_RPAREN, "expected )");
    consume(p, TOK_FAT_ARROW, "expected =>");
    if (!check(p, TOK_LBRACE)) {
        error(p, "expected { after => — bodies use braces, e.g. (x) => { x + 1 }");
        return NULL;
    }
    AstNode *body = parse_block(p);
    AstNode *n = ast_fn(NULL, params, count, NULL, body, loc);
    n->kind = AST_CLOSURE;
    return n;
}

/** `fn(a: T, b) -> R => expr` or `fn(...) { ... }`. `fn` already consumed;
 * `(` is current. Parameter types are optional — an omitted one is inferred
 * from the expected type, exactly as for `|x| { ... }`. */
static AstNode *parse_fn_closure(Parser *p, SourceLoc loc) {
    consume(p, TOK_LPAREN, "expected ( after fn");
    Param *ps = NULL;
    size_t pc = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            if (!match(p, TOK_IDENT)) {
                error(p, "expected closure parameter name");
                break;
            }
            char *name = tok_text(p->previous);
            SourceLoc nloc = p->previous.loc;
            AstNode *ty = NULL;
            if (match(p, TOK_COLON)) ty = parse_type(p);
            ps = (Param *)realloc(ps, (pc + 1) * sizeof(Param));
            ps[pc].name = name;
            ps[pc].type = ty;
            ps[pc].def = NULL;
            ps[pc].loc = nloc;
            pc++;
        } while (match(p, TOK_COMMA));
    }
    consume(p, TOK_RPAREN, "expected ) after closure parameters");
    AstNode *ret = NULL;
    if (match(p, TOK_ARROW)) ret = parse_type(p);
    AstNode *body;
    if (match(p, TOK_FAT_ARROW)) {
        SourceLoc bloc = p->current.loc;
        AstNode *e = parse_expr(p);
        AstNode **st = (AstNode **)malloc(sizeof(AstNode *));
        st[0] = ast_expr_stmt(e, bloc);
        body = ast_block(st, 1, bloc);
    } else if (check(p, TOK_LBRACE)) {
        body = parse_block(p);
    } else {
        error(p, "expected => or { after fn(...) closure");
        return NULL;
    }
    AstNode *n = ast_fn(NULL, ps, pc, ret, body, loc);
    n->kind = AST_CLOSURE;
    return n;
}

/** `if c { a } else { b }` in expression position. `else` is required; both
 * branches must end in an expression. `else if` nests as a one-statement
 * block so the AST shape stays `if / block / block`. */
static AstNode *parse_if_expr(Parser *p) {
    SourceLoc loc = p->current.loc;
    consume(p, TOK_IF, "expected if");
    AstNode *cond = parse_cond_expr(p);
    AstNode *thenb = parse_block(p);
    if (!match(p, TOK_ELSE)) {
        error(p, "an `if` used as a value needs an `else` branch");
        return NULL;
    }
    AstNode *elseb;
    if (check(p, TOK_IF)) {
        SourceLoc nloc = p->current.loc;
        AstNode *nested = parse_if_expr(p);
        AstNode **st = (AstNode **)malloc(sizeof(AstNode *));
        st[0] = ast_expr_stmt(nested, nloc);
        elseb = ast_block(st, 1, nloc);
    } else {
        elseb = parse_block(p);
    }
    return ast_if(cond, thenb, elseb, loc);
}

/** Literals, ident, parenthesized expr, array lit, or `|x| { }` /
 * `(x) => { }` closure. */
static AstNode *parse_primary(Parser *p) {
    SourceLoc loc = p->current.loc;
    if (match(p, TOK_IDENT)) {
        char *nm = tok_text(p->previous);
        if (p->allow_struct_lit && match(p, TOK_LBRACE)) {
            FieldInit *fi = NULL;
            size_t fc = 0;
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                if (!match(p, TOK_IDENT)) {
                    error(p, "expected field name");
                    break;
                }
                char *fn = tok_text(p->previous);
                consume(p, TOK_COLON, "expected : in struct literal");
                AstNode *v = parse_expr(p);
                fi = (FieldInit *)realloc(fi, (fc + 1) * sizeof(FieldInit));
                fi[fc].name = fn;
                fi[fc].init = v;
                fc++;
                if (!match(p, TOK_COMMA)) break;
            }
            consume(p, TOK_RBRACE, "expected }");
            return ast_struct_lit(nm, fi, fc, loc);
        }
        return ast_ident(nm, loc);
    }
    if (match(p, TOK_FLOAT)) {
        double v = strtod(p->previous.start, NULL);
        return ast_float(v, loc);
    }
    if (match(p, TOK_NUMBER)) {
        int64_t v = tok_int(p->previous);
        return ast_number(v, loc);
    }
    if (match(p, TOK_STRING)) {
        char *raw = unescape_string(p->previous);
        int holes = 0;
        AstNode *in = interp_node(raw, loc, &holes);
        if (in) return in;
        if (holes) { error(p, "bad {{ }} interpolation"); return NULL; }
        return ast_string(raw, loc);
    }
    if (match(p, TOK_TRUE)) return ast_bool(1, loc);
    if (match(p, TOK_FALSE)) return ast_bool(0, loc);
    /* `fn(x: T) => expr` / `fn(x: T) { ... }` — the spec's event-handler
       spelling. Same closure AST as `|x| { ... }`. */
    if (check(p, TOK_FN) && p->peek.kind == TOK_LPAREN) {
        advance(p);
        return parse_fn_closure(p, loc);
    }
    /* `if c { a } else { b }` as a value. Statement `if` never gets here. */
    if (check(p, TOK_IF)) return parse_if_expr(p);
    /* `|x| { ... }` / `|| { ... }`. `a || b` stays boolean or (infix). */
    if (match(p, TOK_PIPE_PIPE)) return parse_closure(p, loc, 1);
    if (match(p, TOK_PIPE)) return parse_closure(p, loc, 0);
    if (check(p, TOK_LPAREN) && arrow_closure_ahead(p)) {
        return parse_arrow_closure(p, loc);
    }
    if (match(p, TOK_LPAREN)) {
        if (match(p, TOK_RPAREN)) {
            error(p, "empty tuple");
            return NULL;
        }
        AstNode *e = parse_expr(p);
        if (!match(p, TOK_COMMA)) {
            consume(p, TOK_RPAREN, "expected )");
            return e;
        }
        AstNode **elems = (AstNode **)malloc(sizeof(AstNode *));
        elems[0] = e;
        size_t n = 1;
        if (!check(p, TOK_RPAREN)) {
            do {
                AstNode *x = parse_expr(p);
                elems = (AstNode **)realloc(elems, (n + 1) * sizeof(AstNode *));
                elems[n++] = x;
            } while (match(p, TOK_COMMA) && !check(p, TOK_RPAREN));
        }
        consume(p, TOK_RPAREN, "expected )");
        return ast_tuple(elems, n, loc);
    }
    if (match(p, TOK_LBRACKET)) {
        int64_t n = -1;
        if (match(p, TOK_RBRACKET)) {
            n = -1;
        } else {
            if (!check(p, TOK_NUMBER)) {
                error(p, "expected array literal [N]T{...} or []T{...}");
                return NULL;
            }
            n = strtoll(p->current.start, NULL, 10);
            advance(p);
            consume(p, TOK_RBRACKET, "expected ]");
        }
        AstNode *et = parse_type(p);
        consume(p, TOK_LBRACE, "expected {");
        AstNode **elems = NULL;
        size_t ec = 0;
        if (!check(p, TOK_RBRACE)) {
            do {
                AstNode *e = parse_expr(p);
                elems = (AstNode **)realloc(elems, (ec + 1) * sizeof(AstNode *));
                elems[ec++] = e;
            } while (match(p, TOK_COMMA) && !check(p, TOK_RBRACE));
        }
        consume(p, TOK_RBRACE, "expected }");
        return ast_array_lit(et, n, elems, ec, loc);
    }
    error(p, "expected expression");
    return NULL;
}

/** Calls, `.field`, `::path`, and `[index]` after a primary. */
static AstNode *parse_postfix(Parser *p, AstNode *left) {
    for (;;) {
        SourceLoc loc = p->current.loc;
        if (match(p, TOK_LPAREN)) {
            AstNode **args = NULL;
            size_t ac = 0;
            FieldInit *props = NULL;
            size_t pn = 0;
            if (!check(p, TOK_RPAREN)) {
                do {
                    /* `name = value` is a named argument: it joins the props
                       struct built for the callee's last parameter. */
                    if (check(p, TOK_IDENT) && p->peek.kind == TOK_EQ) {
                        char *pname = tok_text(p->current);
                        advance(p);
                        advance(p);
                        AstNode *v = parse_expr(p);
                        props = (FieldInit *)realloc(props, (pn + 1) * sizeof(FieldInit));
                        props[pn].name = pname;
                        props[pn].init = v;
                        pn++;
                        continue;
                    }
                    if (pn) {
                        error(p, "positional argument after a named one");
                        break;
                    }
                    AstNode *a = parse_expr(p);
                    args = (AstNode **)realloc(args, (ac + 1) * sizeof(AstNode *));
                    args[ac++] = a;
                } while (match(p, TOK_COMMA) && !check(p, TOK_RPAREN));
            }
            consume(p, TOK_RPAREN, "expected )");
            loc.end_line = p->previous.loc.end_line;
            loc.end_col = p->previous.loc.end_col;
            if (left && left->loc.line > 0) {
                loc.line = left->loc.line;
                loc.col = left->loc.col;
                loc.file = left->loc.file;
            }
            if (pn) {
                /* type_name is filled in by typecheck from the callee's
                   last parameter, which must be a struct. */
                AstNode *lit = ast_struct_lit(NULL, props, pn, loc);
                args = (AstNode **)realloc(args, (ac + 1) * sizeof(AstNode *));
                args[ac++] = lit;
            }
            left = ast_call(left, args, ac, loc);
            /* Trailing block: `C(...) { ... }` is the UI hierarchy. The `{`
               must open on the line the `)` closed on, so a following
               standalone block stays its own statement. */
            if (p->allow_struct_lit && check(p, TOK_LBRACE) &&
                p->current.loc.line == p->previous.loc.end_line) {
                SourceLoc bloc = p->current.loc;
                AstNode *body = parse_block(p);
                AstNode *clos = ast_fn(NULL, NULL, 0, NULL, body, bloc);
                clos->kind = AST_CLOSURE;
                AstNode **sa = (AstNode **)malloc(2 * sizeof(AstNode *));
                sa[0] = left;
                sa[1] = clos;
                left = ast_call(ast_ident(loam_dup("slot"), loc), sa, 2, loc);
            }
            continue;
        }
        if (match(p, TOK_DOT)) {
            if (!match(p, TOK_IDENT)) {
                /* While typing `recv.` the next token is often `}` / EOF. */
                if (loam_diag_capturing() &&
                    (check(p, TOK_EOF) || check(p, TOK_RBRACE) || check(p, TOK_RPAREN) ||
                     check(p, TOK_COMMA) || check(p, TOK_RBRACKET) || check(p, TOK_SEMICOLON)))
                    break;
                error(p, "expected field name");
                break;
            }
            left = ast_field(left, tok_text(p->previous), 0, p->previous.loc);
            continue;
        }
        if (match(p, TOK_COLON_COLON)) {
            if (!match(p, TOK_IDENT)) {
                if (loam_diag_capturing() &&
                    (check(p, TOK_EOF) || check(p, TOK_RBRACE) || check(p, TOK_RPAREN) ||
                     check(p, TOK_COMMA) || check(p, TOK_RBRACKET) || check(p, TOK_SEMICOLON)))
                    break;
                error(p, "expected identifier after ::");
                break;
            }
            left = ast_field(left, tok_text(p->previous), 1, p->previous.loc);
            continue;
        }
        if (match(p, TOK_LBRACKET)) {
            AstNode *idx = parse_expr(p);
            consume(p, TOK_RBRACKET, "expected ]");
            left = ast_index(left, idx, loc);
            continue;
        }
        if (match(p, TOK_AS)) {
            AstNode *ty = parse_type(p);
            left = ast_cast(left, ty, loc);
            continue;
        }
        /* Postfix ++/--: `i++` / `i--`. */
        if (match(p, TOK_PLUS_PLUS) || match(p, TOK_MINUS_MINUS)) {
            int dec = p->previous.kind == TOK_MINUS_MINUS;
            left = ast_incdec(left, dec, 1, loc);
            continue;
        }
        break;
    }
    return left;
}

/** Unary `&`, `&mut`, `*`, `!`, `-`. */
static AstNode *parse_unary(Parser *p) {
    SourceLoc loc = p->current.loc;
    /* `await <expr>` — JS-shaped sugar: desugars to `async.await_value(<expr>)`,
       a typed call resolved against std:async. Contextual: only when an
       expression starts with the identifier `await`. */
    if (check(p, TOK_IDENT) && strcmp(tok_text(p->current), "await") == 0) {
        advance(p);
        AstNode *opnd = parse_unary(p);
        if (!opnd) return NULL;
        AstNode *callee = ast_field(ast_ident(loam_dup("async"), loc), loam_dup("await_value"),
                                    0, loc);
        AstNode **args = (AstNode **)malloc(sizeof(AstNode *));
        args[0] = opnd;
        AstNode *call = ast_call(callee, args, 1, loc);
        call->flags |= ASTF_AWAIT;
        return call;
    }
    if (match(p, TOK_AMP)) {
        int mut = match(p, TOK_MUT);
        return ast_addr(parse_unary(p), mut, loc);
    }
    if (match(p, TOK_STAR)) return ast_deref(parse_unary(p), loc);
    if (match(p, TOK_BANG) || match(p, TOK_MINUS) || match(p, TOK_TILDE)) {
        TokenKind op = p->previous.kind;
        return ast_unary(op, parse_unary(p), loc);
    }
    /* Prefix ++/--: `++i` / `--i`. */
    if (match(p, TOK_PLUS_PLUS) || match(p, TOK_MINUS_MINUS)) {
        int dec = p->previous.kind == TOK_MINUS_MINUS;
        return ast_incdec(parse_unary(p), dec, 0, loc);
    }
    return parse_postfix(p, parse_primary(p));
}

/** Binding power for Pratt parsing of binary operators. C-compatible order
 *  (low -> high): ||, &&, |, ^, &, equality, relational, shifts, additive,
 *  multiplicative. `..` stays lowest (range sugar). */
static int precedence(TokenKind k) {
    if (k == TOK_DOT_DOT) return 1;
    if (k == TOK_PIPE_PIPE) return 2;
    if (k == TOK_AMP_AMP) return 3;
    if (k == TOK_PIPE) return 4;
    if (k == TOK_CARET) return 5;
    if (k == TOK_AMP) return 6;
    if (k == TOK_EQ_EQ || k == TOK_BANG_EQ) return 7;
    if (k == TOK_LT || k == TOK_GT || k == TOK_LT_EQ || k == TOK_GT_EQ) return 8;
    if (k == TOK_SHL || k == TOK_SHR) return 9;
    if (k == TOK_PLUS || k == TOK_MINUS) return 10;
    if (k == TOK_STAR || k == TOK_SLASH || k == TOK_PERCENT) return 11;
    return 0;
}

/** Binary expression with precedence >= min_prec. */
static AstNode *parse_binary(Parser *p, int min_prec) {
    AstNode *left = parse_unary(p);
    for (;;) {
        TokenKind op = p->current.kind;
        int pr = precedence(op);
        if (pr < min_prec || pr == 0) break;
        /* do not treat a line-leading operator as continuing the previous expr
           (`let x = Box::new(1)` then `*x += 1` is two statements) */
        if (p->current.loc.line > p->previous.loc.line) break;
        advance(p);
        AstNode *right = parse_binary(p, pr + 1);
        left = ast_binary(op, left, right, left ? left->loc : p->previous.loc);
    }
    return left;
}

static AstNode *parse_expr(Parser *p) { return parse_binary(p, 1); }

/** Expression used as `if`/`for` condition: no struct-lit at this `{`. */
static AstNode *parse_cond_expr(Parser *p) {
    int saved = p->allow_struct_lit;
    p->allow_struct_lit = 0;
    AstNode *e = parse_expr(p);
    p->allow_struct_lit = saved;
    return e;
}

/** `let` / `let mut` / `const` binding. A `const` is an immutable binding
 *  whose initializer is a constant expression — the spelling props use. */
static AstNode *parse_let(Parser *p) {
    SourceLoc loc = p->previous.loc;
    int is_const = p->previous.kind == TOK_CONST;
    if (is_const && check(p, TOK_MUT)) {
        error(p, "a `const` cannot be `mut`");
        return NULL;
    }
    int is_mut = match(p, TOK_MUT);
    if (!match(p, TOK_IDENT)) {
        error(p, "expected identifier after let");
        return NULL;
    }
    char *name = tok_text(p->previous);
    loc.end_line = p->previous.loc.end_line;
    loc.end_col = p->previous.loc.end_col;
    AstNode *type = NULL;
    if (match(p, TOK_COLON)) type = parse_type(p);
    consume(p, TOK_EQ, "expected =");
    AstNode *init = parse_expr(p);
    optional_semi(p);
    return ast_var(name, type, init, is_mut, loc);
}

static int is_assign_op(TokenKind k) {
    return k == TOK_EQ || k == TOK_PLUS_EQ || k == TOK_MINUS_EQ ||
           k == TOK_STAR_EQ || k == TOK_SLASH_EQ || k == TOK_PERCENT_EQ ||
           k == TOK_AMP_EQ || k == TOK_PIPE_EQ || k == TOK_CARET_EQ ||
           k == TOK_SHL_EQ || k == TOK_SHR_EQ;
}

/** One match pattern: literal, enum constant (`LOOK.Solid`), or `_`. */
static AstNode *parse_match_pat(Parser *p, int *wild) {
    SourceLoc loc = p->current.loc;
    *wild = 0;
    if (match(p, TOK_IDENT)) {
        char *nm = tok_text(p->previous);
        if (strcmp(nm, "_") == 0) {
            free(nm);
            *wild = 1;
            return NULL;
        }
        /* `Enum.Member` is a compile-time constant, so it is a legal pattern
           and keeps the arm readable — matching on raw 0/1/2 would not. */
        if (match(p, TOK_DOT)) {
            if (!match(p, TOK_IDENT)) {
                free(nm);
                error(p, "expected a name after `.` in a match pattern");
                return NULL;
            }
            char *field = tok_text(p->previous);
            loc.end_line = p->previous.loc.end_line;
            loc.end_col = p->previous.loc.end_col;
            /* ast_ident / ast_field take ownership of these names. */
            return ast_field(ast_ident(nm, loc), field, 0, loc);
        }
        free(nm);
        error(p, "match patterns are literals, `Enum.Member`, or `_`");
        return NULL;
    }
    if (match(p, TOK_NUMBER)) {
        int64_t v = tok_int(p->previous);
        return ast_number(v, loc);
    }
    if (match(p, TOK_FLOAT)) {
        double v = strtod(p->previous.start, NULL);
        return ast_float(v, loc);
    }
    if (match(p, TOK_TRUE)) return ast_bool(1, loc);
    if (match(p, TOK_FALSE)) return ast_bool(0, loc);
    if (match(p, TOK_STRING)) {
        char *raw = unescape_string(p->previous);
        int holes = 0;
        AstNode *in = interp_node(raw, loc, &holes);
        if (in) return in;
        if (holes) { error(p, "bad {{ }} interpolation"); return NULL; }
        return ast_string(raw, loc);
    }
    error(p, "expected match pattern");
    return NULL;
}

static AstNode *parse_match_arm(Parser *p) {
    SourceLoc loc = p->current.loc;
    int wild = 0;
    AstNode **pats = NULL;
    size_t np = 0;
    int w0 = 0;
    AstNode *first = parse_match_pat(p, &w0);
    if (w0) {
        wild = 1;
        if (check(p, TOK_PIPE)) {
            error(p, "wildcard `_` cannot be in an or-pattern");
            return NULL;
        }
    } else if (first) {
        pats = (AstNode **)malloc(sizeof(AstNode *));
        pats[0] = first;
        np = 1;
        while (match(p, TOK_PIPE)) {
            int w = 0;
            AstNode *q = parse_match_pat(p, &w);
            if (w) {
                error(p, "wildcard `_` cannot be in an or-pattern");
                break;
            }
            if (q) {
                pats = (AstNode **)realloc(pats, (np + 1) * sizeof(AstNode *));
                pats[np++] = q;
            }
        }
    }
    consume(p, TOK_FAT_ARROW, "expected => after match pattern");
    AstNode *body = parse_block(p);
    return ast_match_arm(pats, np, wild, body, loc);
}

/** One statement: if, for, while, match, return, break, continue, let, assign, or expression. */
static AstNode *parse_stmt(Parser *p) {
    char *doc = take_docs(p);
    SourceLoc loc = p->current.loc;
    if (match(p, TOK_IF)) {
        free(doc);
        AstNode *cond = parse_cond_expr(p);
        AstNode *thenb = parse_block(p);
        AstNode *elseb = NULL;
        if (match(p, TOK_ELSE)) {
            if (check(p, TOK_IF)) elseb = parse_stmt(p);
            else elseb = parse_block(p);
        }
        return ast_if(cond, thenb, elseb, loc);
    }
    if (match(p, TOK_FOR)) {
        free(doc);
        if (!match(p, TOK_IDENT)) {
            error(p, "expected loop variable");
            return NULL;
        }
        char *var = tok_text(p->previous);
        consume(p, TOK_IN, "expected in");
        AstNode *iter = parse_cond_expr(p);
        AstNode *body = parse_block(p);
        return ast_for(var, iter, body, loc);
    }
    if (match(p, TOK_WHILE)) {
        free(doc);
        AstNode *cond = parse_cond_expr(p);
        AstNode *body = parse_block(p);
        return ast_while(cond, body, loc);
    }
    if (match(p, TOK_MATCH)) {
        free(doc);
        AstNode *scrut = parse_cond_expr(p);
        consume(p, TOK_LBRACE, "expected { after match");
        AstNode **arms = NULL;
        size_t na = 0;
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            AstNode *arm = parse_match_arm(p);
            if (arm) {
                arms = (AstNode **)realloc(arms, (na + 1) * sizeof(AstNode *));
                arms[na++] = arm;
            }
            if (p->had_error) break;
        }
        consume(p, TOK_RBRACE, "expected }");
        return ast_match(scrut, arms, na, loc);
    }
    if (match(p, TOK_BREAK)) {
        free(doc);
        optional_semi(p);
        return ast_break(loc);
    }
    if (match(p, TOK_CONTINUE)) {
        free(doc);
        optional_semi(p);
        return ast_continue(loc);
    }
    if (match(p, TOK_RETURN)) {
        free(doc);
        AstNode *expr = NULL;
        if (!check(p, TOK_SEMICOLON) && !check(p, TOK_RBRACE)) expr = parse_expr(p);
        optional_semi(p);
        return ast_return(expr, loc);
    }
    if (match(p, TOK_LET) || match(p, TOK_CONST)) {
        AstNode *v = parse_let(p);
        if (v) v->doc = doc;
        else free(doc);
        return v;
    }
    free(doc);

    AstNode *e = parse_expr(p);
    if (is_assign_op(p->current.kind)) {
        TokenKind op = p->current.kind;
        advance(p);
        AstNode *r = parse_expr(p);
        optional_semi(p);
        return ast_assign(op, e, r, loc);
    }
    optional_semi(p);
    return ast_expr_stmt(e, loc);
}

/** `{` statements `}`. A comma after a statement is allowed so UI children
 *  can be written as a list: `Box(...) { A(), B(), }`. */
static AstNode *parse_block(Parser *p) {
    SourceLoc loc = p->current.loc;
    consume(p, TOK_LBRACE, "expected {");
    AstNode **stmts = NULL;
    size_t count = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode *s = parse_stmt(p);
        if (s) {
            stmts = (AstNode **)realloc(stmts, (count + 1) * sizeof(AstNode *));
            stmts[count++] = s;
        }
        if (p->had_error) break;
        match(p, TOK_COMMA);
    }
    consume(p, TOK_RBRACE, "expected }");
    return ast_block(stmts, count, loc);
}

static Param *parse_params(Parser *p, size_t *out) {
    Param *params = NULL;
    size_t count = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            if (!match(p, TOK_IDENT)) {
                error(p, "expected parameter name");
                break;
            }
            char *name = tok_text(p->previous);
            SourceLoc nloc = p->previous.loc;
            consume(p, TOK_COLON, "expected :");
            AstNode *type = parse_type(p);
            AstNode *def = NULL;
            if (match(p, TOK_EQ)) def = parse_expr(p);
            params = (Param *)realloc(params, (count + 1) * sizeof(Param));
            params[count].name = name;
            params[count].type = type;
            params[count].def = def;
            params[count].loc = nloc;
            count++;
        } while (match(p, TOK_COMMA));
    }
    *out = count;
    return params;
}

/** `fn` item: optional type params, params, return type, body. */
static AstNode *parse_fn(Parser *p) {
    SourceLoc loc = p->previous.loc;
    if (!match(p, TOK_IDENT)) {
        error(p, "expected function name");
        return NULL;
    }
    char *name = tok_text(p->previous);
    loc.end_line = p->previous.loc.end_line;
    loc.end_col = p->previous.loc.end_col;
    const char **tparams = NULL;
    size_t nt = 0;
    if (match(p, TOK_LT)) {
        do {
            if (!match(p, TOK_IDENT)) {
                error(p, "expected type parameter name");
                break;
            }
            tparams = (const char **)realloc(tparams, (nt + 1) * sizeof(char *));
            tparams[nt++] = tok_text(p->previous);
        } while (match(p, TOK_COMMA));
        consume(p, TOK_GT, "expected >");
    }
    consume(p, TOK_LPAREN, "expected (");
    size_t pc = 0;
    Param *ps = parse_params(p, &pc);
    consume(p, TOK_RPAREN, "expected )");
    AstNode *ret = NULL;
    if (match(p, TOK_ARROW)) ret = parse_type(p);
    AstNode *body = parse_block(p);
    AstNode *n = ast_fn(name, ps, pc, ret, body, loc);
    n->as.fn.tparams = tparams;
    n->as.fn.tparam_count = nt;
    return n;
}

/** `struct Name { field: Type, ... }` or `struct Name<T> { ... }`. */
static AstNode *parse_struct(Parser *p) {
    SourceLoc loc = p->previous.loc;
    if (!match(p, TOK_IDENT)) {
        error(p, "expected struct name");
        return NULL;
    }
    char *name = tok_text(p->previous);
    loc.end_line = p->previous.loc.end_line;
    loc.end_col = p->previous.loc.end_col;
    const char **tparams = NULL;
    size_t nt = 0;
    if (match(p, TOK_LT)) {
        do {
            if (!match(p, TOK_IDENT)) {
                error(p, "expected type parameter name");
                break;
            }
            tparams = (const char **)realloc(tparams, (nt + 1) * sizeof(char *));
            tparams[nt++] = tok_text(p->previous);
        } while (match(p, TOK_COMMA));
        consume(p, TOK_GT, "expected >");
    }
    consume(p, TOK_LBRACE, "expected {");
    Field *fields = NULL;
    size_t fc = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        char *fdoc = take_docs(p);
        if (!match(p, TOK_IDENT)) {
            free(fdoc);
            error(p, "expected field name");
            break;
        }
        char *fn = tok_text(p->previous);
        consume(p, TOK_COLON, "expected :");
        AstNode *ty = parse_type(p);
        AstNode *def = NULL;
        if (match(p, TOK_EQ)) def = parse_expr(p);
        fields = (Field *)realloc(fields, (fc + 1) * sizeof(Field));
        fields[fc].name = fn;
        fields[fc].type = ty;
        fields[fc].def = def;
        fields[fc].doc = fdoc;
        fc++;
        match(p, TOK_COMMA);
        match(p, TOK_SEMICOLON);
    }
    consume(p, TOK_RBRACE, "expected }");
    AstNode *st = ast_struct(name, fields, fc, loc);
    st->as.strct.tparams = tparams;
    st->as.strct.tparam_count = nt;
    return st;
}

/** `enum Name { A, B = 7, C }` — variants are int constants, auto-numbered
 * from 0 (or from the last explicit value). `Name.A` lowers to that int. */
static AstNode *parse_enum(Parser *p) {
    SourceLoc loc = p->previous.loc;
    if (!match(p, TOK_IDENT)) {
        error(p, "expected enum name");
        return NULL;
    }
    char *name = tok_text(p->previous);
    loc.end_line = p->previous.loc.end_line;
    loc.end_col = p->previous.loc.end_col;
    consume(p, TOK_LBRACE, "expected {");
    const char **vn = NULL;
    int64_t *vals = NULL;
    size_t n = 0;
    int64_t next = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (!match(p, TOK_IDENT)) {
            error(p, "expected enum variant name");
            break;
        }
        char *vname = tok_text(p->previous);
        int64_t v = next;
        if (match(p, TOK_EQ)) {
            int neg = match(p, TOK_MINUS);
            if (!match(p, TOK_NUMBER)) {
                error(p, "enum variant value must be an integer literal");
                free(vname);
                break;
            }
            v = tok_int(p->previous);
            if (neg) v = -v;
        }
        next = v + 1;
        vn = (const char **)realloc(vn, (n + 1) * sizeof(char *));
        vals = (int64_t *)realloc(vals, (n + 1) * sizeof(int64_t));
        vn[n] = vname;
        vals[n] = v;
        n++;
        match(p, TOK_COMMA);
        match(p, TOK_SEMICOLON);
    }
    consume(p, TOK_RBRACE, "expected }");
    return ast_enum(name, vn, vals, n, loc);
}

/** The module alias a resolved import spec binds to: the `std:`/`pkg:` name, or
    the file stem for a relative path. NULL (with an error) if the spec is bad. */
static char *alias_from_path(Parser *p, const char *path) {
    if (strncmp(path, "std:", 4) == 0) {
        const char *name = path + 4;
        if (!name[0] || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) {
            error(p, "std import must be import \"std:name\"");
            return NULL;
        }
        return loam_dup(name);
    }
    if (strncmp(path, "pkg:", 4) == 0) {
        const char *name = path + 4;
        if (!name[0] || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) {
            error(p, "package import must be import \"pkg:name\"");
            return NULL;
        }
        return loam_dup(name);
    }
    /* `package:module` — the alias is the module, not the package. */
    {
        const char *colon = strchr(path, ':');
        if (colon) {
            char *alias = file_stem(colon + 1);
            if (!alias || !alias[0]) {
                error(p, "import spec needs a module after ':'");
                free(alias);
                return NULL;
            }
            return alias;
        }
    }
    char *alias = file_stem(path);
    if (!alias || !alias[0]) {
        error(p, "import path has no module name");
        free(alias);
        return NULL;
    }
    return alias;
}

/** `import { A, B as C } from "path"` — the `{` has been consumed. */
static AstNode *parse_named_import(Parser *p, SourceLoc loc) {
    const char **names = NULL;
    const char **locals = NULL;
    size_t n = 0;
    if (!check(p, TOK_RBRACE)) {
        do {
            if (!match(p, TOK_IDENT)) {
                error(p, "expected a name in the import list");
                break;
            }
            char *src = tok_text(p->previous);
            char *local = loam_dup(src);
            if (match(p, TOK_AS)) {
                if (match(p, TOK_IDENT)) {
                    free(local);
                    local = tok_text(p->previous);
                } else {
                    error(p, "expected a local name after 'as'");
                }
            }
            names = (const char **)realloc(names, (n + 1) * sizeof(char *));
            locals = (const char **)realloc(locals, (n + 1) * sizeof(char *));
            names[n] = src;
            locals[n] = local;
            n++;
        } while (match(p, TOK_COMMA));
    }
    consume(p, TOK_RBRACE, "expected } to close the import list");
    if (!check(p, TOK_IDENT) || p->current.len != 4 || strncmp(p->current.start, "from", 4) != 0) {
        error(p, "expected 'from' before the import path");
    } else {
        advance(p);
    }
    if (!match(p, TOK_STRING)) {
        error(p, "expected import path string after 'from'");
        for (size_t i = 0; i < n; i++) {
            free((void *)names[i]);
            free((void *)locals[i]);
        }
        free(names);
        free(locals);
        return NULL;
    }
    loc.end_line = p->previous.loc.end_line;
    loc.end_col = p->previous.loc.end_col;
    char *path = unescape_string(p->previous);
    char *alias = alias_from_path(p, path);
    if (!alias) {
        for (size_t i = 0; i < n; i++) {
            free((void *)names[i]);
            free((void *)locals[i]);
        }
        free(names);
        free(locals);
        free(path);
        return NULL;
    }
    return ast_import_names(alias, path, names, locals, n, loc);
}

/** `import "std:foo"` / `import "rel/path.loam"` (either with `as name`), or a
    selective `import { A, B as C } from "path"`. */
static AstNode *parse_import(Parser *p) {
    SourceLoc loc = p->previous.loc;
    if (match(p, TOK_LBRACE)) return parse_named_import(p, loc);
    if (match(p, TOK_IDENT)) {
        error(p, "import requires a quoted path, e.g. import \"std:zeus\" or import \"foo.loam\"");
        return NULL;
    }
    if (!match(p, TOK_STRING)) {
        error(p, "expected import path string after import");
        return NULL;
    }
    loc.end_line = p->previous.loc.end_line;
    loc.end_col = p->previous.loc.end_col;
    char *path = unescape_string(p->previous);
    char *alias = alias_from_path(p, path);
    if (!alias) {
        free(path);
        return NULL;
    }
    /* `import "path.loam" as name` — needed when two files share a stem
       (file-system routing: many `page.loam`). */
    if (match(p, TOK_AS)) {
        if (!match(p, TOK_IDENT)) {
            error(p, "expected a module name after 'as'");
            free(path);
            free(alias);
            return NULL;
        }
        free(alias);
        alias = tok_text(p->previous);
    }
    return ast_import(alias, path, loc);
}

/** Module: zero or more imports, then fn/struct/let items, until EOF. */
AstNode *parser_parse(Parser *p) {
    SourceLoc loc = p->current.loc;
    AstNode **imps = NULL;
    size_t ni = 0;
    AstNode **decls = NULL;
    size_t nd = 0;
    char *mod_doc = take_mod_docs(p);

    for (;;) {
        char *more = take_mod_docs(p);
        if (more) mod_doc = doc_join(mod_doc, more);
        if (check(p, TOK_EOF) || p->had_error) break;
        char *doc = take_docs(p);
        if (match(p, TOK_IMPORT)) {
            if (nd) {
                error(p, "import must appear at the top of the file");
                free(doc);
                break;
            }
            AstNode *im = parse_import(p);
            optional_semi(p);
            if (im) {
                im->doc = doc;
                imps = (AstNode **)realloc(imps, (ni + 1) * sizeof(AstNode *));
                imps[ni++] = im;
            } else {
                free(doc);
            }
            continue;
        }
        int is_proto = 0;
        int is_test = 0;
        int is_server = 0;
        int is_json = 0;
        int is_must_check = 0;
        if (match(p, TOK_HASH)) {
            consume(p, TOK_LBRACKET, "expected [ after #");
            if (!match(p, TOK_IDENT)) {
                error(p, "expected attribute name");
                free(doc);
                break;
            }
            char *attr = tok_text(p->previous);
            if (strcmp(attr, "proto") == 0) {
                is_proto = 1;
            } else if (strcmp(attr, "test") == 0) {
                is_test = 1;
            } else if (strcmp(attr, "server") == 0) {
                is_server = 1;
            } else if (strcmp(attr, "json") == 0) {
                is_json = 1;
            } else if (strcmp(attr, "must_check") == 0) {
                is_must_check = 1;
            } else {
                char msg[96];
                snprintf(msg, sizeof msg, "unknown attribute '%s'", attr);
                error(p, msg);
                free(attr);
                free(doc);
                break;
            }
            free(attr);
            consume(p, TOK_RBRACKET, "expected ] after attribute");
            if (is_proto && !check(p, TOK_STRUCT)) {
                error(p, "#[proto] can only be applied to a struct");
                free(doc);
                break;
            }
            if (is_test && !check(p, TOK_FN)) {
                error(p, "#[test] can only be applied to a fn");
                free(doc);
                break;
            }
            if (is_server && !check(p, TOK_FN)) {
                error(p, "#[server] can only be applied to a fn");
                free(doc);
                break;
            }
            if (is_json && !check(p, TOK_STRUCT)) {
                error(p, "#[json] can only be applied to a struct");
                free(doc);
                break;
            }
            if (is_must_check && !check(p, TOK_STRUCT)) {
                error(p, "#[must_check] can only be applied to a struct");
                free(doc);
                break;
            }
        }
        if (match(p, TOK_FN)) {
            AstNode *fn = parse_fn(p);
            if (fn) {
                fn->doc = doc;
                fn->as.fn.is_test = is_test;
                fn->as.fn.is_server = is_server;
                decls = (AstNode **)realloc(decls, (nd + 1) * sizeof(AstNode *));
                decls[nd++] = fn;
            } else {
                free(doc);
            }
            continue;
        }
        /* `async fn name(...)` — a fn whose body may `await`. Contextual
           keyword: only in decl position before `fn`. */
        if (check(p, TOK_IDENT) && p->peek.kind == TOK_FN &&
            strcmp(tok_text(p->current), "async") == 0) {
            advance(p);
            if (!match(p, TOK_FN)) {
                error(p, "expected fn after async");
                free(doc);
                continue;
            }
            AstNode *fn = parse_fn(p);
            if (fn) {
                fn->as.fn.is_async = 1;
                fn->doc = doc;
                decls = (AstNode **)realloc(decls, (nd + 1) * sizeof(AstNode *));
                decls[nd++] = fn;
            } else {
                free(doc);
            }
            continue;
        }
        if (match(p, TOK_ENUM)) {
            AstNode *en = parse_enum(p);
            if (en) {
                en->doc = doc;
                decls = (AstNode **)realloc(decls, (nd + 1) * sizeof(AstNode *));
                decls[nd++] = en;
            } else {
                free(doc);
            }
            continue;
        }
        if (match(p, TOK_STRUCT)) {
            AstNode *st = parse_struct(p);
            if (st) {
                st->doc = doc;
                st->as.strct.is_proto = is_proto;
                st->as.strct.is_json = is_json;
                st->as.strct.is_must_check = is_must_check;
                decls = (AstNode **)realloc(decls, (nd + 1) * sizeof(AstNode *));
                decls[nd++] = st;
            } else {
                free(doc);
            }
            continue;
        }
        if (is_proto || is_test || is_server) {
            error(p, is_test ? "#[test] can only be applied to a fn"
                             : (is_server ? "#[server] can only be applied to a fn"
                                          : "#[proto] can only be applied to a struct"));
            free(doc);
            break;
        }
        if (is_json) {
            error(p, "#[json] can only be applied to a struct");
            free(doc);
            break;
        }
        if (is_must_check) {
            error(p, "#[must_check] can only be applied to a struct");
            free(doc);
            break;
        }
        if (match(p, TOK_LET) || match(p, TOK_CONST)) {
            AstNode *v = parse_let(p);
            if (v) {
                v->doc = doc;
                decls = (AstNode **)realloc(decls, (nd + 1) * sizeof(AstNode *));
                decls[nd++] = v;
            } else {
                free(doc);
            }
            continue;
        }
        free(doc);
        error(p, "expected fn, struct, enum, let, or const");
        break;
    }

    AstNode *prog = ast_program(imps, ni, decls, nd, loc);
    prog->as.program.mod_doc = mod_doc;
    return prog;
}
