#include "context.h"

AST_Node *decls_find(Decls *decls, SV name) {
    for (size_t i = decls->count; i; i--) {
        AST_Node *it = decls->data[i - 1];
        if (sv_eq(it->token.sv, name)) {
            return it;
        }
    }

    return NULL;
}
