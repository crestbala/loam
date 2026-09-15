/**
 * lexer.h — tokenize Yuga source.
 *
 * Tokens are spans into the caller's buffer (`start` + `len`); the lexer
 * does not copy lexemes. Keywords are TokenKind values, not identifiers.
 * Line comments (`//`) and block comments are skipped. `///` and `//!` are
 * tokens (item / module docs), not skipped. Semicolons are tokens; the parser
 * treats them as optional.
 */
#ifndef YUGA_LEXER_H
#define YUGA_LEXER_H

#include "diagnostics.h"

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_FLOAT,
    TOK_STRING,
    TOK_COLON_COLON,
    TOK_COLON,
    TOK_SEMICOLON,
    TOK_COMMA,
    TOK_DOT,
    TOK_DOT_DOT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_EQ,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_PLUS_EQ,
    TOK_MINUS_EQ,
    TOK_STAR_EQ,
    TOK_SLASH_EQ,
    TOK_PERCENT_EQ,
    TOK_PLUS_PLUS,
    TOK_MINUS_MINUS,
    TOK_AMP_EQ,
    TOK_PIPE_EQ,
    TOK_CARET_EQ,
    TOK_SHL_EQ,
    TOK_SHR_EQ,
    TOK_SHL,
    TOK_SHR,
    TOK_CARET,
    TOK_TILDE,
    TOK_EQ_EQ,
    TOK_BANG_EQ,
    TOK_LT,
    TOK_GT,
    TOK_LT_EQ,
    TOK_GT_EQ,
    TOK_AMP,
    TOK_AMP_AMP,
    TOK_PIPE,
    TOK_PIPE_PIPE,
    TOK_BANG,
    TOK_HASH,
    TOK_ARROW,
    TOK_FAT_ARROW,
    TOK_UNKNOWN,
    TOK_FN,
    TOK_LET,
    TOK_CONST,
    TOK_MUT,
    TOK_STRUCT,
    TOK_ENUM,
    TOK_IMPORT,
    TOK_IF,
    TOK_ELSE,
    TOK_FOR,
    TOK_WHILE,
    TOK_IN,
    TOK_RETURN,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_MATCH,
    TOK_AS,
    TOK_TRUE,
    TOK_FALSE,
    TOK_DOC,      /* `///` item documentation */
    TOK_MOD_DOC,  /* `//!` module documentation */
} TokenKind;

/** One token: kind, source span, and start/end location (1-based). */
typedef struct {
    TokenKind kind;
    const char *start;
    int len;
    SourceLoc loc;
} Token;

/** Streaming lexer state. `src` must outlive the Lexer. */
typedef struct {
    const char *src;
    const char *pos;
    const char *file;
    int line;
    int col;
} Lexer;

/** Point `l` at `src` from `file` (used only in diagnostics). */
void lexer_init(Lexer *l, const char *src, const char *file);

/** Next token, advancing the cursor. Returns TOK_EOF at end. */
Token lexer_next(Lexer *l);

/** Human-readable name for diagnostics (`"identifier"`, `"=="`, …). */
const char *token_kind_name(TokenKind k);

#endif
