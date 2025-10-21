#ifndef PARSER_H
#define PARSER_H

#include "formatter.h"

typedef struct {
    Arena    *arena;
    Packages *packages;

    const char *cwd;
    const char *std;
    const char *root;

    Lexer lexer;

    Formatter *formatter;
    bool       formatter_failed;

    bool local;
    bool in_loop;
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

ParseDirError parse_dir(Parser *p, const char *path, bool check_in_std);

#endif // PARSER_H
