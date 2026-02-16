#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Arena *arena;

    Lexer lexer;
    Token ahead;
    bool  peeked;

    bool in_loop;
    bool in_extern;
    bool is_local;

    AST_Nodes nodes;
} Parser;

bool parse_file(Parser *p, const char *path);

#endif // PARSER_H
