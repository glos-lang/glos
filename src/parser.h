#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "package.h"

typedef struct {
    Arena    *arena;
    Packages *packages;

    SV    cwd;
    Lexer lexer;

    bool local;
    bool in_extern;
    bool dont_consume_eols;

    Paths paths;
} Parser;

void parser_free(Parser *p);

bool parse_file(Parser *p, const char *path);

typedef enum {
    PDE_NONE,
    PDE_EMPTY,
    PDE_FAILED,
} ParseDirError;

ParseDirError parse_dir(Parser *p, const char *path);

#endif // PARSER_H
