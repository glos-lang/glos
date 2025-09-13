#ifndef PACKAGE_H
#define PACKAGE_H

#include "node.h"

typedef struct Import Import;

struct Import {
    SV       as;
    Pos      pos;
    Package *package;

    Import *next;
};

typedef struct {
    Import *head;
    Import *tail;
} Imports;

void imports_push(Imports *is, Import *i);

typedef DynamicArray(Node *) Scope;

struct Package {
    SV    path;
    Token name;

    bool        is_file;
    const char *real_path;

    Nodes   nodes;
    Scope   globals;
    Imports imports;

    Package *next;
};

typedef struct {
    Package *head;
    Package *tail;
    Package *current;
} Packages;

void packages_free(Packages *ps);
void packages_push(Packages *ps, Package *p);

Package *packages_find_by_name(Packages ps, SV name);
Package *packages_find_by_path(Packages ps, SV name);

#endif // PACKAGE_H
