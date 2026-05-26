#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "node.h"

typedef DA(const char *) Paths;

typedef struct {
    Lexer lexer;
    Token ahead;
    bool  peeked;

    bool in_loop;
    bool in_defer;
    bool in_extern;
    bool in_compile_time_condition;

    Node_Fn *fn_current;
} Parser_State;

typedef struct {
    Arena *arena;
    Paths  paths;

    // TODO: Should these be SV?
    const char *cwd;
    const char *std;
    const char *root;

    Parser_State state;

    Module  *module_current;
    Modules *modules;
} Parser;

Module *module_get(Parser *p, const char *path); // `path` is absolute

typedef enum {
    PARSE_OK,
    PARSE_FAILURE,
    PARSE_EMPTY_DIRECTORY,
} Parse_Result;

void parser_free(Parser *p);
void parser_import(Parser *p, Node_Import *import);

Parse_Result parse_file(Parser *p, const char *path);
Parse_Result parse_directory(Parser *p, const char *path);

#endif // PARSER_H
