#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    Arena *arena;

    Pos  pos;
    SV   sv;
    bool newline;
} Lexer;

bool  lexer_open(Lexer *l, const char *path, Arena *a);
Token lexer_iter(Lexer *l);

#endif // LEXER_H
