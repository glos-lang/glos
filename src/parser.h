#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "node.h"

typedef struct Import  Import;
typedef struct Package Package;

struct Import {
    SV       as;
    Package *package;

    Import *next;
};

typedef struct {
    Import *head;
    Import *tail;
} Imports;

struct Package {
    SV      path;
    Token   name;
    Nodes   nodes;
    Imports imports;

    Package *next;
};

typedef struct {
    Package *head;
    Package *tail;
    Package *current;
} Packages;

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

void packages_push(Parser *p, Package *package);

bool parse_file(Parser *p, const char *path);
bool parse_dir(Parser *p, const char *path);

#endif // PARSER_H
