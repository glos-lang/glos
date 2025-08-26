#ifndef PACKAGE_H
#define PACKAGE_H

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

void imports_push(Imports *is, Import *i);

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

void packages_push(Packages *ps, Package *p);

#endif // PACKAGE_H
