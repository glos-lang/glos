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

static bool get_builtin_type_kind(SV name, AST_Type_Kind *kind) {
    static_assert(COUNT_AST_TYPES == 5, "");
    static const char *names[COUNT_AST_TYPES] = {
        [AST_TYPE_BOOL] = "bool",
        [AST_TYPE_I64] = "i64",
    };

    for (AST_Type_Kind k = 0; k < len(names); k++) {
        const char *it = names[k];
        if (it && sv_match(name, it)) {
            if (kind) {
                *kind = k;
            }
            return true;
        }
    }

    return false;
}

static void check_ident(Compiler *c, AST_Node *n) {
    AST_Node_Atom *atom = (AST_Node_Atom *) n;
    AST_Node_Atom *definition = scope_find(c->locals, n->token.sv, c->fn_base);
    if (!definition) {
        definition = scope_find(c->globals, n->token.sv, 0);
    }
    atom->definition = definition;

    if (definition) {
        n->type = definition->node.type;
        n->is_memory = !definition->is_const;
        return;
    }

    AST_Type_Kind kind;
    if (get_builtin_type_kind(n->token.sv, &kind)) {
        const AST_Type type = {.kind = kind};
        n->type = (AST_Type) {
            .kind = AST_TYPE_TYPE,
            .spec.type = arena_clone(c->llvm.arena, &type, sizeof(type)),
        };
        return;
    }

    error_undefined(n);
}

static void check_stmt(Compiler *c, AST_Node *n);

static_assert(COUNT_AST_NODES == 11, "");
static void check_expr(Compiler *c, AST_Node *n, bool ref) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        static_assert(COUNT_TOKENS == 30, "");
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

        static_assert(COUNT_TOKENS == 30, "");
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
        static_assert(COUNT_TOKENS == 30, "");
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

    case AST_NODE_FN: {
        AST_Node_Fn *fn = (AST_Node_Fn *) n;

        const size_t fn_base_save = c->fn_base;
        c->fn_base = c->locals.count;
        {
            // TODO: Just directly allocate
            {
                const void *temp_save = temp_alloc(0);
                AST_Type_Fn type = {.args = temp_alloc(0)};

                for (AST_Node *arg = fn->args.head; arg; arg = arg->next) {
                    check_stmt(c, arg); // TODO: Check for argument redefinition

                    assert(arg->kind == AST_NODE_DEFINE);
                    AST_Node_Define *define = (AST_Node_Define *) arg;

                    assert(define->name->kind == AST_NODE_ATOM);
                    AST_Node_Atom *it = (AST_Node_Atom *) define->name;

                    temp_alloc(sizeof(*type.args)); // Temporary memory is guaranteed to be contiguous with no alignment
                    type.args[type.arity++] = it->node.type;
                }

                type.args = arena_clone(c->llvm.arena, type.args, type.arity * sizeof(*type.args));
                temp_reset(temp_save);

                n->type = (AST_Type) {.kind = AST_TYPE_FN, .spec.fn = type};
            }

            check_stmt(c, fn->body);
        }
        c->locals.count = c->fn_base;
        c->fn_base = fn_base_save;
    } break;

    case AST_NODE_CALL: {
        AST_Node_Call *call = (AST_Node_Call *) n;
        check_expr(c, call->fn, false);

        const AST_Type fn_type = call->fn->type;
        if (fn_type.kind != AST_TYPE_FN) {
            fprintf(stderr, Pos_Fmt "ERROR: Cannot call %s\n", Pos_Arg(call->fn->token.pos), ast_type_to_cstr(fn_type));
            exit(1);
        }

        call->arity = 0;
        for (AST_Node *arg = call->args.head; arg; arg = arg->next) {
            check_expr(c, arg, false);
            if (call->arity >= fn_type.spec.fn.arity) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Too many arguments, expected %zu\n",
                    Pos_Arg(arg->token.pos),
                    fn_type.spec.fn.arity);
                exit(1);
            }
            ast_type_assert(arg, fn_type.spec.fn.args[call->arity++]);
        }

        if (call->arity < fn_type.spec.fn.arity) {
            fprintf(
                stderr, Pos_Fmt "ERROR: Too few arguments, expected %zu\n", Pos_Arg(call->end), fn_type.spec.fn.arity);
            exit(1);
        }

        n->type = (AST_Type) {.kind = AST_TYPE_UNIT};
    } break;

    default:
        unreachable();
    }

    if (!n->is_memory && ref) {
        fprintf(stderr, Pos_Fmt "ERROR: Cannot take reference to value not in memory\n", Pos_Arg(n->token.pos));
        exit(1);
    }
}

static_assert(COUNT_AST_NODES == 11, "");
static Const_Value eval_const_expr(Compiler *c, AST_Node *n) {
    if (!n) {
        return (Const_Value) {0};
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        AST_Node_Atom *atom = (AST_Node_Atom *) n;

        static_assert(COUNT_TOKENS == 30, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
        case TOKEN_INT:
            return const_value_int(n->token.as.integer);

        case TOKEN_IDENT: {
            check_ident(c, n);
            if (n->type.kind == AST_TYPE_TYPE) {
                return const_value_type(n->type);
            }

            AST_Node_Atom *definition = atom->definition;
            assert(definition);

            if (!definition->is_const) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot use runtime values in a constant expression\n",
                    Pos_Arg(n->token.pos));
                exit(1);
            }

            return definition->const_value;
        }

        default:
            unreachable();
            break;
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        Const_Value     value = {0};

        static_assert(COUNT_TOKENS == 30, "");
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
        Const_Value      lhs = {0};
        Const_Value      rhs = {0};

        static_assert(COUNT_TOKENS == 30, "");
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

    case AST_NODE_FN:
        return const_value_fn((AST_Node_Fn *) n);

    case AST_NODE_CALL: {
        todo();
    } break;

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_AST_NODES == 11, "");
static void check_stmt(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        assert(define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT);

        AST_Node_Atom *it = (AST_Node_Atom *) define->name;
        AST_Node      *it_expr = define->expr;

        if (!define->is_local) {
            if (get_builtin_type_kind(it->node.token.sv, NULL)) {
                error_redefinition(it, NULL);
            }

            AST_Node_Atom *previous = scope_find(c->globals, it->node.token.sv, 0);
            if (previous) {
                error_redefinition(it, previous);
            }
        }

        if (define->type) {
            check_expr(c, define->type, false);
            ast_type_assert(define->type, (AST_Type) {.kind = AST_TYPE_TYPE});
            it->node.type = *define->type->type.spec.type;
        }

        if (it_expr) {
            it->is_assigned = true;
            check_expr(c, it_expr, false);

            if (it_expr->type.kind == AST_TYPE_UNIT || (it_expr->type.kind == AST_TYPE_TYPE && !define->is_const)) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot store %s in a variable\n",
                    Pos_Arg(it_expr->token.pos),
                    ast_type_to_cstr(it_expr->type));

                exit(1);
            }

            if (define->type) {
                ast_type_assert(it_expr, it->node.type);
            } else {
                it->node.type = it_expr->type;
            }
        }

        if (define->is_const) {
            it->is_const = true;
            it->const_value = eval_const_expr(c, it_expr);

            if (it_expr->kind == AST_NODE_FN) {
                ((AST_Node_Fn *) it_expr)->defined_as = it;
            }
        } else if (!define->is_local && it_expr) {
            it->const_value = eval_const_expr(c, it_expr);
        }

        if (define->is_local) {
            it->is_local = true;
            scope_push(&c->locals, it);
        } else {
            scope_push(&c->globals, it);
        }
    } break;

    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;

        const size_t locals_count_save = c->locals.count;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }
        c->locals.count = locals_count_save;
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

        const size_t locals_count_save = c->locals.count;
        {
            check_stmt(c, forr->init);
            check_expr(c, forr->condition, false);
            if (forr->condition) {
                ast_type_assert(forr->condition, (AST_Type) {.kind = AST_TYPE_BOOL});
            }
            check_stmt(c, forr->update);
            check_stmt(c, forr->body);
        }
        c->locals.count = locals_count_save;
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
