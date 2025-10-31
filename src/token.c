#include "token.h"

static_assert(COUNT_TOKENS == 74, "");
const char *token_kind_to_cstr(TokenKind kind) {
    switch (kind) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_EOL:
        return "';'";

    case TOKEN_DOT:
        return "'.'";

    case TOKEN_COLON:
        return "':'";

    case TOKEN_COMMA:
        return "','";

    case TOKEN_RANGE:
        return "'..'";

    case TOKEN_SCOPE:
        return "'::'";

    case TOKEN_VARIADIC:
        return "'...'";

    case TOKEN_NIL:
        return "'nil'";

    case TOKEN_INT:
        return "integer";

    case TOKEN_FLOAT:
        return "float";

    case TOKEN_STR:
        return "string";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_CHAR:
        return "character";

    case TOKEN_IDENT:
        return "identifier";

    case TOKEN_LPAREN:
        return "'('";

    case TOKEN_RPAREN:
        return "')'";

    case TOKEN_LBRACE:
        return "'{'";

    case TOKEN_RBRACE:
        return "'}'";

    case TOKEN_LBRACKET:
        return "'['";

    case TOKEN_RBRACKET:
        return "']'";

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

    case TOKEN_SHL:
        return "'<<'";

    case TOKEN_SHR:
        return "'>>'";

    case TOKEN_BOR:
        return "'|'";

    case TOKEN_BAND:
        return "'&'";

    case TOKEN_BNOT:
        return "'~'";

    case TOKEN_LOR:
        return "'||'";

    case TOKEN_LAND:
        return "'&&'";

    case TOKEN_LNOT:
        return "'!'";

    case TOKEN_SET:
        return "'='";

    case TOKEN_ADD_SET:
        return "'+='";

    case TOKEN_SUB_SET:
        return "'-='";

    case TOKEN_MUL_SET:
        return "'*='";

    case TOKEN_DIV_SET:
        return "'/='";

    case TOKEN_MOD_SET:
        return "'%='";

    case TOKEN_SHL_SET:
        return "'<<='";

    case TOKEN_SHR_SET:
        return "'>>='";

    case TOKEN_BOR_SET:
        return "'|='";

    case TOKEN_BAND_SET:
        return "'&='";

    case TOKEN_GT:
        return "'>'";

    case TOKEN_GE:
        return "'>='";

    case TOKEN_LT:
        return "'<'";

    case TOKEN_LE:
        return "'<='";

    case TOKEN_EQ:
        return "'=='";

    case TOKEN_NE:
        return "'!='";

    case TOKEN_SIZEOF:
        return "'sizeof'";

    case TOKEN_ASSERT:
        return "'assert'";

    case TOKEN_IF:
        return "'if'";

    case TOKEN_THEN:
        return "'then'";

    case TOKEN_ELSE:
        return "'else'";

    case TOKEN_FOR:
        return "'for'";

    case TOKEN_MATCH:
        return "'match'";

    case TOKEN_BREAK:
        return "'break'";

    case TOKEN_RETURN:
        return "'return'";

    case TOKEN_CONTINUE:
        return "'continue'";

    case TOKEN_FN:
        return "'fn'";

    case TOKEN_VAR:
        return "'var'";

    case TOKEN_TYPE:
        return "'type'";

    case TOKEN_CONST:
        return "'const'";

    case TOKEN_TRAIT:
        return "'trait'";

    case TOKEN_STRUCT:
        return "'struct'";

    case TOKEN_EXTERN:
        return "'extern'";

    case TOKEN_STATIC:
        return "'static'";

    case TOKEN_PUB:
        return "'pub'";

    case TOKEN_IMPORT:
        return "'import'";

    case TOKEN_PACKAGE:
        return "'package'";

    case TOKEN_WHEN:
        return "'when'";

    case TOKEN_PROP_OS:
        return "'#OS'";

    case TOKEN_PROP_LINK:
        return "'#link'";

    default:
        unreachable();
    }
}

static_assert(COUNT_TOKENS == 74, "");
Power token_kind_to_power(TokenKind kind) {
    switch (kind) {
    case TOKEN_DOT:
    case TOKEN_LPAREN:
    case TOKEN_LBRACE:
    case TOKEN_LBRACKET:
        return POWER_DOT;

    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_MOD:
        return POWER_MUL;

    case TOKEN_SHL:
    case TOKEN_SHR:
        return POWER_SHL;

    case TOKEN_BOR:
    case TOKEN_BAND:
        return POWER_BOR;

    case TOKEN_LOR:
    case TOKEN_LAND:
        return POWER_LOR;

    case TOKEN_SET:
    case TOKEN_ADD_SET:
    case TOKEN_SUB_SET:
    case TOKEN_MUL_SET:
    case TOKEN_DIV_SET:
    case TOKEN_MOD_SET:
    case TOKEN_SHL_SET:
    case TOKEN_SHR_SET:
    case TOKEN_BOR_SET:
    case TOKEN_BAND_SET:
        return POWER_SET;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return POWER_CMP;

    default:
        return POWER_NIL;
    }
}

SV resolve_str_token(Token token, Arena *a) {
    SV sv = token.sv;
    sv.data += 1;
    sv.count -= 2;
    resolve_escape_chars(arena_alloc(a, token.as.integer), &sv);
    return sv;
}
