#include "checker.h"
#include "ast.h"
#include "basic.h"
#include "context.h"
#include "token.h"

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

static void error_too_few_arguments(Pos pos, size_t expected) {
    fprintf(stderr, Pos_Fmt "ERROR: Too few arguments, expected %zu\n", Pos_Arg(pos), expected);
    exit(1);
}

static void error_too_many_arguments(Pos pos, size_t expected) {
    fprintf(stderr, Pos_Fmt "ERROR: Too many arguments, expected %zu\n", Pos_Arg(pos), expected);
    exit(1);
}

static void check_int_limit(AST_Node *n, size_t value) {
    static_assert(COUNT_AST_TYPES == 14, "");
    const size_t int_limits[COUNT_AST_TYPES] = {
        [AST_TYPE_I8] = INT8_MAX,
        [AST_TYPE_I16] = INT16_MAX,
        [AST_TYPE_I32] = INT32_MAX,
        [AST_TYPE_I64] = INT64_MAX,

        [AST_TYPE_U8] = UINT8_MAX,
        [AST_TYPE_U16] = UINT16_MAX,
        [AST_TYPE_U32] = UINT32_MAX,
        [AST_TYPE_U64] = UINT64_MAX,

        [AST_TYPE_INT] = INT64_MAX,
    };

    if (value > int_limits[n->type.kind]) {
        fprintf(
            stderr,
            Pos_Fmt "ERROR: Number '%zu' is too large for %s\n",
            Pos_Arg(n->token.pos),
            value,
            ast_type_to_cstr(n->type));
        exit(1);
    }
}

static_assert(COUNT_AST_NODES == 13, "");
static void cast_untyped(Compiler *c, AST_Node *n, AST_Type expected) {
    switch (n->kind) {
    case AST_NODE_ATOM:
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = expected;
            check_int_limit(n, n->token.as.integer);
            break;

        case TOKEN_IDENT: {
            AST_Node_Atom *atom = (AST_Node_Atom *) n;
            assert(atom->definition->is_const); // Only constants can be defined as untyped int

            n->type = expected;
            check_int_limit(n, atom->definition->const_value.as.integer);
        } break;

        default:
            unreachable();
        }
        break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        n->type = expected;
        if (n->token.kind == TOKEN_SIZEOF) {
            check_int_limit(n, compile_sizeof(c, &unary->value->type));
        } else {
            cast_untyped(c, unary->value, expected);
        }
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        cast_untyped(c, binary->lhs, expected);
        cast_untyped(c, binary->rhs, expected);
        n->type = expected;
    } break;

    case AST_NODE_RETURN: {
        AST_Node_Return *ret = (AST_Node_Return *) n;
        cast_untyped(c, ret->value, expected);
        n->type = ret->value->type;
    } break;

    default:
        unreachable();
    }
}

static bool try_auto_cast_untyped(Compiler *c, AST_Node *n, AST_Type expected) {
    if (ast_type_is_integer(expected) && ast_type_eq(n->type, (AST_Type) {.kind = AST_TYPE_INT})) {
        if (expected.kind != AST_TYPE_INT) {
            cast_untyped(c, n, expected);
        }
        return true;
    }

    return false;
}

static AST_Type ast_type_assert(Compiler *c, AST_Node *n, AST_Type expected) {
    if (ast_type_eq(n->type, expected)) {
        return n->type;
    }

    if (try_auto_cast_untyped(c, n, expected)) {
        return expected;
    }

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected %s, got %s\n",
        Pos_Arg(n->token.pos),
        ast_type_to_cstr(expected),
        ast_type_to_cstr(n->type));

    exit(1);
}

static AST_Type ast_type_assert_node(Compiler *c, AST_Node *a, AST_Node *b) {
    if (ast_type_eq(a->type, b->type)) {
        return a->type;
    }

    if (try_auto_cast_untyped(c, b, a->type)) {
        return a->type;
    }

    if (try_auto_cast_untyped(c, a, b->type)) {
        return b->type;
    }

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected %s, got %s\n",
        Pos_Arg(a->token.pos),
        ast_type_to_cstr(b->type),
        ast_type_to_cstr(a->type));

    exit(1);
}

static AST_Type ast_type_assert_numeric(const AST_Node *n, bool pointers_allowed) {
    if (ast_type_is_numeric(n->type)) {
        return n->type;
    }

    if (ast_type_is_pointer(n->type) && pointers_allowed) {
        return n->type;
    }

    const char *label = "arithmetic";
    if (!pointers_allowed) {
        label = "numeric";
    }

    fprintf(
        stderr, Pos_Fmt "ERROR: Expected %s value, got %s\n", Pos_Arg(n->token.pos), label, ast_type_to_cstr(n->type));
    exit(1);
}

static AST_Type ast_type_assert_scalar(const AST_Node *n) {
    if (ast_type_is_scalar(n->type)) {
        return n->type;
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected scalar value, got %s\n", Pos_Arg(n->token.pos), ast_type_to_cstr(n->type));
    exit(1);
}

static bool get_builtin_type_kind(SV name, AST_Type_Kind *kind) {
    static_assert(COUNT_AST_TYPES == 14, "");
    static const char *names[COUNT_AST_TYPES] = {
        [AST_TYPE_BOOL] = "bool",

        [AST_TYPE_I8] = "i8",
        [AST_TYPE_I16] = "i16",
        [AST_TYPE_I32] = "i32",
        [AST_TYPE_I64] = "i64",

        [AST_TYPE_U8] = "u8",
        [AST_TYPE_U16] = "u16",
        [AST_TYPE_U32] = "u32",
        [AST_TYPE_U64] = "u64",

        [AST_TYPE_RAWPTR] = "rawptr",
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

static void ast_node_finalize_type_of_untyped(AST_Node *n) {
    if (n->type.kind == AST_TYPE_INT) {
        n->type.kind = AST_TYPE_I64;
    }
}

static void ast_node_assert_can_be_referenced(AST_Node *n) {
    if (!n->is_memory) {
        fprintf(stderr, Pos_Fmt "ERROR: Cannot take reference to value not in memory\n", Pos_Arg(n->token.pos));
        exit(1);
    }
}

static void check_node(Compiler *c, AST_Node *n);

static void check_ident(Compiler *c, AST_Node *n) {
    AST_Node_Atom *atom = (AST_Node_Atom *) n;
    if (sv_match(n->token.sv, "_")) {
        fprintf(stderr, Pos_Fmt "ERROR: Identifier '_' cannot be used as a value\n", Pos_Arg(n->token.pos));
        exit(1);
    }

    AST_Node_Atom *definition = context_find_local(&c->context, n->token.sv);
    if (!definition) {
        definition = scope_find(c->globals, n->token.sv);
    }
    atom->definition = definition;

    if (definition) {
        switch (definition->inference_status) {
        case UNINFERRED: {
            Context_Fn *context_fn_save = c->context.current;

            // TODO: This should be the context it was defined it
            // For now, only globals can be forward referenced, but later local constants might be considered
            c->context.current = NULL;

            check_node(c, (AST_Node *) definition->definition_stmt);
            context_restore_fn(&c->context, context_fn_save);
        } break;

        case INFERRING:
            fprintf(stderr, Pos_Fmt "ERROR: Cyclic definition\n", Pos_Arg(definition->node.token.pos));
            exit(1);
            break;

        case INFERRED:
            // Pass
            break;
        }

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

static_assert(COUNT_AST_NODES == 13, "");
static bool loop_breaks(AST_Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            if (loop_breaks(it)) {
                return true;
            }
        }
        return false;
    }

    case AST_NODE_IF: {
        AST_Node_If *iff = (AST_Node_If *) n;
        return loop_breaks(iff->consequence) || loop_breaks(iff->antecedence);
    }

    case AST_NODE_JUMP:
        return n->token.kind == TOKEN_BREAK;

    default:
        return false;
    }
}

static bool is_atom_true(AST_Node *n) {
    return n->kind == AST_NODE_ATOM && n->token.kind == TOKEN_BOOL && n->token.as.integer;
}

static bool is_atom_false(AST_Node *n) {
    return n->kind == AST_NODE_ATOM && n->token.kind == TOKEN_BOOL && !n->token.as.integer;
}

static_assert(COUNT_AST_NODES == 13, "");
static bool always_returns(AST_Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            if (always_returns(it)) {
                return true;
            }
        }
        return false;
    }

    case AST_NODE_IF: {
        AST_Node_If *iff = (AST_Node_If *) n;
        if (is_atom_true(iff->condition)) {
            return always_returns(iff->consequence);
        }

        if (is_atom_false(iff->condition)) {
            return always_returns(iff->antecedence);
        }

        if (!iff->antecedence) {
            return false;
        }
        return always_returns(iff->consequence) && always_returns(iff->antecedence);
    }

    case AST_NODE_FOR: {
        AST_Node_For *forr = (AST_Node_For *) n;
        if (forr->init && always_returns(forr->init)) {
            return true;
        }

        bool infinite = false;
        if (!forr->condition) {
            infinite = true;
        } else if (is_atom_true(forr->condition)) {
            infinite = true;
        }

        if (infinite) {
            return !loop_breaks(forr->body);
        }

        return false;
    }

    case AST_NODE_RETURN:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_AST_NODES == 13, "");
static Const_Value eval_const_expr(Compiler *c, AST_Node *n) {
    if (!n) {
        return (Const_Value) {0};
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        AST_Node_Atom *atom = (AST_Node_Atom *) n;

        static_assert(COUNT_TOKENS == 39, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
        case TOKEN_INT:
            return const_value_int(n->token.as.integer);

        case TOKEN_IDENT: {
            check_ident(c, n);
            if (n->type.kind == AST_TYPE_TYPE) {
                return const_value_type(n->type);
            }

            assert(atom->definition);
            if (!atom->definition->is_const) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot use variables in a constant expression\n", Pos_Arg(n->token.pos));
                exit(1);
            }

            return atom->definition->const_value;
        }

        default:
            unreachable();
            break;
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        Const_Value     value = {0};

        static_assert(COUNT_TOKENS == 39, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            value = eval_const_expr(c, unary->value);
            return const_value_int(-value.as.integer);

        case TOKEN_MUL:
            fprintf(stderr, Pos_Fmt "ERROR: Cannot dereference in a constant expression\n", Pos_Arg(n->token.pos));
            exit(1);
            break;

        case TOKEN_BAND:
            value = eval_const_expr(c, unary->value);
            if (value.kind == CONST_VALUE_TYPE) {
                value.as.type.ref++;
                return value;
            }

            fprintf(stderr, Pos_Fmt "ERROR: Cannot take reference in a constant expression\n", Pos_Arg(n->token.pos));
            exit(1);
            break;

        case TOKEN_BNOT:
            value = eval_const_expr(c, unary->value);
            return const_value_int(~value.as.integer);

        case TOKEN_LNOT:
            value = eval_const_expr(c, unary->value);
            return const_value_int(!value.as.integer);

        case TOKEN_SIZEOF:
            return const_value_int(compile_sizeof(c, &unary->value->type));

        default:
            unreachable();
        }
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        Const_Value      lhs = {0};
        Const_Value      rhs = {0};

        static_assert(COUNT_TOKENS == 39, "");
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

        case TOKEN_SHL:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer << rhs.as.integer);

        case TOKEN_SHR:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer >> rhs.as.integer);

        case TOKEN_BOR:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer | rhs.as.integer);

        case TOKEN_BAND:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(lhs.as.integer & rhs.as.integer);

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

    case AST_NODE_FN: {
        AST_Node_Fn *fn = (AST_Node_Fn *) n;
        if (fn->is_type) {
            return const_value_type(fn->node.type);
        } else {
            return const_value_fn(fn);
        }
    }

    case AST_NODE_CALL: {
        AST_Node_Call *call = (AST_Node_Call *) n;
        if (!call->is_type_cast) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot call functions in a constant expression\n",
                Pos_Arg(call->fn->token.pos));
            exit(1);
        }

        const Const_Value value = eval_const_expr(c, call->args.head);
        static_assert(COUNT_TYPE_CASTS == 3, "");
        switch (call->type_cast) {
        case TYPE_CAST_NOP:
            return value;

        case TYPE_CAST_NORMAL:
            return value;

        case TYPE_CAST_TO_BOOL:
            return const_value_int(value.as.integer != 0);

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_AST_NODES == 13, "");
static void check_node(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        static_assert(COUNT_TOKENS == 39, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
            n->type = (AST_Type) {.kind = AST_TYPE_BOOL};
            break;

        case TOKEN_INT:
            n->type = (AST_Type) {.kind = AST_TYPE_INT};
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
        check_node(c, unary->value);

        static_assert(COUNT_TOKENS == 39, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            n->type = ast_type_assert_numeric(unary->value, false);
            break;

        case TOKEN_MUL:
            if (!unary->value->type.ref) {
                if (unary->value->type.kind == AST_TYPE_RAWPTR) {
                    fprintf(
                        stderr, Pos_Fmt "ERROR: Cannot dereference raw pointer\n", Pos_Arg(unary->value->token.pos));
                    exit(1);
                }

                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Expected typed pointer, got %s\n",
                    Pos_Arg(unary->value->token.pos),
                    ast_type_to_cstr(unary->value->type));
                exit(1);
            }

            n->type = unary->value->type;
            n->type.ref--;
            n->is_memory = true;
            break;

        case TOKEN_BAND:
            n->type = unary->value->type;

            if (n->type.kind == AST_TYPE_TYPE) {
                n->type.spec.type->ref++;
            } else {
                ast_node_assert_can_be_referenced(unary->value);
                n->type.ref++;
            }
            break;

        case TOKEN_BNOT:
            n->type = ast_type_assert_numeric(unary->value, false);
            break;

        case TOKEN_LNOT:
            n->type = ast_type_assert(c, unary->value, (AST_Type) {.kind = AST_TYPE_BOOL});
            break;

        case TOKEN_SIZEOF:
            n->type = (AST_Type) {.kind = AST_TYPE_INT};
            break;

        default:
            unreachable();
        }
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        check_node(c, binary->lhs);
        check_node(c, binary->rhs);

        static_assert(COUNT_TOKENS == 39, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
            ast_type_assert_numeric(binary->lhs, true);
            n->type = ast_type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
            ast_type_assert_numeric(binary->lhs, false);
            n->type = ast_type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
            ast_type_assert_numeric(binary->lhs, false);
            n->type = ast_type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            ast_type_assert_numeric(binary->lhs, true);
            ast_type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (AST_Type) {.kind = AST_TYPE_BOOL};
            break;

        case TOKEN_SET:
            ast_node_assert_can_be_referenced(binary->lhs);
            ast_type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (AST_Type) {.kind = AST_TYPE_UNIT};
            break;

        default:
            unreachable();
        }
    } break;

    case AST_NODE_FN: {
        AST_Node_Fn *fn = (AST_Node_Fn *) n;

        Context_Fn context_fn = {.fn = fn, .outer = c->context.current};
        context_push_fn(&c->context, &context_fn);

        {
            if (fn->signature_checked) {
                for (AST_Node *arg = fn->args.head; arg; arg = arg->next) {
                    assert(arg->kind == AST_NODE_DEFINE);
                    AST_Node_Define *define = (AST_Node_Define *) arg;

                    assert(define->name->kind == AST_NODE_ATOM);
                    context_push_local(&c->context, (AST_Node_Atom *) define->name);
                }
            } else {
                AST_Type_Fn fn_type = {
                    .args = arena_alloc(c->llvm.arena, fn->arity * sizeof(*fn_type.args)),
                };

                for (AST_Node *arg = fn->args.head; arg; arg = arg->next) {
                    check_node(c, arg);

                    assert(arg->kind == AST_NODE_DEFINE);
                    AST_Node_Define *define = (AST_Node_Define *) arg;

                    assert(define->name->kind == AST_NODE_ATOM);
                    AST_Node_Atom *it = (AST_Node_Atom *) define->name;

                    fn_type.args[fn_type.arity++] = it->node.type;
                }

                if (fn->returnn) {
                    check_node(c, fn->returnn);
                    ast_type_assert(c, fn->returnn, (AST_Type) {.kind = AST_TYPE_TYPE});
                    fn_type.returnn = fn->returnn->type.spec.type;
                } else {
                    fn_type.returnn = arena_alloc(c->llvm.arena, sizeof(AST_Type));
                    fn_type.returnn->kind = AST_TYPE_UNIT;
                }

                n->type = (AST_Type) {.kind = AST_TYPE_FN, .spec.fn = fn_type};

                fn->signature_checked = true;
                if (fn->defined_as) {
                    // The body of a function is irrelevant for outer expressions
                    fn->defined_as->node.type = n->type;
                    fn->defined_as->inference_status = INFERRED;
                }
            }

            if (fn->is_type) {
                const AST_Type meta = {
                    .kind = AST_TYPE_TYPE,
                    .spec.type = arena_clone(c->llvm.arena, &n->type, sizeof(n->type)),
                };
                n->type = meta;
            } else if (fn->body) {
                check_node(c, fn->body);
                if (fn->returnn && !always_returns(fn->body)) {
                    assert(fn->body->kind == AST_NODE_BLOCK);
                    const Pos end = ((AST_Node_Block *) fn->body)->end;
                    fprintf(stderr, Pos_Fmt "ERROR: Expected return statement\n", Pos_Arg(end));
                    exit(1);
                }
            }
        }

        context_pop_fn(&c->context);
    } break;

    case AST_NODE_CALL: {
        AST_Node_Call *call = (AST_Node_Call *) n;
        check_node(c, call->fn);

        const AST_Type fn_type = call->fn->type;
        if (fn_type.kind == AST_TYPE_TYPE) {
            call->is_type_cast = true;
            if (!call->args.head) {
                error_too_few_arguments(call->end, 1);
                exit(1);
            } else if (call->args.head->next) {
                error_too_many_arguments(call->args.head->next->token.pos, 1);
                exit(1);
            }
            n->type = *fn_type.spec.type;

            check_node(c, call->args.head);
            const AST_Type from_type = call->args.head->type;

            if (ast_type_is_scalar(n->type)) {
                ast_type_assert_scalar(call->args.head);

                bool ok = true;
                if (from_type.kind == AST_TYPE_FN && !from_type.ref) {
                    // fn -> rawptr
                    ok = ast_type_eq(n->type, (AST_Type) {.kind = AST_TYPE_RAWPTR});
                } else if (n->type.kind == AST_TYPE_FN && !n->type.ref) {
                    // rawptr -> fn
                    ok = ast_type_eq(from_type, (AST_Type) {.kind = AST_TYPE_RAWPTR});
                } else if (!ast_type_is_pointer(from_type) && ast_type_is_pointer(n->type)) {
                    // i64/u64 -> ptr
                    if (from_type.kind != AST_TYPE_I64 && from_type.kind != AST_TYPE_U64 &&
                        from_type.kind != AST_TYPE_INT) {
                        ok = false;
                    }
                } else if (ast_type_is_pointer(from_type) && !ast_type_is_pointer(n->type)) {
                    // ptr -> i64/u64
                    if (n->type.kind != AST_TYPE_I64 && n->type.kind != AST_TYPE_U64 && n->type.kind != AST_TYPE_INT) {
                        ok = false;
                    }
                }

                if (!ok) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot cast %s to %s\n",
                        Pos_Arg(call->fn->token.pos),
                        ast_type_to_cstr(from_type),
                        ast_type_to_cstr(n->type));
                    exit(1);
                }
            } else {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot cast to %s\n",
                    Pos_Arg(call->fn->token.pos),
                    ast_type_to_cstr(n->type));
                exit(1);
            }

            if (ast_type_eq(n->type, from_type)) {
                call->type_cast = TYPE_CAST_NOP;
            } else if (ast_type_eq(n->type, (AST_Type) {.kind = AST_TYPE_BOOL})) {
                call->type_cast = TYPE_CAST_TO_BOOL;
            } else {
                call->type_cast = TYPE_CAST_NORMAL;
            }
        } else {
            if (fn_type.kind != AST_TYPE_FN) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot call %s\n", Pos_Arg(call->fn->token.pos), ast_type_to_cstr(fn_type));
                exit(1);
            }

            if (fn_type.ref) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot call %s without deferencing it first\n",
                    Pos_Arg(call->fn->token.pos),
                    ast_type_to_cstr(fn_type));
                exit(1);
            }

            call->arity = 0;
            for (AST_Node *arg = call->args.head; arg; arg = arg->next) {
                check_node(c, arg);
                if (call->arity >= fn_type.spec.fn.arity) {
                    error_too_many_arguments(arg->token.pos, fn_type.spec.fn.arity);
                }
                ast_type_assert(c, arg, fn_type.spec.fn.args[call->arity++]);
            }

            if (call->arity < fn_type.spec.fn.arity) {
                error_too_few_arguments(call->end, fn_type.spec.fn.arity);
            }

            n->type = *fn_type.spec.fn.returnn;
        }
    } break;

    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        assert(define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT);

        AST_Node_Atom *it = (AST_Node_Atom *) define->name;
        AST_Node      *it_expr = define->expr;
        const bool     it_is_underscore = sv_match(it->node.token.sv, "_");

        assert(it->inference_status != INFERRING); // It is already checked
        if (it->inference_status == UNINFERRED) {
            it->inference_status = INFERRING;

            if (define->is_arg) {
                if (!it_is_underscore) {
                    for (size_t i = c->context.current->begin; i < c->context.current->end; i++) {
                        AST_Node_Atom *previous = c->context.locals.data[i];
                        if (sv_eq(previous->node.token.sv, it->node.token.sv)) {
                            error_redefinition(it, previous);
                        }
                    }
                }
            }

            if (define->type) {
                check_node(c, define->type);
                ast_type_assert(c, define->type, (AST_Type) {.kind = AST_TYPE_TYPE});
                it->node.type = *define->type->type.spec.type;
            }

            if (it_expr) {
                it->is_assigned = true;
                check_node(c, it_expr);

                if (it_expr->type.kind == AST_TYPE_UNIT || (it_expr->type.kind == AST_TYPE_TYPE && !define->is_const)) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot store %s in a %s\n",
                        Pos_Arg(it_expr->token.pos),
                        ast_type_to_cstr(it_expr->type),
                        define->is_const ? "constant" : "variable");

                    exit(1);
                }

                if (define->type) {
                    ast_type_assert(c, it_expr, it->node.type);
                } else {
                    if (!define->is_const) {
                        ast_node_finalize_type_of_untyped(it_expr);
                    }
                    it->node.type = it_expr->type;
                }
            }

            if (define->is_const) {
                if (it_expr->kind == AST_NODE_FN) {
                    ((AST_Node_Fn *) it_expr)->defined_as = it;
                }

                it->is_const = true;
                it->const_value = eval_const_expr(c, it_expr);
            } else if (!define->is_local && it_expr) {
                it->const_value = eval_const_expr(c, it_expr);
            }

            it->is_extern = define->is_extern;
            if (define->is_local) {
                it->is_local = true;
                if (!it_is_underscore) {
                    context_push_local(&c->context, it);
                }
            }

            it->inference_status = INFERRED;
        }
    } break;

    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;

        const size_t context_end_save = c->context.current->end;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            check_node(c, it);
        }
        context_set_end(&c->context, context_end_save);
    } break;

    case AST_NODE_IF: {
        AST_Node_If *iff = (AST_Node_If *) n;
        check_node(c, iff->condition);
        ast_type_assert(c, iff->condition, (AST_Type) {.kind = AST_TYPE_BOOL});
        check_node(c, iff->consequence);
        check_node(c, iff->antecedence);
    } break;

    case AST_NODE_FOR: {
        AST_Node_For *forr = (AST_Node_For *) n;

        const size_t context_end_save = c->context.current->end;
        {
            check_node(c, forr->init);
            check_node(c, forr->condition);
            if (forr->condition) {
                ast_type_assert(c, forr->condition, (AST_Type) {.kind = AST_TYPE_BOOL});
            }
            check_node(c, forr->update);
            check_node(c, forr->body);
        }
        context_set_end(&c->context, context_end_save);
    } break;

    case AST_NODE_JUMP:
        // Pass
        break;

    case AST_NODE_RETURN: {
        AST_Node_Return *returnn = (AST_Node_Return *) n;
        const AST_Type   expected = *c->context.current->fn->node.type.spec.fn.returnn;

        n->type.kind = AST_TYPE_UNIT;
        if (returnn->value) {
            check_node(c, returnn->value);
            ast_type_assert(c, returnn->value, expected);
            n->type = returnn->value->type;
        } else {
            ast_type_assert(c, n, expected);
        }
    } break;

    case AST_NODE_EXTERN: {
        AST_Node_Extern *externn = (AST_Node_Extern *) n;
        for (AST_Node *it = externn->nodes.head; it; it = it->next) {
            check_node(c, it);
        }
    } break;

    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        check_node(c, print->value);
        ast_type_assert_scalar(print->value);
    } break;

    default:
        check_node(c, n);
        break;
    }
}

static_assert(COUNT_AST_NODES == 13, "");
static void define_toplevel(Compiler *c, AST_Node *n) {
    switch (n->kind) {
    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        assert(define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT);

        AST_Node_Atom *it = (AST_Node_Atom *) define->name;
        AST_Node      *it_expr = define->expr;

        if (sv_match(it->node.token.sv, "_")) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Identifier '_' cannot be defined as a global variable\n",
                Pos_Arg(it->node.token.pos));

            exit(1);
        }

        if (get_builtin_type_kind(it->node.token.sv, NULL)) {
            error_redefinition(it, NULL);
        }

        AST_Node_Atom *previous = scope_find(c->globals, it->node.token.sv);
        if (previous) {
            error_redefinition(it, previous);
        }

        if (define->is_const && it_expr->kind == AST_NODE_FN) {
            ((AST_Node_Fn *) it_expr)->defined_as = it;
        }

        da_push(&c->globals, it);
    } break;

    case AST_NODE_EXTERN: {
        AST_Node_Extern *externn = (AST_Node_Extern *) n;
        for (AST_Node *it = externn->nodes.head; it; it = it->next) {
            define_toplevel(c, it);
        }
    } break;

    default:
        // Pass
        break;
    }
}

void check_nodes(Compiler *c, AST_Nodes nodes) {
    for (AST_Node *it = nodes.head; it; it = it->next) {
        define_toplevel(c, it);
    }

    for (AST_Node *it = nodes.head; it; it = it->next) {
        check_node(c, it);
    }
}
