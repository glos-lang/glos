#ifndef MODULE_H
#define MODULE_H

#include "node.h"

typedef struct Import Import;

struct Import {
    SV   as;
    bool aliased;

    Token   token;
    Module *module;

    Import *next;
};

typedef struct {
    Import *head;
    Import *tail;
} Imports;

void imports_push(Imports *is, Import *i);

typedef DynamicArray(Node *) Scope;

struct Module {
    SV    path;
    Token name;

    bool        is_file;
    const char *real_path;

    Nodes   nodes;
    Scope   globals;
    Imports imports;

    Module *next;
};

typedef struct {
    Module *head;
    Module *tail;
    Module *current;
} Modules;

void modules_free(Modules *ps);
void modules_push(Modules *ps, Module *p);

Module *modules_find_by_name(Modules ps, SV name);
Module *modules_find_by_path(Modules ps, SV name);

#endif // MODULE_H
