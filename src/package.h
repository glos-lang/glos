#ifndef PACKAGE_H
#define PACKAGE_H

#include "node.h"

typedef struct Import Import;

struct Import {
    SV   as;
    bool aliased;

    Token    token;
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
    SV    relative_path;
    SV    absolute_path;
    Token name;

    bool is_file;

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
Package *packages_find_by_path(Packages ps, SV abspath);

#endif // PACKAGE_H
