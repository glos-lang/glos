#ifndef CONTEXT_H
#define CONTEXT_H

#include "package.h"

Node *scope_find(Scope s, SV name, bool is_type);

typedef struct {
    NodeFn *fn;
    size_t  base;
} ContextFn;

typedef struct Methods Methods;

struct Methods {
    Type type;

    NodeFn *head;
    NodeFn *tail;

    Methods *next;
};

NodeFn *methods_find(Methods *ms, SV name);
void    methods_push(Methods *ms, NodeFn *m);

// TODO: Remove this artificial construct and spill into Compiler
typedef struct {
    Arena    *arena;
    Methods  *methods;
    Packages *packages;

    ContextFn fn;
    Scope     locals;
    Scope     statics;

    bool in_extern;
    bool checking_toplevels;

    Type str_type;
} Context;

ContextFn context_fn_begin(Context *c, NodeFn *fn);
void      context_fn_end(Context *c, ContextFn save);
Node     *context_fn_find(ContextFn f, Scope s, SV name, bool is_type);

Methods *context_methods_find(Context *c, Type type);
Methods *context_methods_alloc(Context *c, Type type);

void context_free(Context *c);

#endif // CONTEXT_H
