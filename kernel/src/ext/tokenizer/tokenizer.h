#ifndef EXT_TOKENIZER_H
#define EXT_TOKENIZER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

enum token_type {
    TOKEN_EOF = 0,
    TOKEN_KEYWORD,      /* use, let, while, if, else */
    TOKEN_IDENTIFIER,   /* console, env, variable names */
    TOKEN_STRING,       /* "literal text" */
    TOKEN_INT,          /* decimal integers */
    TOKEN_HEX,          /* hex numbers (0x...) for mask.h compatibility */
    TOKEN_SYMBOL,       /* =, ., ;, {, }, (, ), +, -, *, /, <, >, etc. */
    TOKEN_UNKNOWN
};

struct token {
    enum token_type type;
    const char *start;  /* Pointer into the original VFS script buffer (zero-copy) */
    size_t length;      /* Length of the token lexeme */
    int line;           /* Line tracking for diagnostics and errors */
};

struct tokenizer {
    const char *cursor;
    const char *end;
    int line;
};

/* Initialize the tokenizer with the loaded VFS script buffer */
void tokenizer_init(struct tokenizer *t, const char *src, size_t size);

/* Fetch the next token from the stream */
struct token tokenizer_next(struct tokenizer *t);

/* Helper to check if a token matches a specific keyword or string */
bool token_equals(struct token *tok, const char *str);

#endif /* EXT_TOKENIZER_H */
