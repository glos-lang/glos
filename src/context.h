#ifndef CONTEXT_H
#define CONTEXT_H

#include "ast.h"

typedef Dynamic_Array(AST_Node_Atom *) Scope;

void           scope_push(Scope *scope, AST_Node_Atom *node);
AST_Node_Atom *scope_find(Scope scope, SV name, size_t base);

#endif // CONTEXT_H
