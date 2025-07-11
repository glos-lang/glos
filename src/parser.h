#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "node.h"

typedef struct {
    Arena *arena;
    Lexer  lexer;

    bool local;
    bool in_extern;
    bool dont_consume_eols;
    bool yield_usable;

    Nodes nodes;
} Parser;

void parse_file(Parser *p, Lexer lexer);

#endif // PARSER_H
