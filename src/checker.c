#include "checker.h"

static void error_undefined(const AST_Node *n) {
    fprintf(stderr, Pos_Fmt "ERROR: Undefined identifier '" SV_Fmt "'\n", Pos_Arg(n->token.pos), SV_Arg(n->token.sv));
    exit(1);
}

static void error_redefinition(const AST_Node_Atom *n, const AST_Node_Atom *previous) {
    fprintf(
        stderr, Pos_Fmt "ERROR: Redefinition of '" SV_Fmt "'\n", Pos_Arg(n->node.token.pos), SV_Arg(n->node.token.sv));
    if (previous) {
        fprintf(stderr, Pos_Fmt "NOTE: Defined here\n", Pos_Arg(previous->node.token.pos));
    }
    exit(1);
}

static AST_Type ast_type_assert(AST_Node *actual, AST_Type expected) {
    if (ast_type_eq(actual->type, expected)) {
        return actual->type;
    }

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected %s, got %s\n",
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
        Pos_Fmt "ERROR: Expected %s, got %s\n",
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
        stderr, Pos_Fmt "ERROR: Expected arithmetic value, got %s\n", Pos_Arg(n->token.pos), ast_type_to_cstr(n->type));
    exit(1);
}

static AST_Type ast_type_assert_scalar(const AST_Node *n) {
    if (ast_type_is_numeric(n->type)) {
        return n->type;
    }

    if (n->type.kind == AST_TYPE_BOOL) {
        return n->type;
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected scalar value, got %s\n", Pos_Arg(n->token.pos), ast_type_to_cstr(n->type));
    exit(1);
}

static_assert(COUNT_AST_TYPES == 4, "");
static const char *builtin_type_names[COUNT_AST_TYPES] = {
    [AST_TYPE_BOOL] = "bool",
    [AST_TYPE_I64] = "i64",
};

static void check_ident(Compiler *c, AST_Node *n) {
    AST_Node_Atom *atom = (AST_Node_Atom *) n;
    AST_Node_Atom *definition = scope_find(&c->globals, n->token.sv);
    atom->as.reference.definition = definition;

    if (definition) {
        n->type = definition->node.type;
        n->is_memory = !definition->as.definition.is_const;
        return;
    }

    for (AST_Type_Kind kind = 0; kind < COUNT_AST_TYPES; kind++) {
        const char *name = builtin_type_names[kind];
        if (name && sv_match(n->token.sv, name)) {
            const AST_Type type = {.kind = kind};
            n->type = (AST_Type) {
                .kind = AST_TYPE_TYPE,
                .spec.type = arena_clone(c->llvm.arena, &type, sizeof(type)),
            };
            return;
        }
    }

    error_undefined(n);
}

static_assert(COUNT_AST_NODES == 9, "");
static void check_expr(Compiler *c, AST_Node *n, bool ref) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        static_assert(COUNT_TOKENS == 29, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
            n->type = (AST_Type) {.kind = AST_TYPE_BOOL};
            break;

        case TOKEN_INT:
            n->type = (AST_Type) {.kind = AST_TYPE_I64};
            break;

        case TOKEN_IDENT:
            check_ident(c, n);
            break;

        default:
            unreachable();
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        check_expr(c, unary->value, false);

        static_assert(COUNT_TOKENS == 29, "");
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
        static_assert(COUNT_TOKENS == 29, "");
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

    if (!n->is_memory && ref) {
        fprintf(stderr, Pos_Fmt "ERROR: Cannot take reference to value not in memory\n", Pos_Arg(n->token.pos));
        exit(1);
    }
}

static_assert(COUNT_AST_NODES == 9, "");
static AST_Const_Value eval_const_expr(Compiler *c, AST_Node *n) {
    if (!n) {
        return (AST_Const_Value) {0};
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        AST_Node_Atom *atom = (AST_Node_Atom *) n;

        static_assert(COUNT_TOKENS == 29, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
        case TOKEN_INT:
            return const_value_int(n->token.as.integer);

        case TOKEN_IDENT: {
            check_ident(c, n);
            if (n->type.kind == AST_TYPE_TYPE) {
                return const_value_type(n->type);
            }

            AST_Node_Atom *definition = atom->as.reference.definition;
            assert(definition);

            if (!definition->as.definition.is_const) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot use runtime values in a constant expression\n",
                    Pos_Arg(n->token.pos));
                exit(1);
            }

            return definition->as.definition.const_value;
        }

        default:
            unreachable();
            break;
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        AST_Const_Value value = {0};

        static_assert(COUNT_TOKENS == 29, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            value = eval_const_expr(c, unary->value);
            return const_value_int(-value.as.integer);

        case TOKEN_LNOT:
            value = eval_const_expr(c, unary->value);
            return const_value_int(!value.as.integer);

        default:
            unreachable();
        }
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        AST_Const_Value  lhs = {0};
        AST_Const_Value  rhs = {0};

        static_assert(COUNT_TOKENS == 29, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer + rhs.as.integer);

        case TOKEN_SUB:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer - rhs.as.integer);

        case TOKEN_MUL:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer * rhs.as.integer);

        case TOKEN_DIV:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer / rhs.as.integer);

        case TOKEN_MOD:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer % rhs.as.integer);

        case TOKEN_GT:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer > rhs.as.integer);

        case TOKEN_GE:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer >= rhs.as.integer);

        case TOKEN_LT:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer < rhs.as.integer);

        case TOKEN_LE:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer <= rhs.as.integer);

        case TOKEN_EQ:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer == rhs.as.integer);

        case TOKEN_NE:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer != rhs.as.integer);

        default:
            unreachable();
            break;
        }
    } break;

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_AST_NODES == 9, "");
static void check_stmt(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_DECL: {
        AST_Node_Decl *decl = (AST_Node_Decl *) n;
        assert(decl->name->kind == AST_NODE_ATOM && decl->name->token.kind == TOKEN_IDENT);

        AST_Node_Atom *it = (AST_Node_Atom *) decl->name;
        AST_Node      *it_expr = decl->expr;

        for (AST_Type_Kind kind = 0; kind < COUNT_AST_TYPES; kind++) {
            const char *name = builtin_type_names[kind];
            if (name && sv_match(it->node.token.sv, name)) {
                error_redefinition(it, NULL);
            }
        }

        AST_Node_Atom *previous = scope_find(&c->globals, it->node.token.sv);
        if (previous) {
            error_redefinition(it, previous);
        }

        if (decl->type) {
            check_expr(c, decl->type, false);
            ast_type_assert(decl->type, (AST_Type) {.kind = AST_TYPE_TYPE});
            it->node.type = *decl->type->type.spec.type;
        }

        if (it_expr) {
            check_expr(c, it_expr, false);

            if (it_expr->type.kind == AST_TYPE_UNIT || (it_expr->type.kind == AST_TYPE_TYPE && !decl->is_const)) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot store %s in a variable\n",
                    Pos_Arg(it_expr->token.pos),
                    ast_type_to_cstr(it_expr->type));

                exit(1);
            }

            if (decl->type) {
                ast_type_assert(it_expr, it->node.type);
            } else {
                it->node.type = it_expr->type;
            }
        }

        if (decl->is_const) {
            it->as.definition.is_const = true;
            it->as.definition.const_value = eval_const_expr(c, it_expr);
        }

        scope_push(&c->globals, it);
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

    case AST_NODE_JUMP:
        // Pass
        break;

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
