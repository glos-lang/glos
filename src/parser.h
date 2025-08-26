#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "package.h"

typedef struct {
    Arena   *arena;
    Packages packages;

    Lexer lexer;

    bool local;
    bool in_extern;
    bool dont_consume_eols;

    Paths paths; // TODO: Get the list of items immediately to bypass this
} Parser;

void parser_free(Parser *p);

bool parse_file(Parser *p, const char *path);
bool parse_dir(Parser *p, const char *path);

#endif // PARSER_H
