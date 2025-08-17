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
    char  *data;
    size_t count;
    size_t capacity;
} ContextMem;

typedef struct {
    Arena *arena;

    Scope types;
    Scope locals;
    Scope globals;

    ContextFn  fn;
    ContextMem memory;

    bool in_extern;

    Type str_type;
    Type cstr_type;
} Context;

ContextFn context_fn_begin(Context *c, NodeFn *fn);
void      context_fn_end(Context *c, ContextFn save);
Node     *context_fn_find(ContextFn f, Scope s, SV name);

size_t context_memory_alloc(Context *c, size_t size);
void  *context_memory_read(Context *c, size_t offset);
void   context_memory_write(Context *c, size_t offset, const void *src, size_t size);

void context_free(Context *c);

#endif // CONTEXT_H
