#ifndef TOKEN_H
#define TOKEN_H

#include "basic.h"

typedef struct {
    const char *path;
    size_t      row;
    size_t      col;

    SV line;
} Pos;

#define PosFmt    "%s:%zu:%zu:"
#define PosArg(p) ((p).path), ((p).row + 1), ((p).col + 1)

typedef enum {
    TOKEN_EOF,
    TOKEN_EOL,
    TOKEN_DOT,
    TOKEN_COLON,
    TOKEN_COMMA,
    TOKEN_RANGE,
    TOKEN_SCOPE,
    TOKEN_VARIADIC,

    TOKEN_NIL,
    TOKEN_INT,
    TOKEN_STR,
    TOKEN_BOOL,
    TOKEN_CHAR,
    TOKEN_IDENT,

    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,

    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,
    TOKEN_MOD,

    TOKEN_SHL,
    TOKEN_SHR,
    TOKEN_BOR,
    TOKEN_BAND,
    TOKEN_BNOT,

    TOKEN_SET,
    TOKEN_ADD_SET,
    TOKEN_SUB_SET,
    TOKEN_MUL_SET,
    TOKEN_DIV_SET,
    TOKEN_MOD_SET,
    TOKEN_SHL_SET,
    TOKEN_SHR_SET,
    TOKEN_BOR_SET,
    TOKEN_BAND_SET,

    TOKEN_LOR,
    TOKEN_LAND,
    TOKEN_LNOT,

    TOKEN_GT,
    TOKEN_GE,
    TOKEN_LT,
    TOKEN_LE,
    TOKEN_EQ,
    TOKEN_NE,

    TOKEN_SIZEOF,
    TOKEN_ASSERT,

    TOKEN_IF,
    TOKEN_THEN,
    TOKEN_ELSE,
    TOKEN_FOR,

    TOKEN_BREAK,
    TOKEN_RETURN,
    TOKEN_CONTINUE,

    TOKEN_FN,
    TOKEN_VAR,
    TOKEN_TYPE,
    TOKEN_CONST,
    TOKEN_TRAIT,
    TOKEN_STRUCT,
    TOKEN_EXTERN,
    TOKEN_STATIC,

    TOKEN_PUB,
    TOKEN_IMPORT,
    TOKEN_PACKAGE,
    TOKEN_WHEN,

    TOKEN_PROP_OS,
    TOKEN_PROP_LINK,
    COUNT_TOKENS
} TokenKind;

const char *token_kind_to_cstr(TokenKind kind);

typedef enum {
    POWER_NIL,
    POWER_SET,
    POWER_LOR,
    POWER_CMP,
    POWER_SHL,
    POWER_ADD,
    POWER_BOR,
    POWER_MUL,
    POWER_PRE,
    POWER_DOT
} Power;

Power token_kind_to_power(TokenKind kind);

typedef struct {
    TokenKind kind;

    SV     sv;
    Pos    pos;
    size_t newlines;

    union {
        bool   boolean;
        size_t integer;
    } as;
} Token;

SV resolve_str_token(Token token, Arena *a);

#endif // TOKEN_H
