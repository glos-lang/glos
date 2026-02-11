#ifndef TOKEN_H
#define TOKEN_H

#include "basic.h"

typedef struct {
    const char *path;
    size_t      row;
    size_t      col;
} Pos;

#define Pos_Fmt    "%s:%zu:%zu: "
#define Pos_Arg(p) ((p).path), ((p).row + 1), ((p).col + 1)

typedef enum {
    TOKEN_EOF,
    TOKEN_EOL,
    TOKEN_COLON,

    TOKEN_INT,
    TOKEN_BOOL,
    TOKEN_IDENT,

    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,

    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,
    TOKEN_MOD,

    TOKEN_SET,

    TOKEN_LNOT,

    TOKEN_GT,
    TOKEN_GE,
    TOKEN_LT,
    TOKEN_LE,
    TOKEN_EQ,
    TOKEN_NE,

    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,

    TOKEN_PRINT,
    COUNT_TOKENS
} Token_Kind;

const char *token_kind_to_cstr(Token_Kind kind);

typedef struct {
    Token_Kind kind;

    SV   sv;
    Pos  pos;
    bool newline;

    union {
        size_t integer;
    } as;
} Token;

#endif // TOKEN_H
