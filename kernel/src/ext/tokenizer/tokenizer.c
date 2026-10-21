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

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool is_keyword(const char *start, size_t len) {
    if (len == 2 && memcmp(start, "if",   2) == 0) return true;
    if (len == 3 && memcmp(start, "use",  3) == 0) return true;
    if (len == 3 && memcmp(start, "let",  3) == 0) return true;
    if (len == 4 && memcmp(start, "else", 4) == 0) return true;
    if (len == 5 && memcmp(start, "while",5) == 0) return true;
    return false;
}

void tokenizer_init(struct tokenizer *t, const char *src, size_t size) {
    if (t == NULL) return;
    t->cursor = src;
    t->end    = src ? src + size : src;
    t->line   = 1;
}

static struct token make(enum token_type type, enum token_error err,
                         const char *start, size_t len,
                         int line, int end_line) {
    return (struct token){
        .type     = type,
        .start    = start,
        .length   = len,
        .line     = line,
        .end_line = end_line,
        .error    = err,
    };
}

struct token tokenizer_next(struct tokenizer *t) {
    /* Whitespace + comment skip, restarting line count as it goes. */
    for (;;) {
        if (t->cursor >= t->end)
            return make(TOKEN_EOF, TOKEN_ERR_NONE, t->cursor, 0, t->line, t->line);

        char c = *t->cursor;
        if (c == ' ' || c == '\r' || c == '\t') { t->cursor++; continue; }
        if (c == '\n') { t->line++; t->cursor++; continue; }
        if (c == '#') {
            const char *cstart = t->cursor;
            int start_line = t->line;
            while (t->cursor < t->end && *t->cursor != '\n') t->cursor++;
            if (t->cursor >= t->end) {
                /* Comment runs to EOF: legal, but report it so the caller
                 * knows there is nothing after it. */
                return make(TOKEN_ERROR, TOKEN_ERR_UNTERMINATED_COMMENT,
                            cstart, (size_t)(t->cursor - cstart),
                            start_line, t->line);
            }
            /* Skip the newline too, keeping line in sync. */
            t->cursor++;
            t->line++;
            continue;
        }
        break;
    }

    const char *start = t->cursor;
    int start_line = t->line;

    /* Hex: 0x/0X followed by one or more hex digits. */
    if (start[0] == '0' && (t->cursor + 1 < t->end) &&
        (start[1] == 'x' || start[1] == 'X')) {
        t->cursor += 2;
        const char *digits = t->cursor;
        while (t->cursor < t->end && is_hex_digit(*t->cursor)) t->cursor++;
        if (t->cursor == digits) {
            /* "0x" with no digits: report and consume the "0x". */
            return make(TOKEN_ERROR, TOKEN_ERR_EMPTY_HEX,
                        start, (size_t)(t->cursor - start),
                        start_line, t->line);
        }
        return make(TOKEN_HEX, TOKEN_ERR_NONE,
                    start, (size_t)(t->cursor - start),
                    start_line, t->line);
    }

    /* Decimal integer. */
    if (is_digit(*start)) {
        while (t->cursor < t->end && is_digit(*t->cursor)) t->cursor++;
        return make(TOKEN_INT, TOKEN_ERR_NONE,
                    start, (size_t)(t->cursor - start),
                    start_line, t->line);
    }

    /* String: "..."  No escapes yet; may span lines. */
    if (*start == '"') {
        t->cursor++;                 /* opening quote */
        const char *body = t->cursor;
        while (t->cursor < t->end && *t->cursor != '"') {
            if (*t->cursor == '\n') t->line++;
            t->cursor++;
        }
        if (t->cursor >= t->end) {
            /* Unterminated: report the run from the opening quote to EOF.
             * .length is well-defined; do not underflow. */
            return make(TOKEN_ERROR, TOKEN_ERR_UNTERMINATED_STRING,
                        start, (size_t)(t->cursor - start),
                        start_line, t->line);
        }
        int end_line = t->line;
        t->cursor++;                 /* closing quote */
        return make(TOKEN_STRING, TOKEN_ERR_NONE,
                    body, (size_t)(t->cursor - body - 1),
                    start_line, end_line);
    }

    /* Single-char symbols. Maximal munch is the parser's job. */
    switch (*start) {
        case '=': case '.': case ';': case '{': case '}':
        case '(': case ')': case '+': case '-': case '*':
        case '/': case ',': case '<': case '>':
            t->cursor++;
            return make(TOKEN_SYMBOL, TOKEN_ERR_NONE,
                        start, 1, start_line, start_line);
        default: break;
    }

    /* Identifier / keyword. */
    if (is_alpha(*start)) {
        while (t->cursor < t->end && is_alnum(*t->cursor)) t->cursor++;
        size_t len = (size_t)(t->cursor - start);
        enum token_type ty = is_keyword(start, len) ? TOKEN_KEYWORD : TOKEN_IDENTIFIER;
        return make(ty, TOKEN_ERR_NONE, start, len, start_line, start_line);
    }

    /* Unrecognised byte: skip one, tell the caller. */
    t->cursor++;
    return make(TOKEN_UNKNOWN, TOKEN_ERR_NONE,
                start, 1, start_line, start_line);
}

bool token_equals(const struct token *tok, const char *str) {
    if (tok == NULL || str == NULL) return false;
    size_t len = strlen(str);
    if (tok->length != len) return false;
    return memcmp(tok->start, str, len) == 0;
}

const char *token_error_str(enum token_error e) {
    switch (e) {
        case TOKEN_ERR_NONE:                  return "none";
        case TOKEN_ERR_UNTERMINATED_STRING:   return "unterminated string";
        case TOKEN_ERR_UNTERMINATED_COMMENT:  return "unterminated comment";
        case TOKEN_ERR_EMPTY_HEX:             return "empty hex literal";
        default:                              return "unknown error";
    }
}
