#include <lib/string.h>
#include <lib/mem.h>
#include <ext/tokenizer/tokenizer.h>

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_alnum(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_keyword(const char *start, size_t len) {
    if (len == 3 && memcmp(start, "use", 3) == 0) return true;
    if (len == 3 && memcmp(start, "let", 3) == 0) return true;
    if (len == 5 && memcmp(start, "while", 5) == 0) return true;
    if (len == 2 && memcmp(start, "if", 2) == 0) return true;
    if (len == 4 && memcmp(start, "else", 4) == 0) return true;
    return false;
}

void tokenizer_init(struct tokenizer *t, const char *src, size_t size) {
    t->cursor = src;
    t->end = src + size;
    t->line = 1;
}

struct token tokenizer_next(struct tokenizer *t) {
    while (t->cursor < t->end) {
        char c = *t->cursor;

        /* 1. Skip whitespace */
        if (c == ' ' || c == '\r' || c == '\t') {
            t->cursor++;
            continue;
        }
        if (c == '\n') {
            t->line++;
            t->cursor++;
            continue;
        }

        /* 2. Skip Python/BASIC style comments starting with '#' */
        if (c == '#') {
            while (t->cursor < t->end && *t->cursor != '\n') {
                t->cursor++;
            }
            continue;
        }

        /* 3. Hexadecimal detection (0x or 0X followed by hex digits) */
        if (c == '0' && (t->cursor + 1 < t->end) && (t->cursor[1] == 'x' || t->cursor[1] == 'X')) {
            const char *start = t->cursor;
            t->cursor += 2; /* consume '0x' */
            while (t->cursor < t->end &&
                   ((*t->cursor >= '0' && *t->cursor <= '9') ||
                    (*t->cursor >= 'a' && *t->cursor <= 'f') ||
                    (*t->cursor >= 'A' && *t->cursor <= 'F'))) {
                t->cursor++;
            }
            return (struct token){
                .type = TOKEN_HEX,
                .start = start,
                .length = (size_t)(t->cursor - start),
                .line = t->line
            };
        }

        /* 4. Decimal Integers */
        if (is_digit(c)) {
            const char *start = t->cursor;
            while (t->cursor < t->end && is_digit(*t->cursor)) {
                t->cursor++;
            }
            return (struct token){
                .type = TOKEN_INT,
                .start = start,
                .length = (size_t)(t->cursor - start),
                .line = t->line
            };
        }

        /* 5. Strings ("...") */
        if (c == '"') {
            const char *start = t->cursor;
            t->cursor++; /* consume opening quote */
            while (t->cursor < t->end && *t->cursor != '"') {
                if (*t->cursor == '\n') t->line++;
                t->cursor++;
            }
            if (t->cursor < t->end) t->cursor++; /* consume closing quote */
            return (struct token){
                .type = TOKEN_STRING,
                .start = start + 1,             /* exclude quotes from token content */
                .length = (size_t)(t->cursor - start - 2),
                .line = t->line
            };
        }

        /* 6. Symbols and Operators (=, ., ;, {, }, (, ), +, -, *, /, <, >, etc.) */
        if (c == '=' || c == '.' || c == ';' || c == '{' || c == '}' ||
            c == '(' || c == ')' || c == '+' || c == '-' || c == '*' ||
            c == '/' || c == ',' || c == '<' || c == '>') {
            struct token tok = {
                .type = TOKEN_SYMBOL,
                .start = t->cursor,
                .length = 1,
                .line = t->line
            };
            t->cursor++;
            return tok;
        }

        /* 7. Identifiers & Keywords */
        if (is_alpha(c)) {
            const char *start = t->cursor;
            while (t->cursor < t->end && is_alnum(*t->cursor)) {
                t->cursor++;
            }
            size_t len = (size_t)(t->cursor - start);

            enum token_type type = is_keyword(start, len) ? TOKEN_KEYWORD : TOKEN_IDENTIFIER;

            return (struct token){
                .type = type,
                .start = start,
                .length = len,
                .line = t->line
            };
        }

        /* Fallback for unrecognized characters */
        struct token unknown = {
            .type = TOKEN_UNKNOWN,
            .start = t->cursor,
            .length = 1,
            .line = t->line
        };
        t->cursor++;
        return unknown;
    }

    return (struct token){
        .type = TOKEN_EOF,
        .start = t->cursor,
        .length = 0,
        .line = t->line
    };
}

bool token_equals(struct token *tok, const char *str) {
    size_t len = strlen(str);
    if (tok->length != len) return false;
    return memcmp(tok->start, str, len) == 0;
}
