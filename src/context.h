#ifndef CONTEXT_H
#define CONTEXT_H

#include "node.h"

typedef Dynamic_Array(Node_Atom *) Scope;

void       scope_push(Scope *scope, Node_Atom *node);
Node_Atom *scope_find(Scope scope, SV name);

struct Context_Fn {
    Node_Fn *fn;

    size_t begin;
    size_t end;

    Context_Fn *outer;
};

Node_Atom *context_fn_find(const Context_Fn *fn, const Scope *locals, SV name, bool only_consts);

typedef struct {
    Scope       locals;
    Context_Fn *current;
} Context;

// The function `context_push_fn()` will modify the following fields of `fn`:
//   - begin
//   - end
void context_push_fn(Context *c, Context_Fn *fn);
void context_pop_fn(Context *c);
void context_restore_fn(Context *c, Context_Fn *save);

void       context_push_local(Context *c, Node_Atom *atom);
Node_Atom *context_find_local(const Context *c, SV name);

void context_set_end(Context *c, size_t end);

#endif // CONTEXT_H
