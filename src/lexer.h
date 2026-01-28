#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    Pos  pos;
    SV   sv;
    bool newline;
} Lexer;

bool  lexer_open(Lexer *l, const char *path, Arena *arena);
Token lexer_iter(Lexer *l);

#endif // LEXER_H
