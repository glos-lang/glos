#ifndef PARSER_H
#define PARSER_H

#include "formatter.h"

typedef struct {
    Arena    *arena;
    Packages *packages;

    const char *cwd;
    const char *std;
    const char *root;

    Lexer    lexer;
    Comments comments;

    Formatter *formatter;
    bool       formatter_failed;
    bool       documenting;

    bool local;
    bool in_loop;
    bool in_when;
    bool in_defer;
    bool in_extern;
    bool dont_consume_eols;
    bool imported_builtin;

    Paths paths;
} Parser;

void parser_load_builtin(Parser *p);
void parser_free(Parser *p);

bool parse_file(Parser *p, const char *path);

typedef enum {
    PDE_NONE,
    PDE_EMPTY,
    PDE_FAILED,
} ParseDirError;

ParseDirError parse_dir(Parser *p, const char *path);

#endif // PARSER_H
