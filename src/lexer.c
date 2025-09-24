#include <ctype.h>
#include <errno.h>

#include "lexer.h"
#include "message.h"

static SV first_line(SV sv) {
    const char *p = memchr(sv.data, '\n', sv.count);
    sv.count = p ? (size_t) (p - sv.data) : sv.count;
    return sv;
}

bool lexer_open(Lexer *l, const char *path, Arena *arena) {
    memset(l, 0, sizeof(*l));
    if (!read_file(&l->sv, path, arena)) {
        return false;
    }

    l->pos.path = path;
    l->pos.line = first_line(l->sv);
    return true;
}

void lexer_buffer(Lexer *l, Token token) {
    l->peeked = true;
    l->buffer = token;
}

void lexer_unbuffer(Lexer *l) {
    l->peeked = false;
}

static bool isident(char ch) {
    return isalnum(ch) || ch == '_';
}

static void next_char(Lexer *l) {
    if (*l->sv.data == '\n') {
        if (l->sv.count > 1) {
            l->pos.row++;
            l->pos.col = 0;

            l->sv.data++;
            l->sv.count--;

            l->pos.line = first_line(l->sv);
            return;
        }
    } else {
        l->pos.col++;
    }

    l->sv.data++;
    l->sv.count--;
}

static char peek_char(Lexer *l, size_t n) {
    if (l->sv.count > n) {
        return l->sv.data[n];
    }

    return 0;
}

static char read_char(Lexer *l) {
    next_char(l);
    return l->sv.data[-1];
}

static bool match_char(Lexer *l, char ch) {
    if (l->sv.count && *l->sv.data == ch) {
        next_char(l);
        return true;
    }
    return false;
}

static void skip_whitespace(Lexer *l) {
    size_t newlines_before_comment = 0;

    l->newlines = 0;
    while (l->sv.count) {
        switch (*l->sv.data) {
        case ' ':
        case '\t':
        case '\r':
            next_char(l);
            break;

        case '\n':
            next_char(l);
            l->newlines++;
            newlines_before_comment++;
            break;

        case '#':
            if (l->pos.col == 0 && l->pos.row == 0 && peek_char(l, 1) == '!') {
                Comment comment = {.shebang = true, .pos = l->pos, .sv = l->sv};
                while (l->sv.count && *l->sv.data != '\n') {
                    next_char(l);
                }
                comment.sv.count -= l->sv.count;

                if (l->comments) {
                    da_push(l->comments, comment);
                }
            } else {
                return;
            }
            break;

        case '/': {
            Comment comment = {.pos = l->pos, .sv = l->sv};
            if (peek_char(l, 1) == '/') {
                while (l->sv.count && *l->sv.data != '\n') {
                    next_char(l);
                }
            } else {
                return;
            }
            comment.sv.count -= l->sv.count;

            comment.sv.data += 2;
            comment.sv.count -= 2;

            if (newlines_before_comment > 1) {
                comment.ws = CWS_BLANKLINE;
            } else if (newlines_before_comment == 1) {
                comment.ws = CWS_NEWLINE;
            } else {
                comment.ws = CWS_INLINE;
            }
            newlines_before_comment = 0;

            if (l->comments) {
                da_push(l->comments, comment);
            }
        } break;

        default:
            return;
        }
    }
}

static void error_invalid(Pos pos, SV sv, const char *label) {
    if (isprint(*sv.data)) {
        error_full(ERROR, pos, "Invalid %s '%c'", label, *sv.data);
    } else {
        error_full(ERROR, pos, "Invalid %s (%d)", label, *sv.data);
    }

    exit(1);
}

static void error_unterminated(Pos pos, const char *label) {
    error_full(ERROR, pos, "Unterminated %s", label);
    exit(1);
}

static char parse_char(Lexer *l, const char *label) {
    if (!l->sv.count) {
        error_unterminated(l->pos, label);
    }

    char ch = read_char(l);
    if (ch != '\\') {
        return ch;
    }

    if (!l->sv.count) {
        error_unterminated(l->pos, label);
    }

    ch = *l->sv.data;
    if (!resolve_escape_char(&ch)) {
        error_invalid(l->pos, l->sv, "escape character");
    }

    next_char(l);
    return ch;
}

static size_t parse_str(Lexer *l, const char *label) {
    size_t n = 0;
    while (l->sv.count) {
        if (*l->sv.data == '"') {
            break;
        }
        parse_char(l, label);
        n++;
    }

    if (!l->sv.count) {
        error_unterminated(l->pos, label);
    }

    next_char(l);
    return n;
}

static_assert(COUNT_TOKENS == 67, "");
Token lexer_next(Lexer *l) {
    if (l->peeked) {
        lexer_unbuffer(l);
        return l->buffer;
    }
    skip_whitespace(l);

    Token token = {
        .pos = l->pos,
        .sv = l->sv,
        .newlines = l->newlines,
    };

    if (!l->sv.count) {
        return token;
    }

    if (isdigit(*l->sv.data)) {
        token.kind = TOKEN_INT;
        while (l->sv.count > 0 && isdigit(*l->sv.data)) {
            next_char(l);
        }
        token.sv.count -= l->sv.count;

        if (l->sv.count && isident(*l->sv.data)) {
            error_invalid(l->pos, l->sv, "digit");
        }

        char buffer[32] = {0};
        if (token.sv.count < sizeof(buffer) - 1) {
            memcpy(buffer, token.sv.data, token.sv.count);

            errno = 0;
            token.as.integer = strtol(buffer, NULL, 10);

            if (!errno) {
                return token;
            }
        }

        error_full(ERROR, token.pos, "Integer literal '" SVFmt "' is too large\n", SVArg(token.sv));
        exit(1);
    }

    if (isident(*l->sv.data)) {
        while (l->sv.count > 0 && isident(*l->sv.data)) {
            next_char(l);
        }
        token.sv.count -= l->sv.count;

        if (sv_match(token.sv, "true")) {
            token.kind = TOKEN_BOOL;
            token.as.boolean = 1;
        } else if (sv_match(token.sv, "false")) {
            token.kind = TOKEN_BOOL;
            token.as.boolean = 0;
        } else if (sv_match(token.sv, "len")) {
            token.kind = TOKEN_LEN;
        } else if (sv_match(token.sv, "sizeof")) {
            token.kind = TOKEN_SIZEOF;
        } else if (sv_match(token.sv, "assert")) {
            token.kind = TOKEN_ASSERT;
        } else if (sv_match(token.sv, "if")) {
            token.kind = TOKEN_IF;
        } else if (sv_match(token.sv, "then")) {
            token.kind = TOKEN_THEN;
        } else if (sv_match(token.sv, "else")) {
            token.kind = TOKEN_ELSE;
        } else if (sv_match(token.sv, "for")) {
            token.kind = TOKEN_FOR;
        } else if (sv_match(token.sv, "return")) {
            token.kind = TOKEN_RETURN;
        } else if (sv_match(token.sv, "fn")) {
            token.kind = TOKEN_FN;
        } else if (sv_match(token.sv, "struct")) {
            token.kind = TOKEN_STRUCT;
        } else if (sv_match(token.sv, "var")) {
            token.kind = TOKEN_VAR;
        } else if (sv_match(token.sv, "type")) {
            token.kind = TOKEN_TYPE;
        } else if (sv_match(token.sv, "const")) {
            token.kind = TOKEN_CONST;
        } else if (sv_match(token.sv, "extern")) {
            token.kind = TOKEN_EXTERN;
        } else if (sv_match(token.sv, "static")) {
            token.kind = TOKEN_STATIC;
        } else if (sv_match(token.sv, "pub")) {
            token.kind = TOKEN_PUB;
        } else if (sv_match(token.sv, "import")) {
            token.kind = TOKEN_IMPORT;
        } else if (sv_match(token.sv, "package")) {
            token.kind = TOKEN_PACKAGE;
        } else if (sv_match(token.sv, "print")) {
            token.kind = TOKEN_PRINT;
        } else {
            token.kind = TOKEN_IDENT;
        }

        return token;
    }

    switch (read_char(l)) {
    case ';':
        token.kind = TOKEN_EOL;
        break;

    case '.':
        if (match_char(l, '.')) {
            token.kind = TOKEN_RANGE;
        } else {
            token.kind = TOKEN_DOT;
        }
        break;

    case ':':
        if (match_char(l, ':')) {
            token.kind = TOKEN_SCOPE;
        } else {
            token.kind = TOKEN_COLON;
        }
        break;

    case ',':
        token.kind = TOKEN_COMMA;
        break;

    case '\'':
        token.kind = TOKEN_CHAR;
        token.as.integer = parse_char(l, "character");
        if (!match_char(l, '\'')) {
            error_unterminated(l->pos, "character");
        }
        break;

    case '"':
        token.kind = TOKEN_STR;
        token.as.integer = parse_str(l, "string");
        break;

    case '#':
        while (l->sv.count > 0 && isident(*l->sv.data)) {
            next_char(l);
        }
        token.sv.count -= l->sv.count;

        if (sv_match(token.sv, "#link")) {
            token.kind = TOKEN_LINK;
        } else {
            error_full(ERROR, token.pos, "Invalid property '" SVFmt "'", SVArg(token.sv));
            exit(1);
        }
        break;

    case '(':
        token.kind = TOKEN_LPAREN;
        break;

    case ')':
        token.kind = TOKEN_RPAREN;
        break;

    case '{':
        token.kind = TOKEN_LBRACE;
        break;

    case '}':
        token.kind = TOKEN_RBRACE;
        break;

    case '[':
        token.kind = TOKEN_LBRACKET;
        break;

    case ']':
        token.kind = TOKEN_RBRACKET;
        break;

    case '+':
        if (match_char(l, '=')) {
            token.kind = TOKEN_ADD_SET;
        } else {
            token.kind = TOKEN_ADD;
        }
        break;

    case '-':
        if (match_char(l, '=')) {
            token.kind = TOKEN_SUB_SET;
        } else {
            token.kind = TOKEN_SUB;
        }
        break;

    case '*':
        if (match_char(l, '=')) {
            token.kind = TOKEN_MUL_SET;
        } else {
            token.kind = TOKEN_MUL;
        }
        break;

    case '/':
        if (match_char(l, '=')) {
            token.kind = TOKEN_DIV_SET;
        } else {
            token.kind = TOKEN_DIV;
        }
        break;

    case '%':
        if (match_char(l, '=')) {
            token.kind = TOKEN_MOD_SET;
        } else {
            token.kind = TOKEN_MOD;
        }
        break;

    case '|':
        if (match_char(l, '=')) {
            token.kind = TOKEN_BOR_SET;
        } else if (match_char(l, '|')) {
            token.kind = TOKEN_LOR;
        } else {
            token.kind = TOKEN_BOR;
        }
        break;

    case '&':
        if (match_char(l, '=')) {
            token.kind = TOKEN_BAND_SET;
        } else if (match_char(l, '&')) {
            token.kind = TOKEN_LAND;
        } else {
            token.kind = TOKEN_BAND;
        }
        break;

    case '~':
        token.kind = TOKEN_BNOT;
        break;

    case '!':
        if (match_char(l, '=')) {
            token.kind = TOKEN_NE;
        } else {
            token.kind = TOKEN_LNOT;
        }
        break;

    case '>':
        if (match_char(l, '>')) {
            if (match_char(l, '=')) {
                token.kind = TOKEN_SHR_SET;
            } else {
                token.kind = TOKEN_SHR;
            }
        } else if (match_char(l, '=')) {
            token.kind = TOKEN_GE;
        } else {
            token.kind = TOKEN_GT;
        }
        break;

    case '<':
        if (match_char(l, '<')) {
            if (match_char(l, '=')) {
                token.kind = TOKEN_SHL_SET;
            } else {
                token.kind = TOKEN_SHL;
            }
        } else if (match_char(l, '=')) {
            token.kind = TOKEN_LE;
        } else {
            token.kind = TOKEN_LT;
        }
        break;

    case '=':
        if (match_char(l, '=')) {
            token.kind = TOKEN_EQ;
        } else {
            token.kind = TOKEN_SET;
        }
        break;

    default:
        error_invalid(token.pos, token.sv, "character");
        break;
    }

    token.sv.count -= l->sv.count;
    return token;
}

Token lexer_peek(Lexer *l) {
    if (!l->peeked) {
        lexer_buffer(l, lexer_next(l));
    }
    return l->buffer;
}

bool lexer_read(Lexer *l, TokenKind kind) {
    lexer_peek(l);
    l->peeked = l->buffer.kind != kind;
    return !l->peeked;
}

Token lexer_expect_impl(Lexer *l, const TokenKind *kinds) {
    const Token token = lexer_next(l);
    for (const TokenKind *it = kinds; *it != TOKEN_EOF; it++) {
        if (token.kind == *it) {
            return token;
        }
    }

    error_begin(ERROR, token.pos);
    fprintf(stderr, "Expected ");
    for (const TokenKind *it = kinds; *it != TOKEN_EOF; it++) {
        if (it != kinds) {
            fprintf(stderr, " or ");
        }

        fprintf(stderr, "%s", token_kind_to_cstr(*it));
    }

    fprintf(stderr, ", got %s", token_kind_to_cstr(token.kind));
    error_end(token.pos);
    exit(1);
}

Token lexer_split_token(Lexer *l, Token token) {
    switch (token.kind) {
    case TOKEN_SHR:
        token.kind = TOKEN_GT;
        break;

    case TOKEN_LAND:
        token.kind = TOKEN_BAND;
        break;

    default:
        unreachable();
    }
    token.sv.count = 1;

    Token remainder = token;
    remainder.pos.col++;
    lexer_buffer(l, remainder);
    return token;
}
