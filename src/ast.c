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

static_assert(COUNT_AST_TYPES == 5, "");
static const char *ast_type_to_cstr_impl(AST_Type type) {
    const char *s = temp_alloc(0);
    for (size_t i = 0; i < type.ref; i++) {
        temp_sprintf("&");
        temp_remove_null();
    }

    switch (type.kind) {
    case AST_TYPE_UNIT:
        temp_sprintf("void");
        break;

    case AST_TYPE_BOOL:
        temp_sprintf("bool");
        break;

    case AST_TYPE_I64:
        temp_sprintf("i64");
        break;

    case AST_TYPE_FN:
        temp_sprintf("(");

        for (size_t i = 0; i < type.spec.fn.arity; i++) {
            if (i) {
                temp_remove_null();
                temp_sprintf(", ");
            }

            temp_remove_null();
            temp_sprintf("_: ");

            temp_remove_null();
            ast_type_to_cstr_impl(type.spec.fn.args[i]);
        }

        temp_remove_null();
        temp_sprintf(")");

        if (type.spec.fn.returnn->kind != AST_TYPE_UNIT) {
            temp_remove_null();
            temp_sprintf(" -> ");

            temp_remove_null();
            ast_type_to_cstr_impl(*type.spec.fn.returnn);
        }
        break;

    case AST_TYPE_TYPE:
        unreachable();

    default:
        unreachable();
    }

    return s;
}

const char *ast_type_to_cstr(AST_Type type) {
    if (type.kind == AST_TYPE_TYPE) {
        return temp_sprintf("a type");
    }

    const char *s = temp_sprintf("'");
    temp_remove_null();
    ast_type_to_cstr_impl(type);
    temp_remove_null();
    temp_sprintf("'");
    return s;
}

static_assert(COUNT_AST_TYPES == 5, "");
bool ast_type_eq(AST_Type a, AST_Type b) {
    if (a.kind != b.kind || a.ref != b.ref) {
        return false;
    }

    switch (a.kind) {
    case AST_TYPE_FN: {
        if (a.spec.fn.arity != b.spec.fn.arity) {
            return false;
        }

        for (size_t i = 0; i < a.spec.fn.arity; i++) {
            if (!ast_type_eq(a.spec.fn.args[i], b.spec.fn.args[i])) {
                return false;
            }
        }

        return ast_type_eq(*a.spec.fn.returnn, *b.spec.fn.returnn);
    }

    default:
        return true;
    }
}

static_assert(COUNT_AST_TYPES == 5, "");
bool ast_type_is_numeric(AST_Type type) {
    if (type.ref) {
        return false;
    }

    switch (type.kind) {
    case AST_TYPE_I64:
        return true;

    default:
        return false;
    }
}

bool ast_type_is_pointer(AST_Type type) {
    return type.ref != 0;
}

bool ast_type_is_scalar(AST_Type type) {
    if (ast_type_is_numeric(type) || ast_type_is_pointer(type)) {
        return true;
    }

    if (type.kind == AST_TYPE_BOOL) {
        return true;
    }

    return false;
}

#define Indent_Fmt    "%*s"
#define Indent_Arg(d) (d) * 4, ""

static void ast_node_debug_impl(FILE *f, AST_Node *n, int depth, const char *label);

static void ast_nodes_debug_impl(FILE *f, AST_Nodes ns, int depth, const char *label) {
    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    if (label) {
        fprintf(f, "%s = ", label);
    }

    for (AST_Node *it = ns.head; it; it = it->next) {
        ast_node_debug_impl(f, it, depth, NULL);
    }
}

static_assert(COUNT_AST_NODES == 12, "");
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

    case AST_NODE_FN: {
        AST_Node_Fn *fn = (AST_Node_Fn *) n;
        fprintf(f, "Fn {\n");
        ast_nodes_debug_impl(f, fn->args, depth + 1, "Args");
        ast_node_debug_impl(f, fn->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_CALL: {
        AST_Node_Call *call = (AST_Node_Call *) n;
        fprintf(f, "Call {\n");
        ast_node_debug_impl(f, call->fn, depth + 1, "Fn");
        ast_nodes_debug_impl(f, call->args, depth + 1, "Args");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        fprintf(f, "Define %s {\n", define->is_const ? "constant" : "variable");
        ast_node_debug_impl(f, define->name, depth + 1, "Name");
        ast_node_debug_impl(f, define->type, depth + 1, "Type");
        ast_node_debug_impl(f, define->expr, depth + 1, "Expr");
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

    case AST_NODE_FOR: {
        AST_Node_For *forr = (AST_Node_For *) n;
        fprintf(f, "For {\n");
        ast_node_debug_impl(f, forr->init, depth + 1, "Init");
        ast_node_debug_impl(f, forr->condition, depth + 1, "Condition");
        ast_node_debug_impl(f, forr->update, depth + 1, "Update");
        ast_node_debug_impl(f, forr->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_JUMP:
        if (n->token.kind == TOKEN_BREAK) {
            fprintf(f, "Break\n");
        } else if (n->token.kind == TOKEN_CONTINUE) {
            fprintf(f, "Continue\n");
        } else {
            unreachable();
        }
        break;

    case AST_NODE_RETURN: {
        AST_Node_Return *returnn = (AST_Node_Return *) n;
        fprintf(f, "Return {\n");
        ast_node_debug_impl(f, returnn->value, depth + 1, "Value");
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
    ast_nodes_debug_impl(f, ns, 0, NULL);
}
