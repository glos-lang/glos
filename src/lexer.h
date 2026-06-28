#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    Pos  pos;
    SV   sv;
    bool newline;
} Lexer;

bool  lexer_open(Lexer *l, const char *path);
Token lexer_get_string(Lexer *l, Pos pos);
Token lexer_iter(Lexer *l);

#endif // LEXER_H
