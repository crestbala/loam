/**
 * lexer.c — scan Yuga source into tokens.
 *
 * Identifier vs keyword is decided after scanning a word. Integers are
 * decimal digits. Floats are `1.5`, `1e-9`, `1.0e3` (`1..10` stays a range).
 */
#include "lexer.h"
#include <string.h>
#include <ctype.h>

/** Advance `n` UTF-8 bytes, tracking 1-based line/column (byte columns). */
static void advance(Lexer *l, int n) {
    for (int i = 0; i < n; i++) {
        if (*l->pos == '\n') {
            l->line++;
            l->col = 1;
        } else {
            l->col++;
        }
        l->pos++;
    }
}

/** Reset the lexer to the start of `src`. */
void lexer_init(Lexer *l, const char *src, const char *file) {
    l->src = src;
    l->pos = src;
    l->file = file;
    l->line = 1;
    l->col = 1;
}

/** Token spanning [start, pos) whose loc starts at (sl, sc). */
static Token make_token(Lexer *l, TokenKind kind, const char *start, int sl, int sc) {
    Token t;
    t.kind = kind;
    t.start = start;
    t.len = (int)(l->pos - start);
    if (t.len < 0) t.len = 0;
    t.loc.file = l->file;
    t.loc.line = sl;
    t.loc.col = sc < 1 ? 1 : sc;
    t.loc.end_line = l->line;
    t.loc.end_col = l->col < 1 ? 1 : l->col;
    return t;
}

/** Names for error messages (`"identifier"`, `"+="`, `"fn"`, …). */
const char *token_kind_name(TokenKind k) {
    switch (k) {
        case TOK_EOF: return "EOF";
        case TOK_IDENT: return "identifier";
        case TOK_NUMBER: return "number";
        case TOK_FLOAT: return "float";
        case TOK_STRING: return "string";
        case TOK_COLON_COLON: return "::";
        case TOK_COLON: return ":";
        case TOK_SEMICOLON: return ";";
        case TOK_COMMA: return ",";
        case TOK_DOT: return ".";
        case TOK_DOT_DOT: return "..";
        case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")";
        case TOK_LBRACE: return "{";
        case TOK_RBRACE: return "}";
        case TOK_LBRACKET: return "[";
        case TOK_RBRACKET: return "]";
        case TOK_EQ: return "=";
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_PERCENT: return "%";
        case TOK_PLUS_EQ: return "+=";
        case TOK_MINUS_EQ: return "-=";
        case TOK_STAR_EQ: return "*=";
        case TOK_SLASH_EQ: return "/=";
        case TOK_PERCENT_EQ: return "%=";
        case TOK_PLUS_PLUS: return "++";
        case TOK_MINUS_MINUS: return "--";
        case TOK_AMP_EQ: return "&=";
        case TOK_PIPE_EQ: return "|=";
        case TOK_CARET_EQ: return "^=";
        case TOK_SHL_EQ: return "<<=";
        case TOK_SHR_EQ: return ">>=";
        case TOK_SHL: return "<<";
        case TOK_SHR: return ">>";
        case TOK_CARET: return "^";
        case TOK_TILDE: return "~";
        case TOK_EQ_EQ: return "==";
        case TOK_BANG_EQ: return "!=";
        case TOK_LT: return "<";
        case TOK_GT: return ">";
        case TOK_LT_EQ: return "<=";
        case TOK_GT_EQ: return ">=";
        case TOK_AMP: return "&";
        case TOK_AMP_AMP: return "&&";
        case TOK_PIPE: return "|";
        case TOK_PIPE_PIPE: return "||";
        case TOK_BANG: return "!";
        case TOK_HASH: return "#";
        case TOK_ARROW: return "->";
        case TOK_FAT_ARROW: return "=>";
        case TOK_UNKNOWN: return "unknown";
        case TOK_FN: return "fn";
        case TOK_LET: return "let";
        case TOK_CONST: return "const";
        case TOK_MUT: return "mut";
        case TOK_STRUCT: return "struct";
        case TOK_ENUM: return "enum";
        case TOK_IMPORT: return "import";
        case TOK_IF: return "if";
        case TOK_ELSE: return "else";
        case TOK_FOR: return "for";
        case TOK_WHILE: return "while";
        case TOK_IN: return "in";
        case TOK_RETURN: return "return";
        case TOK_BREAK: return "break";
        case TOK_CONTINUE: return "continue";
        case TOK_MATCH: return "match";
        case TOK_AS: return "as";
        case TOK_TRUE: return "true";
        case TOK_FALSE: return "false";
        case TOK_DOC: return "doc comment";
        case TOK_MOD_DOC: return "module doc comment";
        default: return "?";
    }
}

/** Byte-compare `s[0..len)` to keyword `kw`. */
static int kw_eq(const char *s, int len, const char *kw) {
    int n = (int)strlen(kw);
    return len == n && strncmp(s, kw, (size_t)n) == 0;
}

/** Skip whitespace and comments; return the next significant token. */
Token lexer_next(Lexer *l) {
    for (;;) {
        while (*l->pos && isspace((unsigned char)*l->pos)) advance(l, 1);
        if (*l->pos == '/' && l->pos[1] == '/') {
            /* `///` item docs, `//!` module docs. `////` stays a normal comment. */
            if (l->pos[2] == '/' && l->pos[3] != '/') {
                const char *start = l->pos;
                int sl = l->line, sc = l->col;
                while (*l->pos && *l->pos != '\n') advance(l, 1);
                return make_token(l, TOK_DOC, start, sl, sc);
            }
            if (l->pos[2] == '!') {
                const char *start = l->pos;
                int sl = l->line, sc = l->col;
                while (*l->pos && *l->pos != '\n') advance(l, 1);
                return make_token(l, TOK_MOD_DOC, start, sl, sc);
            }
            while (*l->pos && *l->pos != '\n') advance(l, 1);
            continue;
        }
        if (*l->pos == '/' && l->pos[1] == '*') {
            advance(l, 2);
            while (*l->pos && !(*l->pos == '*' && l->pos[1] == '/')) advance(l, 1);
            if (*l->pos) advance(l, 2);
            continue;
        }
        break;
    }

    if (!*l->pos) return make_token(l, TOK_EOF, l->pos, l->line, l->col);

    const char *start = l->pos;
    int sl = l->line, sc = l->col;
    char c = *l->pos;

    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)*l->pos) || *l->pos == '_') advance(l, 1);
        int len = (int)(l->pos - start);
        if (kw_eq(start, len, "fn")) return make_token(l, TOK_FN, start, sl, sc);
        if (kw_eq(start, len, "let")) return make_token(l, TOK_LET, start, sl, sc);
        if (kw_eq(start, len, "const")) return make_token(l, TOK_CONST, start, sl, sc);
        if (kw_eq(start, len, "mut")) return make_token(l, TOK_MUT, start, sl, sc);
        if (kw_eq(start, len, "struct")) return make_token(l, TOK_STRUCT, start, sl, sc);
        if (kw_eq(start, len, "enum")) return make_token(l, TOK_ENUM, start, sl, sc);
        if (kw_eq(start, len, "import")) return make_token(l, TOK_IMPORT, start, sl, sc);
        if (kw_eq(start, len, "if")) return make_token(l, TOK_IF, start, sl, sc);
        if (kw_eq(start, len, "else")) return make_token(l, TOK_ELSE, start, sl, sc);
        if (kw_eq(start, len, "for")) return make_token(l, TOK_FOR, start, sl, sc);
        if (kw_eq(start, len, "while")) return make_token(l, TOK_WHILE, start, sl, sc);
        if (kw_eq(start, len, "in")) return make_token(l, TOK_IN, start, sl, sc);
        if (kw_eq(start, len, "return")) return make_token(l, TOK_RETURN, start, sl, sc);
        if (kw_eq(start, len, "break")) return make_token(l, TOK_BREAK, start, sl, sc);
        if (kw_eq(start, len, "continue")) return make_token(l, TOK_CONTINUE, start, sl, sc);
        if (kw_eq(start, len, "match")) return make_token(l, TOK_MATCH, start, sl, sc);
        if (kw_eq(start, len, "as")) return make_token(l, TOK_AS, start, sl, sc);
        if (kw_eq(start, len, "true")) return make_token(l, TOK_TRUE, start, sl, sc);
        if (kw_eq(start, len, "false")) return make_token(l, TOK_FALSE, start, sl, sc);
        return make_token(l, TOK_IDENT, start, sl, sc);
    }

    if (isdigit((unsigned char)c)) {
        if (c == '0' && (l->pos[1] == 'x' || l->pos[1] == 'X') &&
            isxdigit((unsigned char)l->pos[2])) {
            advance(l, 2);
            while (isxdigit((unsigned char)*l->pos)) advance(l, 1);
            return make_token(l, TOK_NUMBER, start, sl, sc);
        }
        while (isdigit((unsigned char)*l->pos)) advance(l, 1);
        int is_float = 0;
        if (*l->pos == '.' && l->pos[1] != '.' && isdigit((unsigned char)l->pos[1])) {
            is_float = 1;
            advance(l, 1);
            while (isdigit((unsigned char)*l->pos)) advance(l, 1);
        }
        if (*l->pos == 'e' || *l->pos == 'E') {
            const char *rest = l->pos + 1;
            if (*rest == '+' || *rest == '-') rest++;
            if (isdigit((unsigned char)*rest)) {
                is_float = 1;
                advance(l, 1);
                if (*l->pos == '+' || *l->pos == '-') advance(l, 1);
                while (isdigit((unsigned char)*l->pos)) advance(l, 1);
            }
        }
        return make_token(l, is_float ? TOK_FLOAT : TOK_NUMBER, start, sl, sc);
    }

    if (c == '"') {
        advance(l, 1);
        while (*l->pos && *l->pos != '"') {
            if (*l->pos == '\\' && l->pos[1]) advance(l, 2);
            else advance(l, 1);
        }
        if (*l->pos == '"') advance(l, 1);
        return make_token(l, TOK_STRING, start, sl, sc);
    }

    if (c == ':' && l->pos[1] == ':') { advance(l, 2); return make_token(l, TOK_COLON_COLON, start, sl, sc); }
    if (c == '-' && l->pos[1] == '>') { advance(l, 2); return make_token(l, TOK_ARROW, start, sl, sc); }
    if (c == '=' && l->pos[1] == '>') { advance(l, 2); return make_token(l, TOK_FAT_ARROW, start, sl, sc); }
    if (c == '+' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_PLUS_EQ, start, sl, sc); }
    if (c == '-' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_MINUS_EQ, start, sl, sc); }
    if (c == '*' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_STAR_EQ, start, sl, sc); }
    if (c == '/' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_SLASH_EQ, start, sl, sc); }
    if (c == '%' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_PERCENT_EQ, start, sl, sc); }
    if (c == '+' && l->pos[1] == '+') { advance(l, 2); return make_token(l, TOK_PLUS_PLUS, start, sl, sc); }
    if (c == '-' && l->pos[1] == '-') { advance(l, 2); return make_token(l, TOK_MINUS_MINUS, start, sl, sc); }
    if (c == '&' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_AMP_EQ, start, sl, sc); }
    if (c == '|' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_PIPE_EQ, start, sl, sc); }
    if (c == '^' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_CARET_EQ, start, sl, sc); }
    if (c == '<' && l->pos[1] == '<' && l->pos[2] == '=') { advance(l, 3); return make_token(l, TOK_SHL_EQ, start, sl, sc); }
    if (c == '>' && l->pos[1] == '>' && l->pos[2] == '=') { advance(l, 3); return make_token(l, TOK_SHR_EQ, start, sl, sc); }
    if (c == '<' && l->pos[1] == '<') { advance(l, 2); return make_token(l, TOK_SHL, start, sl, sc); }
    if (c == '>' && l->pos[1] == '>') { advance(l, 2); return make_token(l, TOK_SHR, start, sl, sc); }
    if (c == '=' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_EQ_EQ, start, sl, sc); }
    if (c == '!' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_BANG_EQ, start, sl, sc); }
    if (c == '<' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_LT_EQ, start, sl, sc); }
    if (c == '>' && l->pos[1] == '=') { advance(l, 2); return make_token(l, TOK_GT_EQ, start, sl, sc); }
    if (c == '&' && l->pos[1] == '&') { advance(l, 2); return make_token(l, TOK_AMP_AMP, start, sl, sc); }
    if (c == '|' && l->pos[1] == '|') { advance(l, 2); return make_token(l, TOK_PIPE_PIPE, start, sl, sc); }
    if (c == '|') { advance(l, 1); return make_token(l, TOK_PIPE, start, sl, sc); }
    if (c == '.' && l->pos[1] == '.') { advance(l, 2); return make_token(l, TOK_DOT_DOT, start, sl, sc); }

    advance(l, 1);
    switch (c) {
        case ':': return make_token(l, TOK_COLON, start, sl, sc);
        case ';': return make_token(l, TOK_SEMICOLON, start, sl, sc);
        case ',': return make_token(l, TOK_COMMA, start, sl, sc);
        case '.': return make_token(l, TOK_DOT, start, sl, sc);
        case '(': return make_token(l, TOK_LPAREN, start, sl, sc);
        case ')': return make_token(l, TOK_RPAREN, start, sl, sc);
        case '{': return make_token(l, TOK_LBRACE, start, sl, sc);
        case '}': return make_token(l, TOK_RBRACE, start, sl, sc);
        case '[': return make_token(l, TOK_LBRACKET, start, sl, sc);
        case ']': return make_token(l, TOK_RBRACKET, start, sl, sc);
        case '=': return make_token(l, TOK_EQ, start, sl, sc);
        case '+': return make_token(l, TOK_PLUS, start, sl, sc);
        case '-': return make_token(l, TOK_MINUS, start, sl, sc);
        case '*': return make_token(l, TOK_STAR, start, sl, sc);
        case '/': return make_token(l, TOK_SLASH, start, sl, sc);
        case '%': return make_token(l, TOK_PERCENT, start, sl, sc);
        case '<': return make_token(l, TOK_LT, start, sl, sc);
        case '>': return make_token(l, TOK_GT, start, sl, sc);
        case '&': return make_token(l, TOK_AMP, start, sl, sc);
        case '!': return make_token(l, TOK_BANG, start, sl, sc);
        case '#': return make_token(l, TOK_HASH, start, sl, sc);
        case '^': return make_token(l, TOK_CARET, start, sl, sc);
        case '~': return make_token(l, TOK_TILDE, start, sl, sc);
        default:  return make_token(l, TOK_UNKNOWN, start, sl, sc);
    }
}
