#ifndef CONTEXT_H
#define CONTEXT_H

#include "package.h"

Node *scope_find(Scope s, SV name, bool is_type);

typedef struct {
    NodeFn *fn;
    size_t  base;
} ContextFn;

// TODO: Remove this artificial construct and spill into Compiler
typedef struct {
    Arena    *arena;
    Packages *packages;

    ContextFn fn;
    Scope     locals;
    Scope     statics;

    Methods methods;

    bool in_extern;
    bool checking_toplevels;

    Type str_type;
} Context;

ContextFn context_fn_begin(Context *c, NodeFn *fn);
void      context_fn_end(Context *c, ContextFn save);
Node     *context_fn_find(ContextFn f, Scope s, SV name, bool is_type);

void context_free(Context *c);

#endif // CONTEXT_H
