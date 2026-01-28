#include "ast.h"

void ast_nodes_push(AST_Nodes *ns, AST_Node *n) {
    if (!n) {
        return;
    }

    if (ns->tail) {
        ns->tail->next = n;
    } else {
        ns->head = n;
    }

    ns->tail = n;
}

#define Indent_Fmt    "%*s"
#define Indent_Arg(d) (d) * 4, ""

static void ast_node_debug_impl(FILE *f, AST_Node *n, int depth, const char *label) {
    if (!n) {
        return;
    }

    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    if (label) {
        fprintf(f, "%s = ", label);
    }

    switch (n->kind) {
    case AST_NODE_ATOM:
        fprintf(f, "Atom '" SV_Fmt "'\n", SV_Arg(n->token.sv));
        break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        fprintf(f, "Unary '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        ast_node_debug_impl(f, unary->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        fprintf(f, "Binary '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        ast_node_debug_impl(f, binary->lhs, depth + 1, "Lhs");
        ast_node_debug_impl(f, binary->rhs, depth + 1, "Rhs");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        fprintf(f, "Print {\n");
        ast_node_debug_impl(f, print->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case COUNT_AST_NODES:
        unreachable();
    }
}

static_assert(COUNT_AST_NODES == 4, "");
void ast_node_debug(FILE *f, AST_Node *n) {
    ast_node_debug_impl(f, n, 0, NULL);
}
