/**
 * scanner.c — the statement terminator the grammar cannot express.
 *
 * Loam ends a statement at a newline, but `extras` makes a newline plain
 * whitespace, so `let a = 1` then `*a = 1` would read as `let a = (1 * a)`.
 * Emit a zero-width `_line_break` when a statement could end here and the next
 * line opens with a token that would otherwise continue the expression:
 *
 *   ( [ ++ -- and every binary operator   start a new statement
 *   . :: as ?                             continue it, so no break here
 *
 * This is the list `parse_postfix` and `parse_binary` stop at in parser.c.
 * It is a terminator rather than a "same line" marker before each operator
 * because the operators must stay the lookahead, or precedence has nothing to
 * compare. Any other token ends the statement without help.
 */
#include "tree_sitter/parser.h"

enum { LINE_BREAK, ERROR_SENTINEL };

void *tree_sitter_loam_external_scanner_create(void) { return NULL; }
void tree_sitter_loam_external_scanner_destroy(void *p) { (void)p; }
unsigned tree_sitter_loam_external_scanner_serialize(void *p, char *b) {
    (void)p;
    (void)b;
    return 0;
}
void tree_sitter_loam_external_scanner_deserialize(void *p, const char *b, unsigned n) {
    (void)p;
    (void)b;
    (void)n;
}

static void skip(TSLexer *l) { l->advance(l, true); }

/** The `/` is consumed. Skip the rest of a comment; 0 if it was a division. */
static bool skip_comment(TSLexer *l, bool *newline) {
    if (l->lookahead == '/') {
        while (l->lookahead && l->lookahead != '\n') skip(l);
        return true;
    }
    if (l->lookahead != '*') return false;
    skip(l);
    while (l->lookahead) {
        if (l->lookahead == '\n') *newline = true;
        if (l->lookahead == '*') {
            skip(l);
            if (l->lookahead == '/') {
                skip(l);
                return true;
            }
        } else {
            skip(l);
        }
    }
    return true;
}

/** Does the token at the lookahead, after one char `c` was consumed, start a
 *  new statement? `+=` and friends do not: they continue into an assignment. */
static bool opens_statement(TSLexer *l, int32_t c) {
    switch (c) {
        case '(': case '[':
            return true;
        case '+': case '*': case '%': case '^': case '/':
            return l->lookahead != '=';
        case '-':
            return l->lookahead != '=' && l->lookahead != '>';
        case '&': case '|':
            return l->lookahead != '=';
        case '<': case '>':
            if (l->lookahead != c) return true;
            skip(l);
            return l->lookahead != '=';
        case '=': case '!':
            return l->lookahead == '=';
        case '.':
            return l->lookahead == '.';
        default:
            return false;
    }
}

bool tree_sitter_loam_external_scanner_scan(void *p, TSLexer *l, const bool *valid) {
    (void)p;
    /* During error recovery every symbol is valid; stay out of it. */
    if (valid[ERROR_SENTINEL] || !valid[LINE_BREAK]) return false;
    l->result_symbol = LINE_BREAK;
    l->mark_end(l); /* zero width: everything below is lookahead */
    bool newline = false;
    for (;;) {
        while (l->lookahead == ' ' || l->lookahead == '\t' || l->lookahead == '\r' ||
               l->lookahead == '\n') {
            if (l->lookahead == '\n') newline = true;
            skip(l);
        }
        if (l->lookahead != '/') break;
        skip(l);
        if (!skip_comment(l, &newline)) return newline && l->lookahead != '=';
    }
    if (!newline || !l->lookahead) return false;
    int32_t c = l->lookahead;
    skip(l);
    return opens_statement(l, c);
}
