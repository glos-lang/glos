#include "context.h"

void scope_push(Scope *scope, AST_Node_Atom *node) {
    da_push(scope, node);
}

AST_Node_Atom *scope_find(Scope scope, SV name, size_t base) {
    assert(base <= scope.count);
    for (size_t i = scope.count; i > 0; i--) {
        AST_Node_Atom *it = scope.data[i - 1];
        if (!it->is_const && i <= base) {
            // Local variables cannot be accessed from inner functions
            continue;
        }

        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}
