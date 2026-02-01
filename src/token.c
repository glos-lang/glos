#include "token.h"

static_assert(COUNT_TOKENS == 14, "");
const char *token_kind_to_cstr(Token_Kind kind) {
    switch (kind) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_EOL:
        return "';'";

    case TOKEN_INT:
        return "integer";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_IDENT:
        return "identifier";

    case TOKEN_LPAREN:
        return "'('";

    case TOKEN_RPAREN:
        return "')'";

    case TOKEN_ADD:
        return "'+'";

    case TOKEN_SUB:
        return "'-'";

    case TOKEN_MUL:
        return "'*'";

    case TOKEN_DIV:
        return "'/'";

    case TOKEN_MOD:
        return "'%'";

    case TOKEN_LNOT:
        return "'!'";

    case TOKEN_PRINT:
        return "'print'";

    default:
        unreachable();
    }
}
