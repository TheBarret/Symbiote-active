#ifndef EXT_TOKENIZER_H
#define EXT_TOKENIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lexer for the Symbiote script language.
 *
 * Lifetime contract:
 * `struct token.start` points into the buffer passed to tokenizer_init().
 * The caller owns that buffer and MUST keep it alive and unmodified for as long as any token from it is in use.
 * The lexer performs no allocation and takes no copy.
 *
 * Error handling: lexical errors are reported as TOKEN_ERROR, not as TOKEN_UNKNOWN.
 * TOKEN_UNKNOWN means "one unrecognised byte was skipped";
 * TOKEN_ERROR means "the lexer could not make progress and the stream is no longer trustworthy".
 * On TOKEN_ERROR, .length and .start describe the offending region (best effort),
 * and the tokenizer advances past it so a caller may continue if it wants better diagnostics. Check .type first. */

enum token_type {
    TOKEN_EOF = 0,
    TOKEN_KEYWORD,      /* use, let, while, if, else */
    TOKEN_IDENTIFIER,   /* console, env, variable names */
    TOKEN_STRING,       /* "literal text" (quotes excluded from .start/.length) */
    TOKEN_INT,          /* decimal integers */
    TOKEN_HEX,          /* 0x-prefixed hex; .start includes "0x" */
    TOKEN_SYMBOL,       /* single char: = . ; { } ( ) + - * / , < > */
    TOKEN_UNKNOWN,      /* one unrecognised byte, skipped */
    TOKEN_ERROR,        /* lexer could not produce a valid token here */
};

enum token_error {
    TOKEN_ERR_NONE = 0,
    TOKEN_ERR_UNTERMINATED_STRING,
    TOKEN_ERR_UNTERMINATED_COMMENT,
    TOKEN_ERR_EMPTY_HEX,
};

struct token {
    enum token_type type;
    const char *start;
    size_t length;
    int line;           /* first line of the lexeme */
    int end_line;       /* last line of the lexeme (>= line) */
    enum token_error error;  /* TOKEN_ERR_NONE unless type == TOKEN_ERROR */
};

struct tokenizer {
    const char *cursor;
    const char *end;
    int line;
};

void tokenizer_init(struct tokenizer *t, const char *src, size_t size);
struct token tokenizer_next(struct tokenizer *t);

/* Exact lexeme comparison. `tok` may be NULL. */
bool token_equals(const struct token *tok, const char *str);

/* Human-readable error name for diagnostics. */
const char *token_error_str(enum token_error e);

#endif /* EXT_TOKENIZER_H */
