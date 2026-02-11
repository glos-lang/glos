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

static_assert(COUNT_AST_TYPES == 4, "");
static const char *ast_type_to_cstr_impl(AST_Type type) {
    switch (type.kind) {
    case AST_TYPE_UNIT:
        return "()";

    case AST_TYPE_BOOL:
        return "bool";

    case AST_TYPE_I64:
        return "i64";

    case AST_TYPE_TYPE:
        unreachable();

    default:
        unreachable();
    }
}

const char *ast_type_to_cstr(AST_Type type) {
    if (type.kind == AST_TYPE_TYPE) {
        return temp_sprintf("a type");
    }

    return temp_sprintf("'%s'", ast_type_to_cstr_impl(type));
}

bool ast_type_eq(AST_Type a, AST_Type b) {
    return a.kind == b.kind;
}

static_assert(COUNT_AST_TYPES == 4, "");
bool ast_type_is_numeric(AST_Type type) {
    switch (type.kind) {
    case AST_TYPE_I64:
        return true;

    default:
        return false;
    }
}

#define Indent_Fmt    "%*s"
#define Indent_Arg(d) (d) * 4, ""

static_assert(COUNT_AST_NODES == 7, "");
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
        fprintf(f, "Atom " SV_Fmt "\n", SV_Arg(n->token.sv));
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

    case AST_NODE_DECL: {
        AST_Node_Decl *decl = (AST_Node_Decl *) n;
        fprintf(f, "Define {\n");
        ast_node_debug_impl(f, decl->name, depth + 1, "Name");
        ast_node_debug_impl(f, decl->type, depth + 1, "Type");
        ast_node_debug_impl(f, decl->expr, depth + 1, "Expr");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;
        fprintf(f, "Block {\n");
        for (AST_Node *it = block->body.head; it; it = it->next) {
            ast_node_debug_impl(f, it, depth + 1, NULL);
        }
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_IF: {
        AST_Node_If *iff = (AST_Node_If *) n;
        fprintf(f, "If {\n");
        ast_node_debug_impl(f, iff->condition, depth + 1, "Condition");
        ast_node_debug_impl(f, iff->consequence, depth + 1, "Consequence");
        ast_node_debug_impl(f, iff->antecedence, depth + 1, "Antecedence");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        fprintf(f, "Print {\n");
        ast_node_debug_impl(f, print->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    default:
        unreachable();
    }
}

void ast_node_debug(FILE *f, AST_Node *n) {
    ast_node_debug_impl(f, n, 0, NULL);
}

void ast_nodes_debug(FILE *f, AST_Nodes ns) {
    for (AST_Node *it = ns.head; it; it = it->next) {
        ast_node_debug(f, it);
    }
}
