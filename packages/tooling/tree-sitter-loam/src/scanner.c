/**
 * scanner.c — the statement terminator the grammar cannot express, and the
 * contextual word `unsafe`.
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
 *
 * `unsafe` is a keyword only as `unsafe fn` or `unsafe {`. The scanner emits
 * it in those two places. `let unsafe = 1` and `unsafe.tick()` stay names,
 * because a keyword token would hide the identifier.
 */
#include "tree_sitter/parser.h"

enum { LINE_BREAK, ERROR_SENTINEL, UNSAFE_FN, UNSAFE_BLOCK };

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

/* `true` moves the token start. Use it for whitespace before a token, and
 * for lookahead after a zero-width `mark_end`. After a keyword's `mark_end`,
 * lookahead must use `advance(..., false)` or the token collapses. */
static void skip(TSLexer *l) { l->advance(l, true); }
static void peek(TSLexer *l) { l->advance(l, false); }

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

static bool ident_cont(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/** Consume `w` as a whole word. The caller returns false on failure, which
 *  restores the start position. */
static bool take_word(TSLexer *l, const char *w) {
    int i;
    for (i = 0; w[i]; i++) {
        if (l->lookahead != (int32_t)w[i]) return false;
        peek(l);
    }
    return !ident_cont(l->lookahead);
}

/** Past space and comments, `{` or `fn`. 1 is `{`, 2 is `fn`, 0 is neither.
 *  Call after `mark_end`: this uses `peek`, so the keyword keeps its span. */
static int unsafe_follower(TSLexer *l, bool brace, bool fn) {
    bool newline = false;
    for (;;) {
        while (l->lookahead == ' ' || l->lookahead == '\t' || l->lookahead == '\r' ||
               l->lookahead == '\n') {
            peek(l);
        }
        if (l->lookahead != '/') break;
        peek(l);
        if (!skip_comment(l, &newline)) return 0;
    }
    if (brace && l->lookahead == '{') return 1;
    if (!fn || l->lookahead != 'f') return 0;
    peek(l);
    if (l->lookahead != 'n') return 0;
    peek(l);
    return ident_cont(l->lookahead) ? 0 : 2;
}

/** `true` when `unsafe` was emitted. Leading space is not part of the word. */
static bool scan_unsafe(TSLexer *l, bool brace, bool fn) {
    int which;
    bool newline = false;
    for (;;) {
        while (l->lookahead == ' ' || l->lookahead == '\t' || l->lookahead == '\r' ||
               l->lookahead == '\n') {
            skip(l);
        }
        if (l->lookahead != '/') break;
        skip(l);
        if (!skip_comment(l, &newline)) return false;
    }
    if (l->lookahead != 'u' || !take_word(l, "unsafe")) return false;
    l->mark_end(l);
    which = unsafe_follower(l, brace, fn);
    if (which == 0) return false;
    l->result_symbol = which == 1 ? UNSAFE_BLOCK : UNSAFE_FN;
    return true;
}

bool tree_sitter_loam_external_scanner_scan(void *p, TSLexer *l, const bool *valid) {
    (void)p;
    /* During error recovery every symbol is valid; stay out of it. */
    if (valid[ERROR_SENTINEL]) return false;
    if ((valid[UNSAFE_FN] || valid[UNSAFE_BLOCK]) && !valid[LINE_BREAK]) {
        return scan_unsafe(l, valid[UNSAFE_BLOCK], valid[UNSAFE_FN]);
    }
    if (!valid[LINE_BREAK]) return false;
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
    /* `unsafe {` / `unsafe fn` after a newline is the keyword, not a name
     * the line break has to separate from an operator. */
    if ((valid[UNSAFE_FN] || valid[UNSAFE_BLOCK]) && l->lookahead == 'u') {
        return scan_unsafe(l, valid[UNSAFE_BLOCK], valid[UNSAFE_FN]);
    }
    if (!newline || !l->lookahead) return false;
    int32_t c = l->lookahead;
    skip(l);
    return opens_statement(l, c);
}
