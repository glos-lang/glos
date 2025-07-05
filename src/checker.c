#include <stdint.h>

#include "checker.h"

static_assert(COUNT_NODES == 13, "");
static void cast_untyped_int(Node *n, Type expected) {
    switch (n->kind) {
    case NODE_ATOM:
        switch (n->token.kind) {
        case TOKEN_INT: {
            n->type = expected;

            static_assert(COUNT_TYPES == 13, "");
            const size_t int_limits[COUNT_TYPES] = {
                [TYPE_I8] = INT8_MAX,
                [TYPE_I16] = INT16_MAX,
                [TYPE_I32] = INT32_MAX,
                [TYPE_I64] = INT64_MAX,

                [TYPE_U8] = UINT8_MAX,
                [TYPE_U16] = UINT16_MAX,
                [TYPE_U32] = UINT32_MAX,
                [TYPE_U64] = UINT64_MAX,

                [TYPE_INT] = INT64_MAX,
            };

            if (n->token.as.integer > int_limits[n->type.kind]) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Integer literal '" SVFmt "' is too large for type '%s'\n",
                    PosArg(n->token.pos),
                    SVArg(n->token.sv),
                    type_to_cstr(n->type));

                exit(1);
            }
        } break;

        default:
            unreachable();
        }
        break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        cast_untyped_int(unary->operand, expected);
        n->type = expected;
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;
        cast_untyped_int(binary->lhs, expected);
        cast_untyped_int(binary->rhs, expected);
        n->type = expected;
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        cast_untyped_int(ret->value, expected);
        n->type = ret->value->type;
    } break;

    default:
        unreachable();
    }
}

static bool try_auto_cast_untyped_int(Node *n, Type expected) {
    // If the types are already equal, consider it a succesful auto cast
    if (type_eq(n->type, expected)) {
        return true;
    }

    // Untyped integer -> Typed integer
    if (type_is_integer(type_remove_ref(expected)) && n->type.kind == TYPE_INT) {
        // The indirection level of the typed and untyped integers must match
        if (expected.ref != n->type.ref) {
            return false;
        }

        if (expected.kind != TYPE_INT) {
            cast_untyped_int(n, expected);
        }

        return true;
    }

    return false;
}

static Type type_assert(Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return n->type;
    }

    if (try_auto_cast_untyped_int(n, expected)) {
        return expected;
    }

    fprintf(
        stderr,
        PosFmt "ERROR: Expected type '%s', got '%s'\n",
        PosArg(n->token.pos),
        type_to_cstr(expected),
        type_to_cstr(n->type));

    exit(1);
}

static Type type_assert_node(Node *a, Node *b) {
    if (type_eq(a->type, b->type)) {
        return a->type;
    }

    if (try_auto_cast_untyped_int(b, a->type)) {
        return a->type;
    }

    if (try_auto_cast_untyped_int(a, b->type)) {
        return b->type;
    }

    fprintf(
        stderr,
        PosFmt "ERROR: Expected type '%s', got '%s'\n",
        PosArg(a->token.pos),
        type_to_cstr(b->type),
        type_to_cstr(a->type));

    exit(1);
}

static Type type_assert_arith(const Node *n) {
    if (type_is_integer(n->type) || type_is_pointer(n->type)) {
        return n->type;
    }

    fprintf(stderr, PosFmt "ERROR: Expected arithmetic type, got '%s'\n", PosArg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_scalar(const Node *n) {
    if (type_is_integer(n->type) || type_is_pointer(n->type)) {
        return n->type;
    }

    if (n->type.kind == TYPE_BOOL || n->type.kind == TYPE_FN) {
        return n->type;
    }

    fprintf(stderr, PosFmt "ERROR: Expected scalar type, got '%s'\n", PosArg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static bool is_type_cast_illegal(Node *from_node, Node *to_node) {
    const Type from = from_node->type;
    const Type to = to_node->type;

    // Function -> Not rawptr
    if (from.kind == TYPE_FN) {
        return !type_eq(to, (Type) {.kind = TYPE_RAWPTR});
    }

    // Not rawptr -> Function
    if (to.kind == TYPE_FN) {
        return !type_eq(from, (Type) {.kind = TYPE_RAWPTR});
    }

    // Not 64 Bit Integer -> Pointer
    if (!type_is_pointer(from) && type_is_pointer(to)) {
        return from.kind != TYPE_I64 && from.kind != TYPE_U64 && from.kind != TYPE_INT;
    }

    // Pointer -> Not 64 Bit Integer
    if (!type_is_pointer(to) && type_is_pointer(from)) {
        return to.kind != TYPE_I64 && to.kind != TYPE_U64 && to.kind != TYPE_INT;
    }

    return false;
}

static void error_undefined(const Node *n, const char *label) {
    fprintf(stderr, PosFmt "ERROR: Undefined %s '" SVFmt "'\n", PosArg(n->token.pos), label, SVArg(n->token.sv));
    exit(1);
}

static Node *ident_find(Context *c, SV name) {
    if (c->fn.fn) {
        Node *n = context_fn_find(c->fn, c->locals, name);
        if (n) {
            return n;
        }
    }

    return scope_find(c->globals, name);
}

static Node *nodes_find(Nodes ns, SV name, Node *until) {
    for (Node *it = ns.head; it && it != until; it = it->next) {
        if (sv_eq(it->token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

static_assert(COUNT_NODES == 13, "");
static_assert(COUNT_TYPES == 13, "");
static void check_type(Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM:
        if (sv_match(n->token.sv, "bool")) {
            n->type = (Type) {.kind = TYPE_BOOL};
        } else if (sv_match(n->token.sv, "i8")) {
            n->type = (Type) {.kind = TYPE_I8};
        } else if (sv_match(n->token.sv, "i16")) {
            n->type = (Type) {.kind = TYPE_I16};
        } else if (sv_match(n->token.sv, "i32")) {
            n->type = (Type) {.kind = TYPE_I32};
        } else if (sv_match(n->token.sv, "i64")) {
            n->type = (Type) {.kind = TYPE_I64};
        } else if (sv_match(n->token.sv, "u8")) {
            n->type = (Type) {.kind = TYPE_U8};
        } else if (sv_match(n->token.sv, "u16")) {
            n->type = (Type) {.kind = TYPE_U16};
        } else if (sv_match(n->token.sv, "u32")) {
            n->type = (Type) {.kind = TYPE_U32};
        } else if (sv_match(n->token.sv, "u64")) {
            n->type = (Type) {.kind = TYPE_U64};
        } else if (sv_match(n->token.sv, "rawptr")) {
            n->type = (Type) {.kind = TYPE_RAWPTR};
        } else {
            error_undefined(n, "type");
        }
        break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        check_type(unary->operand);
        n->type = unary->operand->type;
        n->type.ref++;
    } break;

    case NODE_FN: {
        NodeFn *spec = (NodeFn *) n;
        for (Node *it = spec->args.head; it; it = it->next) {
            NodeVar *arg = (NodeVar *) it;
            check_type(arg->type);
            it->type = arg->type->type;
        }

        check_type(spec->ret);
        n->type = (Type) {.kind = TYPE_FN, .spec = n};
    } break;

    default:
        unreachable();
    }
}

static void check_fn(Context *c, Node *n);

static_assert(COUNT_NODES == 13, "");
static void check_expr(Context *c, Node *n, bool ref) {
    if (!n) {
        return;
    }

    bool allow_ref = false;
    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 30, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_INT};
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_IDENT:
            atom->definition = ident_find(c, n->token.sv);
            if (!atom->definition) {
                error_undefined(n, "identifier");
            }

            allow_ref = atom->definition->kind == NODE_VAR;
            if (allow_ref && ref) {
                NodeVar *var = (NodeVar *) atom->definition;
                if (var->kind == NODE_VAR_ARG) {
                    var->kind = NODE_VAR_LOCAL;
                }
            }

            n->type = atom->definition->type;
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        check_expr(c, call->fn, false);

        const Type fn_type = call->fn->type;
        if (fn_type.kind != TYPE_FN) {
            fprintf(
                stderr, PosFmt "ERROR: Cannot call type '%s'\n", PosArg(call->fn->token.pos), type_to_cstr(fn_type));
            exit(1);
        }

        if (fn_type.ref != 0) {
            fprintf(
                stderr,
                PosFmt "ERROR: Cannot call type '%s' without dereferencing it first\n",
                PosArg(call->fn->token.pos),
                type_to_cstr(call->fn->type));

            exit(1);
        }

        const NodeFn *expected = (const NodeFn *) fn_type.spec;
        if (call->arity != expected->arity) {
            fprintf(
                stderr,
                PosFmt "ERROR: Expected %zu argument%s, got %zu\n",
                PosArg(n->token.pos),
                expected->arity,
                expected->arity == 1 ? "" : "s",
                call->arity);

            exit(1);
        }

        for (Node *a = call->args.head, *e = expected->args.head; a; a = a->next, e = e->next) {
            check_expr(c, a, false);
            type_assert_node(a, e);
        }

        n->type = node_fn_return_type(expected);
    } break;

    case NODE_CAST: {
        NodeCast *cast = (NodeCast *) n;
        check_expr(c, cast->from, false);
        check_type(cast->to);

        const Type from = type_assert_scalar(cast->from);
        const Type to = type_assert_scalar(cast->to);
        if (!type_eq(from, to) && is_type_cast_illegal(cast->from, cast->to)) {
            fprintf(
                stderr,
                PosFmt "ERROR: Cannot cast type '%s' to type '%s'\n",
                PosArg(n->token.pos),
                type_to_cstr(from),
                type_to_cstr(to));

            exit(1);
        }

        n->type = to;
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 30, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_expr(c, unary->operand, false);
            n->type = type_assert_arith(unary->operand);
            break;

        case TOKEN_MUL:
            check_expr(c, unary->operand, false);

            if (!type_is_pointer(unary->operand->type)) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Expected pointer type, got '%s'\n",
                    PosArg(unary->operand->token.pos),
                    type_to_cstr(unary->operand->type));

                exit(1);
            }

            if (type_eq(unary->operand->type, (Type) {.kind = TYPE_RAWPTR})) {
                fprintf(stderr, PosFmt "ERROR: Cannot dereference raw pointer\n", PosArg(unary->operand->token.pos));
                exit(1);
            }

            n->type = unary->operand->type;
            n->type.ref--;
            allow_ref = true;
            break;

        case TOKEN_BAND:
            check_expr(c, unary->operand, true);
            n->type = unary->operand->type;
            n->type.ref++;
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 30, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            type_assert_arith(binary->lhs);
            n->type = type_assert_node(binary->rhs, binary->lhs);
            break;

        case TOKEN_SET:
            check_expr(c, binary->lhs, true);
            check_expr(c, binary->rhs, false);
            type_assert_node(binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            type_assert_arith(binary->lhs);
            type_assert_node(binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        check_type(sizeoff->type);
        check_expr(c, sizeoff->expr, false);
        n->type = (Type) {.kind = TYPE_I64};
    } break;

    case NODE_FN:
        check_fn(c, n);
        break;

    default:
        unreachable();
    }

    if (!allow_ref && ref) {
        fprintf(stderr, PosFmt "ERROR: Cannot take reference to value not in memory\n", PosArg(n->token.pos));
        exit(1);
    }
}

static void error_redefinition(const Node *n, const Node *previous, const char *label) {
    fprintf(stderr, PosFmt "ERROR: Redefinition of %s '" SVFmt "'\n", PosArg(n->token.pos), label, SVArg(n->token.sv));
    fprintf(stderr, PosFmt "NOTE: Defined here\n", PosArg(previous->token.pos));
    exit(1);
}

static_assert(COUNT_NODES == 13, "");
static bool always_returns(Node *n) {
    switch (n->kind) {
    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            if (always_returns(it)) {
                return true;
            }
        }
        return false;
    }

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        if (!iff->antecedence) {
            return false;
        }
        return always_returns(iff->consequence) && always_returns(iff->antecedence);
    }

    case NODE_FOR: {
        NodeFor *forr = (NodeFor *) n;
        if (forr->init && always_returns(forr->init)) {
            return true;
        }

        bool infinite = false;
        if (!forr->condition) {
            infinite = true;
        } else if (
            forr->condition->kind == NODE_ATOM && forr->condition->token.kind == TOKEN_BOOL &&
            forr->condition->token.as.boolean) {
            infinite = true;
        }

        if (infinite) {
            // NOTE: Till we get break, an infinite loop "always returns"
            return true;
        }

        return false;
    }

    case NODE_RETURN:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_NODES == 13, "");
static void check_stmt(Context *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        check_expr(c, iff->condition, false);
        type_assert(iff->condition, (Type) {.kind = TYPE_BOOL});

        check_stmt(c, iff->consequence);
        check_stmt(c, iff->antecedence);
    } break;

    case NODE_FOR: {
        NodeFor *forr = (NodeFor *) n;
        check_stmt(c, forr->init);

        if (forr->condition) {
            check_expr(c, forr->condition, false);
            type_assert(forr->condition, (Type) {.kind = TYPE_BOOL});
        }

        check_expr(c, forr->update, false);
        check_stmt(c, forr->body);
    } break;

    case NODE_BLOCK: {
        const size_t locals_count_save = c->locals.count;

        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }

        c->locals.count = locals_count_save;
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;

        n->type = (Type) {.kind = TYPE_UNIT};
        if (ret->value) {
            check_expr(c, ret->value, false);
            n->type = ret->value->type;
        }

        type_assert(n, node_fn_return_type(c->fn.fn));
    } break;

    case NODE_FN:
        check_fn(c, n);
        break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (var->kind == NODE_VAR_GLOBAL) {
            const Node *previous = scope_find(c->globals, n->token.sv);
            if (previous) {
                error_redefinition(n, previous, "identifier");
            }
        }

        if (var->type) {
            check_type(var->type);
            n->type = var->type->type;
        }

        if (var->expr) {
            check_expr(c, var->expr, false);
            n->type = var->expr->type;

            if (n->type.kind == TYPE_UNIT) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Cannot define variable with type '%s'\n",
                    PosArg(n->token.pos),
                    type_to_cstr(n->type));

                exit(1);
            }

            if (var->type) {
                type_assert(var->expr, var->type->type);
                n->type = var->expr->type;
            }

            if (n->type.kind == TYPE_INT) {
                n->type.kind = TYPE_I64;
            }
        }

        if (var->kind == NODE_VAR_GLOBAL) {
            da_push(&c->globals, n);
        } else {
            da_push(&c->locals, n);
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        check_expr(c, print->operand, false);
        type_assert_scalar(print->operand);
    } break;

    default:
        check_expr(c, n, false);
        break;
    }
}

static void check_fn(Context *c, Node *n) {
    NodeFn *fn = (NodeFn *) n;
    if (fn->local) {
        da_push(&c->locals, n);
    } else {
        const Node *previous = scope_find(c->globals, n->token.sv);
        if (previous) {
            error_redefinition(n, previous, "identifier");
        }
        da_push(&c->globals, n);
    }
    n->type = (Type) {.kind = TYPE_FN, .spec = n};

    const ContextFn context_fn_save = context_fn_begin(c, fn);
    for (Node *it = fn->args.head; it; it = it->next) {
        if (it->token.kind == TOKEN_IDENT) {
            const Node *previous = nodes_find(fn->args, it->token.sv, it);
            if (previous) {
                error_redefinition(it, previous, "argument");
            }
        }

        check_stmt(c, it);
    }

    check_type(fn->ret);
    check_stmt(c, fn->body);

    if (fn->ret && !always_returns(fn->body)) {
        fprintf(stderr, PosFmt "ERROR: Expected return statement\n", PosArg(fn->body->token.pos));
        exit(1);
    }

    context_fn_end(c, context_fn_save);
}

void check_nodes(Context *c, Nodes ns) {
    for (Node *it = ns.head; it; it = it->next) {
        check_stmt(c, it);
    }
}
