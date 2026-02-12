#include "context.h"

void scope_push(Scope *decls, AST_Node_Atom *node) {
    da_push(decls, node);
}

AST_Node_Atom *scope_find(Scope *decls, SV name) {
    for (size_t i = decls->count; i; i--) {
        AST_Node_Atom *it = decls->data[i - 1];
        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}
