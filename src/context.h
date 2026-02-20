#ifndef CONTEXT_H
#define CONTEXT_H

#include "ast.h"

typedef Dynamic_Array(AST_Node_Atom *) Scope;

void           scope_push(Scope *scope, AST_Node_Atom *node);
AST_Node_Atom *scope_find(Scope scope, SV name);

struct Context_Fn {
    AST_Node_Fn *fn;

    size_t begin;
    size_t end;

    Context_Fn *outer;
};

AST_Node_Atom *context_fn_find(const Context_Fn *fn, const Scope *locals, SV name, bool only_consts);

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

void           context_push_local(Context *c, AST_Node_Atom *atom);
AST_Node_Atom *context_find_local(const Context *c, SV name);

void context_set_end(Context *c, size_t end);

#endif // CONTEXT_H
