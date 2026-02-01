#include "checker.h"

static AST_Type ast_type_assert(AST_Node *actual, AST_Type expected) {
    if (ast_type_eq(actual->type, expected)) {
        return actual->type;
    }

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected type '%s', got '%s'\n",
        Pos_Arg(actual->token.pos),
        ast_type_to_cstr(expected),
        ast_type_to_cstr(actual->type));

    exit(1);
}

static AST_Type ast_type_assert_node(AST_Node *actual, AST_Node *expected) {
    if (ast_type_eq(actual->type, expected->type)) {
        return actual->type;
    }

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected type '%s', got '%s'\n",
        Pos_Arg(actual->token.pos),
        ast_type_to_cstr(expected->type),
        ast_type_to_cstr(actual->type));

    exit(1);
}

static AST_Type ast_type_assert_numeric(const AST_Node *n) {
    if (ast_type_is_numeric(n->type)) {
        return n->type;
    }

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected arithmetic type, got '%s'\n",
        Pos_Arg(n->token.pos),
        ast_type_to_cstr(n->type));
    exit(1);
}

static AST_Type ast_type_assert_scalar(const AST_Node *n) {
    if (ast_type_is_numeric(n->type)) {
        return n->type;
    }

    if (n->type.kind == AST_TYPE_BOOL) {
        return n->type;
    }

    fprintf(
        stderr, Pos_Fmt "ERROR: Expected scalar type, got '%s'\n", Pos_Arg(n->token.pos), ast_type_to_cstr(n->type));
    exit(1);
}

static_assert(COUNT_AST_NODES == 4, "");
static void check_expr(AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        static_assert(COUNT_TOKENS == 14, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
            n->type = (AST_Type) {.kind = AST_TYPE_BOOL};
            break;

        case TOKEN_INT:
            n->type = (AST_Type) {.kind = AST_TYPE_I64};
            break;

        default:
            unreachable();
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        check_expr(unary->value);

        static_assert(COUNT_TOKENS == 14, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            n->type = ast_type_assert_numeric(unary->value);
            break;

        case TOKEN_LNOT:
            n->type = ast_type_assert(unary->value, (AST_Type) {.kind = AST_TYPE_BOOL});
            break;

        default:
            unreachable();
        }
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        check_expr(binary->lhs);
        check_expr(binary->rhs);

        static_assert(COUNT_TOKENS == 14, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD: {
            ast_type_assert_numeric(binary->lhs);
            n->type = ast_type_assert_node(binary->rhs, binary->lhs);
        } break;

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_AST_NODES == 4, "");
static void check_stmt(AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        check_expr(print->value);
        ast_type_assert_scalar(print->value);
    } break;

    default:
        check_expr(n);
        break;
    }
}

void check_nodes(AST_Nodes nodes) {
    for (AST_Node *it = nodes.head; it; it = it->next) {
        check_stmt(it);
    }
}
