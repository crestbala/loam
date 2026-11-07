/**
 * parser.h — recursive-descent parse of a Loam module into an AST.
 *
 * One token of lookahead. `allow_struct_lit` is cleared in condition
 * position so `if x {` is not parsed as a struct literal. An item doc
 * attaches to the next item.
 */
#ifndef LOAM_PARSER_H
#define LOAM_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer *lex;
    Token current;
    Token peek;
    Token previous;
    int had_error;
    int allow_struct_lit;
    AstNode **spill;     /* statements a `let` tuple split off, for push_stmt */
    size_t spill_n;
    int hidden_id;       /* numbers the hidden locals of `let` tuples */
} Parser;

/** Bind `p` to lexer `l` and prime current/peek. */
void parser_init(Parser *p, Lexer *l);

/**
 * Parse a full module: imports, then fn/struct items.
 * Returns a program node even on error; check `p->had_error`.
 */
AstNode *parser_parse(Parser *p);

#endif
