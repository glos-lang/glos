#ifndef CONTEXT_H
#define CONTEXT_H

#include "node.h"

typedef struct {
    Node **data;
    size_t count;
    size_t capacity;
} Scope;

Node *scope_find(Scope s, SV name);

typedef struct {
    NodeFn *fn;
    size_t  base;
} ContextFn;

typedef struct {
    Scope types;
    Scope locals;
    Scope globals;

    ContextFn fn;

    bool in_extern;
} Context;

ContextFn context_fn_begin(Context *c, NodeFn *fn);
void      context_fn_end(Context *c, ContextFn save);
Node     *context_fn_find(ContextFn f, Scope s, SV name);

void context_free(Context *c);

#endif // CONTEXT_H
