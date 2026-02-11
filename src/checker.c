#include "checker.h"
#include <stdnoreturn.h>

static noreturn void error_undefined(const AST_Node *n) {
    fprintf(stderr, Pos_Fmt "ERROR: Undefined identifier '" SV_Fmt "'\n", Pos_Arg(n->token.pos), SV_Arg(n->token.sv));
    exit(1);
}

static noreturn void error_redefinition(const AST_Node *n, const AST_Node *previous) {
    fprintf(stderr, Pos_Fmt "ERROR: Redefinition of '" SV_Fmt "'\n", Pos_Arg(n->token.pos), SV_Arg(n->token.sv));
    if (previous) {
        fprintf(stderr, Pos_Fmt "NOTE: Defined here\n", Pos_Arg(previous->token.pos));
    }
    exit(1);
}

static noreturn void error_type_mismatch(AST_Node *actual, AST_Type expected) {
    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected %s, got %s\n",
        Pos_Arg(actual->token.pos),
        ast_type_to_cstr(expected),
        ast_type_to_cstr(actual->type));

    exit(1);
}

static AST_Type ast_type_assert(AST_Node *actual, AST_Type expected) {
    if (ast_type_eq(actual->type, expected)) {
        return actual->type;
    }

    error_type_mismatch(actual, expected);
}

static AST_Type ast_type_assert_node(AST_Node *actual, AST_Node *expected) {
    if (ast_type_eq(actual->type, expected->type)) {
        return actual->type;
    }

    error_type_mismatch(actual, expected->type);
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

static_assert(COUNT_AST_TYPES == 4, "");
static const char *builtin_type_names[COUNT_AST_TYPES] = {
    [AST_TYPE_BOOL] = "bool",
    [AST_TYPE_I64] = "i64",
};

static_assert(COUNT_AST_NODES == 8, "");
static void check_expr(Compiler *c, AST_Node *n, bool ref) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        AST_Node_Atom *atom = (AST_Node_Atom *) n;
        static_assert(COUNT_TOKENS == 27, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
            n->type = (AST_Type) {.kind = AST_TYPE_BOOL};
            break;

        case TOKEN_INT:
            n->type = (AST_Type) {.kind = AST_TYPE_I64};
            break;

        case TOKEN_IDENT: {
            atom->definition = decls_find(&c->globals, n->token.sv);
            if (atom->definition) {
                n->type = atom->definition->type;
                n->allow_ref = true;
            } else {
                bool ok = false;
                for (AST_Type_Kind kind = 0; kind < COUNT_AST_TYPES; kind++) {
                    const char *name = builtin_type_names[kind];
                    if (name && sv_match(n->token.sv, name)) {
                        const AST_Type type = {.kind = kind};
                        n->type = (AST_Type) {
                            .kind = AST_TYPE_TYPE,
                            .spec.type = arena_clone(c->llvm.arena, &type, sizeof(type)),
                        };

                        ok = true;
                        break;
                    }
                }

                if (!ok) {
                    error_undefined(n);
                }
            }
        } break;

        default:
            unreachable();
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        check_expr(c, unary->value, false);

        static_assert(COUNT_TOKENS == 27, "");
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
        static_assert(COUNT_TOKENS == 27, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            ast_type_assert_numeric(binary->lhs);
            n->type = ast_type_assert_node(binary->rhs, binary->lhs);
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            ast_type_assert_numeric(binary->lhs);
            ast_type_assert_node(binary->rhs, binary->lhs);
            n->type = (AST_Type) {.kind = AST_TYPE_BOOL};
            break;

        case TOKEN_SET:
            check_expr(c, binary->lhs, true);
            check_expr(c, binary->rhs, false);
            ast_type_assert_node(binary->rhs, binary->lhs);
            n->type = (AST_Type) {.kind = AST_TYPE_UNIT};
            break;

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }

    if (!n->allow_ref && ref) {
        fprintf(stderr, Pos_Fmt "ERROR: Cannot take reference to value not in memory\n", Pos_Arg(n->token.pos));
        exit(1);
    }
}

static_assert(COUNT_AST_NODES == 8, "");
static void check_stmt(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_DECL: {
        AST_Node_Decl *decl = (AST_Node_Decl *) n;

        assert(decl->name->kind == AST_NODE_ATOM && decl->name->token.kind == TOKEN_IDENT);
        AST_Node *it = decl->name;

        for (AST_Type_Kind kind = 0; kind < COUNT_AST_TYPES; kind++) {
            const char *name = builtin_type_names[kind];
            if (name && sv_match(it->token.sv, name)) {
                error_redefinition(it, NULL);
            }
        }

        AST_Node *previous = decls_find(&c->globals, it->token.sv);
        if (previous) {
            error_redefinition(it, previous);
        }

        if (decl->type) {
            check_expr(c, decl->type, false);
            ast_type_assert(decl->type, (AST_Type) {.kind = AST_TYPE_TYPE});
            it->type = *decl->type->type.spec.type;
        }

        if (decl->expr) {
            check_expr(c, decl->expr, false);

            if (decl->expr->type.kind == AST_TYPE_UNIT || decl->expr->type.kind == AST_TYPE_TYPE) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot store %s in a variable\n",
                    Pos_Arg(decl->expr->token.pos),
                    ast_type_to_cstr(decl->expr->type));

                exit(1);
            }

            if (decl->type) {
                ast_type_assert(decl->expr, it->type);
            } else {
                it->type = decl->expr->type;
            }
        }

        da_push(&c->globals, it);
    } break;

    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }
    } break;

    case AST_NODE_IF: {
        AST_Node_If *iff = (AST_Node_If *) n;
        check_expr(c, iff->condition, false);
        ast_type_assert(iff->condition, (AST_Type) {.kind = AST_TYPE_BOOL});
        check_stmt(c, iff->consequence);
        check_stmt(c, iff->antecedence);
    } break;

    case AST_NODE_FOR: {
        AST_Node_For *forr = (AST_Node_For *) n;
        check_stmt(c, forr->init);
        check_expr(c, forr->condition, false);
        if (forr->condition) {
            ast_type_assert(forr->condition, (AST_Type) {.kind = AST_TYPE_BOOL});
        }
        check_stmt(c, forr->update);
        check_stmt(c, forr->body);
    } break;

    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        check_expr(c, print->value, false);
        ast_type_assert_scalar(print->value);
    } break;

    default:
        check_expr(c, n, false);
        break;
    }
}

void check_nodes(Compiler *c, AST_Nodes nodes) {
    for (AST_Node *it = nodes.head; it; it = it->next) {
        check_stmt(c, it);
    }
}
