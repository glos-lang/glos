#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "node.h"

typedef struct {
    Arena *arena;

    Lexer lexer;
    Token ahead;
    bool  peeked;

    bool in_loop;
    bool in_extern;

    Node_Fn *fn_current;

    Nodes nodes;
} Parser;

bool parse_file(Parser *p, const char *path);

#endif // PARSER_H
