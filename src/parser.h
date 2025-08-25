#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "node.h"

typedef struct {
    Arena *arena;
    Lexer  lexer;
    Token  package;

    bool local;
    bool in_extern;
    bool dont_consume_eols;

    Paths paths;
    Nodes nodes;
} Parser;

void parser_free(Parser *p);

bool parse_file(Parser *p, const char *path);
bool parse_dir(Parser *p, const char *path);

#endif // PARSER_H
