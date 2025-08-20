#include <stdint.h>

#include "checker.h"
#include "message.h"

static void check_int_limit(Node *n, size_t value) {
    static_assert(COUNT_TYPES == 15, "");
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

    if (value > int_limits[n->type.kind]) {
        error_full(ERROR, n->token.pos, "Integer value '%zu' is too large for type '%s'", value, type_to_cstr(n->type));
        exit(1);
    }
}

static_assert(COUNT_NODES == 22, "");
static void cast_untyped_int(Compiler *c, Node *n, Type expected) {
    switch (n->kind) {
    case NODE_ATOM:
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = expected;
            check_int_limit(n, n->token.as.integer);
            break;

        case TOKEN_IDENT: {
            NodeAtom *atom = (NodeAtom *) n;

            // Only constants can be defined as untyped int
            assert(atom->definition->kind == NODE_CONST);
            NodeConst *definition = (NodeConst *) atom->definition;
            assert(definition->check_status != CHECK_STATUS_DOING);

            n->type = expected;
            check_int_limit(n, definition->value.as.integer);
        } break;

        default:
            unreachable();
        }
        break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        cast_untyped_int(c, unary->operand, expected);
        n->type = expected;
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;
        cast_untyped_int(c, binary->lhs, expected);
        cast_untyped_int(c, binary->rhs, expected);
        n->type = expected;
    } break;

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        n->type = expected;

        Type *type = NULL;
        if (sizeoff->type) {
            type = &sizeoff->type->type;
        } else {
            type = &sizeoff->expr->type;
        }

        check_int_limit(n, compile_sizeof(c, type));
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        cast_untyped_int(c, ret->value, expected);
        n->type = ret->value->type;
    } break;

    default:
        unreachable();
    }
}

static bool try_auto_cast_untyped_int(Compiler *c, Node *n, Type expected) {
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
            cast_untyped_int(c, n, expected);
        }

        return true;
    }

    return false;
}

static Type type_assert(Compiler *c, Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return n->type;
    }

    if (try_auto_cast_untyped_int(c, n, expected)) {
        return expected;
    }

    error_full(ERROR, n->token.pos, "Expected type '%s', got '%s'", type_to_cstr(expected), type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_node(Compiler *c, Node *a, Node *b) {
    if (type_eq(a->type, b->type)) {
        return a->type;
    }

    if (try_auto_cast_untyped_int(c, b, a->type)) {
        return a->type;
    }

    if (try_auto_cast_untyped_int(c, a, b->type)) {
        return b->type;
    }

    error_full(ERROR, a->token.pos, "Expected type '%s', got '%s'", type_to_cstr(b->type), type_to_cstr(a->type));
    exit(1);
}

static Type type_assert_arith(const Node *n, bool pointers_allowed) {
    if (type_is_integer(n->type)) {
        return n->type;
    }

    if (pointers_allowed && type_is_pointer(n->type)) {
        return n->type;
    }

    error_full(
        ERROR,
        n->token.pos,
        "Expected %s type, got '%s'",
        pointers_allowed ? "arithmetic" : "integer",
        type_to_cstr(n->type));

    exit(1);
}

static Type type_assert_scalar(const Node *n) {
    if (type_is_integer(n->type) || type_is_pointer(n->type)) {
        return n->type;
    }

    if (n->type.kind == TYPE_BOOL || n->type.kind == TYPE_FN) {
        return n->type;
    }

    error_full(ERROR, n->token.pos, "Expected scalar type, got '%s'", type_to_cstr(n->type));
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
    error_full(ERROR, n->token.pos, "Undefined %s '" SVFmt "'", label, SVArg(n->token.sv));
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

static void check_type(Compiler *c, Node *n, bool need_full_definition);
static void check_expr(Compiler *c, Node *n, bool ref);
static void check_stmt(Compiler *c, Node *n);

static_assert(COUNT_NODES == 22, "");
static ConstValue eval_const_expr(Compiler *c, Node *n) {
    if (!n) {
        return (ConstValue) {0};
    }

#define const_int(n)  ((ConstValue) {.as.integer = (n)})
#define const_bool(b) ((ConstValue) {.as.boolean = (b)})
#define const_str(s)  ((ConstValue) {.as.sv = (s), .is_string = true})

    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 59, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_INT};
            return const_int(n->token.as.integer);

        case TOKEN_STR: {
            SV sv = n->token.sv;
            sv.data += 1;
            sv.count -= 2;
            resolve_escape_chars(arena_alloc(c->context.arena, n->token.as.integer), &sv);

            n->type = c->context.str_type;
            return const_str(sv);
        }

        case TOKEN_CSTR: {
            SV sv = n->token.sv;
            sv.data += 2;
            sv.count -= 3;
            resolve_escape_chars(arena_alloc(c->context.arena, n->token.as.integer), &sv);

            n->type = c->context.cstr_type;
            return const_str(sv);
        }

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            return const_bool(n->token.as.boolean);

        case TOKEN_CHAR:
            n->type = (Type) {.kind = TYPE_U8};
            return const_int(n->token.as.integer);

        case TOKEN_IDENT:
            atom->definition = ident_find(&c->context, n->token.sv);
            if (!atom->definition) {
                error_undefined(n, "identifier");
            }

            if (atom->definition->kind != NODE_CONST) {
                error_full(ERROR, n->token.pos, "Can only refer to constants in constant expressions");
                exit(1);
            }

            NodeConst *constt = (NodeConst *) atom->definition;
            if (constt->check_status != CHECK_STATUS_DONE) {
                check_stmt(c, atom->definition);
            }

            n->type = atom->definition->type;
            return constt->value;

        default:
            unreachable();
        }
    } break;

    case NODE_CALL:
        error_full(ERROR, n->token.pos, "Unexpected call in constant expression");
        exit(1);
        break;

    case NODE_CAST: {
        NodeCast  *cast = (NodeCast *) n;
        ConstValue value = eval_const_expr(c, cast->from);
        check_type(c, cast->to, true);

        const Type from = type_assert_scalar(cast->from);
        const Type to = type_assert_scalar(cast->to);
        if (!type_eq(from, to) && is_type_cast_illegal(cast->from, cast->to)) {
            error_full(ERROR, n->token.pos, "Cannot cast type '%s' to type '%s'", type_to_cstr(from), type_to_cstr(to));
            exit(1);
        }

        n->type = to;

        if (to.kind == TYPE_BOOL && from.kind != TYPE_BOOL) {
            return const_bool(value.as.integer != 0);
        }
        return value;
    }

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 59, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            ConstValue value = eval_const_expr(c, unary->operand);
            n->type = type_assert_arith(unary->operand, false);
            return const_int(-value.as.integer);
        }

        case TOKEN_MUL:
            error_full(ERROR, n->token.pos, "Unexpected dereference in constant expression");
            exit(1);
            break;

        case TOKEN_BAND:
            error_full(ERROR, n->token.pos, "Unexpected reference in constant expression");
            exit(1);
            break;

        case TOKEN_BNOT: {
            ConstValue value = eval_const_expr(c, unary->operand);
            n->type = type_assert_arith(unary->operand, false);
            return const_int(~value.as.integer);
        }

        case TOKEN_LNOT: {
            ConstValue value = eval_const_expr(c, unary->operand);
            n->type = type_assert(c, unary->operand, (Type) {.kind = TYPE_BOOL});
            return const_bool(!value.as.boolean);
        }

        default:
            unreachable();
        }
    }

    case NODE_INDEX: // TODO: Indexing and Slicing strings
        error_full(ERROR, n->token.pos, "Unexpected index in constant expression");
        exit(1);
        break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        ConstValue lhs = {0};
        ConstValue rhs = {0};

        static_assert(COUNT_TOKENS == 59, "");
        switch (n->token.kind) {
        case TOKEN_ADD: // TODO: Addition of strings
        case TOKEN_SUB:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            type_assert_arith(binary->lhs, true);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_MUL:
        case TOKEN_DIV:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            type_assert_arith(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            type_assert_arith(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SET:
        case TOKEN_ADD_SET:
        case TOKEN_SUB_SET:
        case TOKEN_MUL_SET:
        case TOKEN_DIV_SET:
        case TOKEN_SHL_SET:
        case TOKEN_SHR_SET:
        case TOKEN_BOR_SET:
        case TOKEN_BAND_SET:
            error_full(ERROR, n->token.pos, "Unexpected assignment in constant expression");
            exit(1);
            break;

        case TOKEN_LOR:
        case TOKEN_LAND:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            n->type = type_assert(c, binary->rhs, type_assert(c, binary->lhs, (Type) {.kind = TYPE_BOOL}));
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            type_assert_arith(binary->lhs, true);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        default:
            unreachable();
        }

        static_assert(COUNT_TOKENS == 59, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
            return const_int(lhs.as.integer + rhs.as.integer);

        case TOKEN_SUB:
            return const_int(lhs.as.integer - rhs.as.integer);

        case TOKEN_MUL:
            return const_int(lhs.as.integer * rhs.as.integer);

        case TOKEN_DIV:
            if (type_is_signed(binary->lhs->type)) {
                return const_int((long) lhs.as.integer / (long) rhs.as.integer);
            } else {
                return const_int(lhs.as.integer / rhs.as.integer);
            }

        case TOKEN_SHL:
            return const_int(lhs.as.integer << rhs.as.integer);

        case TOKEN_SHR:
            if (type_is_signed(binary->lhs->type)) {
                return const_int((long) lhs.as.integer >> (long) rhs.as.integer);
            } else {
                return const_int(lhs.as.integer >> rhs.as.integer);
            }

        case TOKEN_BOR:
            return const_int(lhs.as.integer | rhs.as.integer);

        case TOKEN_BAND:
            return const_int(lhs.as.integer & rhs.as.integer);

        case TOKEN_LOR:
            return const_bool(lhs.as.boolean || rhs.as.boolean);

        case TOKEN_LAND:
            return const_bool(lhs.as.boolean && rhs.as.boolean);

        case TOKEN_GT:
            if (type_is_signed(binary->lhs->type)) {
                return const_bool((long) lhs.as.integer > (long) rhs.as.integer);
            } else {
                return const_bool(lhs.as.integer > rhs.as.integer);
            }

        case TOKEN_GE:
            if (type_is_signed(binary->lhs->type)) {
                return const_bool((long) lhs.as.integer >= (long) rhs.as.integer);
            } else {
                return const_bool(lhs.as.integer >= rhs.as.integer);
            }

        case TOKEN_LT:
            if (type_is_signed(binary->lhs->type)) {
                return const_bool((long) lhs.as.integer < (long) rhs.as.integer);
            } else {
                return const_bool(lhs.as.integer < rhs.as.integer);
            }

        case TOKEN_LE:
            if (type_is_signed(binary->lhs->type)) {
                return const_bool((long) lhs.as.integer <= (long) rhs.as.integer);
            } else {
                return const_bool(lhs.as.integer <= rhs.as.integer);
            }

        case TOKEN_EQ:
            return const_bool(lhs.as.integer == rhs.as.integer);

        case TOKEN_NE:
            return const_bool(lhs.as.integer != rhs.as.integer);

        default:
            unreachable();
        }
    }

    case NODE_MEMBER: // TODO: Accessing members of strings
        error_full(ERROR, n->token.pos, "Unexpected member access in constant expression");
        exit(1);
        break;

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        check_type(c, sizeoff->type, true);
        check_expr(c, sizeoff->expr, false);
        n->type = (Type) {.kind = TYPE_INT};

        Type *type = NULL;
        if (sizeoff->type) {
            type = &sizeoff->type->type;
        } else {
            type = &sizeoff->expr->type;
        }

        return const_int(compile_sizeof(c, type));
    }

    case NODE_COMPOUND:
        error_full(ERROR, n->token.pos, "Unexpected compound literal in constant expression");
        exit(1);
        break;

    case NODE_FN:
        error_full(ERROR, n->token.pos, "Unexpected function in constant expression");
        exit(1);
        break;

    case NODE_IF:
    case NODE_FOR:
    case NODE_BLOCK:
    case NODE_RETURN:
    case NODE_VAR:
    case NODE_FIELD:
    case NODE_STRUCT:
    case NODE_EXTERN:
    case NODE_PRINT:
    default:
        unreachable();
        break;
    }

#undef const_int
#undef const_bool
}

static_assert(COUNT_NODES == 22, "");
static_assert(COUNT_TYPES == 15, "");
static void check_type(Compiler *c, Node *n, bool need_full_definition) {
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
        } else if (sv_match(n->token.sv, "str")) {
            n->type = c->context.str_type;
        } else if (sv_match(n->token.sv, "cstr")) {
            n->type = c->context.cstr_type;
        } else {
            Node *definition = scope_find(c->context.types, n->token.sv);
            if (!definition) {
                error_undefined(n, "type");
            }

            switch (definition->kind) {
            case NODE_TYPE: {
                NodeType *type = (NodeType *) definition;
                if (type->check_status != CHECK_STATUS_DONE) {
                    check_stmt(c, definition);
                }

                n->type = definition->type;
                n->type.alias = type;
                type->ref = n->type.ref;
            } break;

            case NODE_STRUCT: {
                NodeStruct *structt = (NodeStruct *) definition;
                if (structt->check_status != CHECK_STATUS_DONE && need_full_definition) {
                    check_stmt(c, definition);
                }

                n->type = definition->type;
            } break;

            default:
                unreachable();
            }
        }
        break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        check_type(c, unary->operand, false);
        n->type = unary->operand->type;
        n->type.ref++;
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        check_type(c, index->base, true);
        n->type = (Type) {
            .kind = TYPE_SLICE,
            .spec_type = &index->base->type,
        };
    } break;

    case NODE_FN: {
        NodeFn *spec = (NodeFn *) n;
        for (Node *it = spec->args.head; it; it = it->next) {
            NodeVar *arg = (NodeVar *) it;
            check_type(c, arg->type, true);
            it->type = arg->type->type;
        }

        check_type(c, spec->ret, true);
        n->type = (Type) {.kind = TYPE_FN, .spec_node = n};
    } break;

    default:
        unreachable();
    }
}

static void check_fn(Compiler *c, Node *n);

static_assert(COUNT_NODES == 22, "");
static void check_expr(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return;
    }

    bool allow_ref = false;
    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 59, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_INT};
            break;

        case TOKEN_STR:
            n->type = c->context.str_type;
            break;

        case TOKEN_CSTR:
            n->type = (Type) {.kind = TYPE_I8, .ref = 1};
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_CHAR:
            n->type = (Type) {.kind = TYPE_U8};
            break;

        case TOKEN_IDENT:
            atom->definition = ident_find(&c->context, n->token.sv);
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
            error_full(ERROR, call->fn->token.pos, "Cannot call type '%s'", type_to_cstr(fn_type));
            exit(1);
        }

        if (fn_type.ref != 0) {
            error_full(
                ERROR,
                call->fn->token.pos,
                "Cannot call type '%s' without dereferencing it first",
                type_to_cstr(call->fn->type));

            exit(1);
        }

        const NodeFn *expected = (const NodeFn *) fn_type.spec_node;
        if (call->arity != expected->arity) {
            error_full(
                ERROR,
                n->token.pos,
                "Expected %zu argument%s, got %zu",
                expected->arity,
                expected->arity == 1 ? "" : "s",
                call->arity);

            exit(1);
        }

        for (Node *a = call->args.head, *e = expected->args.head; a; a = a->next, e = e->next) {
            check_expr(c, a, false);
            type_assert_node(c, a, e);
        }

        n->type = node_fn_return_type(expected);
    } break;

    case NODE_CAST: {
        NodeCast *cast = (NodeCast *) n;
        check_expr(c, cast->from, false);
        check_type(c, cast->to, true);

        const Type from = type_assert_scalar(cast->from);
        const Type to = type_assert_scalar(cast->to);
        if (!type_eq(from, to) && is_type_cast_illegal(cast->from, cast->to)) {
            error_full(ERROR, n->token.pos, "Cannot cast type '%s' to type '%s'", type_to_cstr(from), type_to_cstr(to));
            exit(1);
        }

        n->type = to;
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 59, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_expr(c, unary->operand, false);
            n->type = type_assert_arith(unary->operand, false);
            break;

        case TOKEN_MUL:
            check_expr(c, unary->operand, false);

            if (!type_is_pointer(unary->operand->type)) {
                error_full(
                    ERROR,
                    unary->operand->token.pos,
                    "Expected pointer type, got '%s'",
                    type_to_cstr(unary->operand->type));

                exit(1);
            }

            if (type_eq(unary->operand->type, (Type) {.kind = TYPE_RAWPTR})) {
                error_full(ERROR, unary->operand->token.pos, "Cannot dereference raw pointer");
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

        case TOKEN_BNOT:
            check_expr(c, unary->operand, false);
            n->type = type_assert_arith(unary->operand, false);
            break;

        case TOKEN_LNOT:
            check_expr(c, unary->operand, false);
            n->type = type_assert(c, unary->operand, (Type) {.kind = TYPE_BOOL});
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        check_expr(c, index->base, ref);

        if (index->ranged) {
            if (!index->base->type.ref && index->base->type.kind != TYPE_SLICE) {
                error_full(
                    ERROR,
                    index->base->token.pos,
                    "Expected typed pointer or slice, got '%s'",
                    type_to_cstr(index->base->type));

                exit(1);
            }

            if (index->base->type.ref && !index->to) {
                error_full(ERROR, index->base->token.pos, "Cannot infer range end of pointer type");
                exit(1);
            }
        } else {
            if (type_is_pointer(index->base->type) || index->base->type.kind != TYPE_SLICE) {
                error_full(
                    ERROR, index->base->token.pos, "Expected slice type, got '%s'", type_to_cstr(index->base->type));

                exit(1);
            }
        }

        if (index->from) {
            check_expr(c, index->from, false);
            type_assert_arith(index->from, false);
        }

        if (index->ranged) {
            if (index->to) {
                check_expr(c, index->to, false);
                type_assert_arith(index->to, false);
            }

            if (index->base->type.ref) {
                Type element = index->base->type;
                element.ref--;

                n->type = (Type) {
                    .kind = TYPE_SLICE,
                    .spec_type = arena_clone(c->context.arena, &element, sizeof(element)),
                };
            } else if (index->base->type.kind == TYPE_SLICE) {
                n->type = index->base->type;
            } else {
                unreachable();
            }
        } else {
            assert(index->base->type.kind == TYPE_SLICE);
            n->type = *index->base->type.spec_type;
            allow_ref = true;
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 59, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            type_assert_arith(binary->lhs, true);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_MUL:
        case TOKEN_DIV:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            type_assert_arith(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            type_assert_arith(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SET:
            check_expr(c, binary->lhs, true);
            check_expr(c, binary->rhs, false);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_ADD_SET:
        case TOKEN_SUB_SET:
            check_expr(c, binary->lhs, true);
            check_expr(c, binary->rhs, false);
            type_assert(c, binary->rhs, type_assert_arith(binary->lhs, true));
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_MUL_SET:
        case TOKEN_DIV_SET:
            check_expr(c, binary->lhs, true);
            check_expr(c, binary->rhs, false);
            type_assert(c, binary->rhs, type_assert_arith(binary->lhs, false));
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_SHL_SET:
        case TOKEN_SHR_SET:
        case TOKEN_BOR_SET:
        case TOKEN_BAND_SET:
            check_expr(c, binary->lhs, true);
            check_expr(c, binary->rhs, false);
            type_assert(c, binary->rhs, type_assert_arith(binary->lhs, false));
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_LOR:
        case TOKEN_LAND:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            n->type = type_assert(c, binary->rhs, type_assert(c, binary->lhs, (Type) {.kind = TYPE_BOOL}));
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            type_assert_arith(binary->lhs, true);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_MEMBER: {
        NodeMember *member = (NodeMember *) n;

        check_expr(c, member->lhs, ref);
        if (member->lhs->type.kind == TYPE_STRUCT) {
            NodeStruct *structt = (NodeStruct *) member->lhs->type.spec_node;

            member->definition = nodes_find(structt->fields, n->token.sv, NULL);
            if (!member->definition) {
                error_undefined(n, "field");
            }

            allow_ref = ref;
            n->type = member->definition->type;
        } else if (member->lhs->type.kind == TYPE_SLICE) {
            if (sv_match(n->token.sv, "data")) {
                n->type = *member->lhs->type.spec_type;
                n->type.ref++;
                n->token.as.integer = 0;
            } else if (sv_match(n->token.sv, "count")) {
                n->type = (Type) {.kind = TYPE_I64};
                n->token.as.integer = 8;
            } else {
                error_undefined(n, "field");
            }

            allow_ref = ref;
        } else {
            if (member->lhs->type.kind != TYPE_STRUCT && member->lhs->type.kind != TYPE_SLICE) {
                error_full(
                    ERROR,
                    member->lhs->token.pos,
                    "Expected structure or slice type, got '%s'",
                    type_to_cstr(member->lhs->type));

                exit(1);
            }
        }
    } break;

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        check_type(c, sizeoff->type, true);
        check_expr(c, sizeoff->expr, false);
        n->type = (Type) {.kind = TYPE_INT};
    } break;

    case NODE_COMPOUND: {
        NodeCompound *compound = (NodeCompound *) n;
        check_type(c, compound->type, true);

        n->type = compound->type->type;
        if (n->type.kind != TYPE_STRUCT) {
            error_full(
                ERROR,
                compound->type->token.pos,
                "Expected structure type, got '%s'",
                type_to_cstr(compound->type->type));

            exit(1);
        }

        NodeStruct *spec = (NodeStruct *) n->type.spec_node;

        Node *ordered_iota = spec->fields.head;
        for (Node *it = compound->nodes.head; it; it = it->next) {
            if (it->kind == NODE_BINARY && it->token.kind == TOKEN_COLON) {
                NodeBinary *assign = (NodeBinary *) it;

                assert(assign->lhs->kind == NODE_ATOM && assign->lhs->token.kind == TOKEN_IDENT);
                NodeAtom *lhs = (NodeAtom *) assign->lhs;

                lhs->definition = nodes_find(spec->fields, lhs->node.token.sv, NULL);
                if (!lhs->definition) {
                    error_undefined((Node *) lhs, "field");
                }

                check_expr(c, assign->rhs, false);
                type_assert(c, assign->rhs, lhs->definition->type);
            } else {
                if (!ordered_iota) {
                    error_full(ERROR, it->token.pos, "Too many ordered initializers");
                    exit(1);
                }

                check_expr(c, it, false);
                type_assert(c, it, ordered_iota->type);
                ordered_iota = ordered_iota->next;
            }
        }
    } break;

    case NODE_FN:
        check_fn(c, n);
        break;

    default:
        unreachable();
    }

    if (!allow_ref && ref) {
        error_full(ERROR, n->token.pos, "Cannot take reference to value not in memory");
        exit(1);
    }
}

static void error_redefinition(const Node *n, const Node *previous, const char *label) {
    error_full(ERROR, n->token.pos, "Redefinition of %s '" SVFmt "'", label, SVArg(n->token.sv));
    fprintf(stderr, "\n");
    error_full(NOTE, previous->token.pos, "Defined here");
    exit(1);
}

static_assert(COUNT_NODES == 22, "");
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

static_assert(COUNT_NODES == 22, "");
static void check_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        NodeAssert *assertt = (NodeAssert *) n;
        if (assertt->is_static) {
            ConstValue value = eval_const_expr(c, assertt->expr);
            type_assert(c, assertt->expr, (Type) {.kind = TYPE_BOOL});

            if (!value.as.boolean) {
                fprintf(stderr, PosFmt " Assertion Failed\n", PosArg(assertt->expr->token.pos));
                exit(1);
            }
        } else {
            check_expr(c, assertt->expr, false);
            type_assert(c, assertt->expr, (Type) {.kind = TYPE_BOOL});
        }
    } break;

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        check_expr(c, iff->condition, false);
        type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});

        check_stmt(c, iff->consequence);
        check_stmt(c, iff->antecedence);
    } break;

    case NODE_FOR: {
        NodeFor *forr = (NodeFor *) n;
        check_stmt(c, forr->init);

        if (forr->condition) {
            check_expr(c, forr->condition, false);
            type_assert(c, forr->condition, (Type) {.kind = TYPE_BOOL});
        }

        check_expr(c, forr->update, false);
        check_stmt(c, forr->body);
    } break;

    case NODE_BLOCK: {
        const size_t types_count_save = c->context.types.count;
        const size_t locals_count_save = c->context.locals.count;

        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }

        c->context.types.count = types_count_save;
        c->context.locals.count = locals_count_save;
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;

        n->type = (Type) {.kind = TYPE_UNIT};
        if (ret->value) {
            check_expr(c, ret->value, false);
            n->type = ret->value->type;
        }

        type_assert(c, n, node_fn_return_type(c->context.fn.fn));
    } break;

    case NODE_FN:
        check_fn(c, n);
        break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (var->kind == NODE_VAR_GLOBAL) {
            if (var->check_status == CHECK_STATUS_DOING) {
                error_full(ERROR, n->token.pos, "Reference loop");
                exit(1);
            }

            var->check_status = CHECK_STATUS_DOING;
        }

        if (var->type) {
            check_type(c, var->type, true);
            n->type = var->type->type;
        }

        if (var->expr) {
            check_expr(c, var->expr, false);
            n->type = var->expr->type;

            if (n->type.kind == TYPE_UNIT) {
                error_full(ERROR, n->token.pos, "Cannot define variable with type '%s'", type_to_cstr(n->type));
                exit(1);
            }

            if (var->type) {
                type_assert(c, var->expr, var->type->type);
                n->type = var->expr->type;
            }

            if (n->type.kind == TYPE_INT) {
                n->type.kind = TYPE_I64;
            }
        }

        var->check_status = CHECK_STATUS_DONE;

        switch (var->kind) {
        case NODE_VAR_ARG:
            if (!c->context.in_extern) {
                da_push(&c->context.locals, n);
            }
            break;

        case NODE_VAR_LOCAL:
            da_push(&c->context.locals, n);
            break;

        case NODE_VAR_GLOBAL:
            // Pass
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_TYPE: {
        NodeType *type = (NodeType *) n;
        if (!type->local) {
            if (type->check_status == CHECK_STATUS_DOING) {
                error_full(ERROR, n->token.pos, "Reference loop");
                exit(1);
            }

            type->check_status = CHECK_STATUS_DOING;
        }

        check_type(c, type->definition, true);
        n->type = type->definition->type;

        type->check_status = CHECK_STATUS_DONE;
        if (type->local) {
            da_push(&c->context.types, n);
        }
    } break;

    case NODE_CONST: {
        NodeConst *constt = (NodeConst *) n;
        if (!constt->local) {
            if (constt->check_status == CHECK_STATUS_DOING) {
                error_full(ERROR, n->token.pos, "Reference loop");
                exit(1);
            }

            constt->check_status = CHECK_STATUS_DOING;
        }

        if (constt->type) {
            check_type(c, constt->type, true);
            n->type = constt->type->type;
        }

        constt->value = eval_const_expr(c, constt->expr);
        n->type = constt->expr->type;

        if (constt->type) {
            type_assert(c, constt->expr, constt->type->type);
            n->type = constt->expr->type;
        }

        constt->check_status = CHECK_STATUS_DONE;
        if (constt->local) {
            da_push(&c->context.locals, n);
        }
    } break;

    case NODE_STRUCT: {
        NodeStruct *structt = (NodeStruct *) n;
        if (!structt->local) {
            if (structt->check_status == CHECK_STATUS_DOING) {
                error_full(ERROR, n->token.pos, "Reference loop");
                exit(1);
            }

            structt->check_status = CHECK_STATUS_DOING;
        }

        for (Node *it = structt->fields.head; it; it = it->next) {
            const Node *previous = nodes_find(structt->fields, it->token.sv, it);
            if (previous) {
                error_redefinition(it, previous, "field");
            }

            NodeField *field = (NodeField *) it;
            check_type(c, field->type, true);

            it->type = field->type->type;
            if (it->type.kind == TYPE_UNIT) {
                error_full(ERROR, it->token.pos, "Cannot define field with type '%s'", type_to_cstr(it->type));
                exit(1);
            }
        }

        n->type = (Type) {.kind = TYPE_STRUCT, .spec_node = n};

        structt->check_status = CHECK_STATUS_DONE;
        if (structt->local) {
            da_push(&c->context.types, n);
        }
    } break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        c->context.in_extern = true;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
        c->context.in_extern = false;
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

static void check_fn(Compiler *c, Node *n) {
    NodeFn *fn = (NodeFn *) n;
    if (fn->local) {
        da_push(&c->context.locals, n);
    }
    n->type = (Type) {.kind = TYPE_FN, .spec_node = n};

    const ContextFn context_fn_save = context_fn_begin(&c->context, fn);
    for (Node *it = fn->args.head; it; it = it->next) {
        if (fn->local && it->token.kind == TOKEN_IDENT) {
            const Node *previous = nodes_find(fn->args, it->token.sv, it);
            if (previous) {
                error_redefinition(it, previous, "argument");
            }
        }

        check_stmt(c, it);
    }

    if (fn->local) {
        check_type(c, fn->ret, true);
    }

    if (fn->body) {
        check_stmt(c, fn->body);
        if (fn->ret && !always_returns(fn->body)) {
            error_full(ERROR, fn->body->token.pos, "Expected return statement");
            exit(1);
        }
    }

    context_fn_end(&c->context, context_fn_save);
}

static_assert(COUNT_NODES == 22, "");
static void pre_register_top_level_stmt(Compiler *c, Node *n) {
    switch (n->kind) {
    case NODE_FN:
    case NODE_VAR:
    case NODE_CONST: {
        const Node *previous = scope_find(c->context.globals, n->token.sv);
        if (previous) {
            error_redefinition(n, previous, "identifier");
        }

        da_push(&c->context.globals, n);
    } break;

    case NODE_TYPE: {
        const Node *previous = scope_find(c->context.types, n->token.sv);
        if (previous) {
            error_redefinition(n, previous, "type");
        }

        da_push(&c->context.types, n);
    } break;

    case NODE_STRUCT: {
        const Node *previous = scope_find(c->context.types, n->token.sv);
        if (previous) {
            error_redefinition(n, previous, "type");
        }

        n->type = (Type) {.kind = TYPE_STRUCT, .spec_node = n};
        da_push(&c->context.types, n);
    } break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            pre_register_top_level_stmt(c, it);
        }
    } break;

    case NODE_ASSERT:
        // Pass
        break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 22, "");
static void pre_typecheck_top_level_stmt(Compiler *c, Node *n) {
    switch (n->kind) {
    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        if (fn->check_status == CHECK_STATUS_DOING) {
            error_full(ERROR, n->token.pos, "Reference loop");
            exit(1);
        }

        fn->check_status = CHECK_STATUS_DOING;

        n->type = (Type) {.kind = TYPE_FN, .spec_node = n};

        for (Node *it = fn->args.head; it; it = it->next) {
            if (it->token.kind == TOKEN_IDENT) {
                const Node *previous = nodes_find(fn->args, it->token.sv, it);
                if (previous) {
                    error_redefinition(it, previous, "argument");
                }
            }

            NodeVar *var = (NodeVar *) it;
            assert(var->type);
            check_type(c, var->type, true);
            it->type = var->type->type;
        }

        check_type(c, fn->ret, true);
        fn->check_status = CHECK_STATUS_DONE;
    } break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            pre_typecheck_top_level_stmt(c, it);
        }
    } break;

    default:
        check_stmt(c, n);
        break;
    }
}

static Type alias_type(Arena *a, SV name, Type type) {
    NodeType *alias = arena_alloc(a, sizeof(NodeType));
    alias->node.token.sv = name;
    alias->node.type = type;
    alias->node.type.alias = alias;
    alias->ref = type.ref;
    alias->check_status = CHECK_STATUS_DONE;
    return alias->node.type;
}

void check_nodes(Compiler *c, Nodes ns) {
    assert(c->context.arena);

    if (!c->context.str_type.alias) {
        const Type u8_type = {.kind = TYPE_U8};
        const Type u8_slice_type = (Type) {
            .kind = TYPE_SLICE,
            .spec_type = arena_clone(c->context.arena, &u8_type, sizeof(u8_type)),
        };

        c->context.str_type = alias_type(c->context.arena, sv_from_cstr("str"), u8_slice_type);
    }

    if (!c->context.cstr_type.alias) {
        c->context.cstr_type = alias_type(c->context.arena, sv_from_cstr("cstr"), (Type) {.kind = TYPE_I8, .ref = 1});
    }

    for (Node *it = ns.head; it; it = it->next) {
        pre_register_top_level_stmt(c, it);
    }

    for (Node *it = ns.head; it; it = it->next) {
        pre_typecheck_top_level_stmt(c, it);
    }

    for (Node *it = ns.head; it; it = it->next) {
        check_stmt(c, it);
    }
}
