#include "context.h"

void scope_push(Scope *scope, AST_Node_Atom *node) {
    da_push(scope, node);
}

AST_Node_Atom *scope_find(Scope *scope, SV name) {
    for (size_t i = scope->count; i; i--) {
        AST_Node_Atom *it = scope->data[i - 1];
        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}
