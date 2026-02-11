#ifndef CONTEXT_H
#define CONTEXT_H

#include "ast.h"

typedef Dynamic_Array(AST_Node *) Decls;

AST_Node *decls_find(Decls *decls, SV name);

#endif // CONTEXT_H
