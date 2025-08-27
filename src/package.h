#ifndef PACKAGE_H
#define PACKAGE_H

#include "node.h"

typedef struct Import Import;

struct Import {
    SV       as;
    Package *package;

    Import *next;
};

typedef struct {
    Import *head;
    Import *tail;
} Imports;

void imports_push(Imports *is, Import *i);

typedef struct {
    Node **data;
    size_t count;
    size_t capacity;
} Scope;

struct Package {
    SV    path;
    Token name;

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

#endif // PACKAGE_H
