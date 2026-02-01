#include "lexer.h"
#include <ctype.h>
#include <errno.h>

bool lexer_open(Lexer *l, const char *path, Arena *arena) {
    if (!read_file_path(path, &l->sv, arena)) {
        return false;
    }

    l->pos.path = path;
    return true;
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

static void skip_whitespace(Lexer *l) {
    l->newline = false;
    while (l->sv.count) {
        switch (*l->sv.data) {
        case ' ':
        case '\t':
        case '\r':
            next_char(l);
            break;

        case '\n':
            next_char(l);
            l->newline = true;
            break;

        case '#':
            if (l->pos.col == 0 && l->pos.row == 0 && peek_char(l, 1) == '!') {
                while (l->sv.count && *l->sv.data != '\n') {
                    next_char(l);
                }
            } else {
                return;
            }
            break;

        case '/':
            if (peek_char(l, 1) == '/') {
                while (l->sv.count && *l->sv.data != '\n') {
                    next_char(l);
                }
            } else {
                return;
            }
            break;

        default:
            return;
        }
    }
}

static void error_invalid(Pos pos, SV sv, const char *label) {
    if (isprint(*sv.data)) {
        fprintf(stderr, Pos_Fmt "ERROR: Invalid %s '%c'\n", Pos_Arg(pos), label, *sv.data);
    } else {
        fprintf(stderr, Pos_Fmt "ERROR: Invalid %s (%d)\n", Pos_Arg(pos), label, *sv.data);
    }
    exit(1);
}

static_assert(COUNT_TOKENS == 16, "");
Token lexer_iter(Lexer *l) {
    skip_whitespace(l);

    Token token = {
        .pos = l->pos,
        .sv = l->sv,
        .newline = l->newline,
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

        char *buffer = temp_sv_to_cstr(token.sv);
        memcpy(buffer, token.sv.data, token.sv.count);

        errno = 0;
        token.as.integer = strtol(buffer, NULL, 10);
        temp_reset(buffer);

        if (!errno) {
            return token;
        }

        fprintf(stderr, Pos_Fmt "ERROR: Number '" SV_Fmt "' is too large\n", Pos_Arg(token.pos), SV_Arg(token.sv));
        exit(1);
    }

    if (isident(*l->sv.data)) {
        while (l->sv.count > 0 && isident(*l->sv.data)) {
            next_char(l);
        }
        token.sv.count -= l->sv.count;

        if (sv_match(token.sv, "true")) {
            token.kind = TOKEN_BOOL;
            token.as.integer = true;
        } else if (sv_match(token.sv, "false")) {
            token.kind = TOKEN_BOOL;
            token.as.integer = false;
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

    case '+':
        token.kind = TOKEN_ADD;
        break;

    case '-':
        token.kind = TOKEN_SUB;
        break;

    case '*':
        token.kind = TOKEN_MUL;
        break;

    case '/':
        token.kind = TOKEN_DIV;
        break;

    case '%':
        token.kind = TOKEN_MOD;
        break;

    case '!':
        token.kind = TOKEN_LNOT;
        break;

    default:
        error_invalid(token.pos, token.sv, "character");
        break;
    }

    token.sv.count -= l->sv.count;
    return token;
}
