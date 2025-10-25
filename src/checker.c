#include <stdint.h>

#include "checker.h"
#include "message.h"

void check_int_limit(Node *n, size_t value) {
    static_assert(COUNT_TYPES == 18, "");
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

static_assert(COUNT_NODES == 23, "");
static void cast_untyped(Compiler *c, Node *n, Type expected) {
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
        cast_untyped(c, unary->operand, expected);
        n->type = expected;
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;
        cast_untyped(c, binary->lhs, expected);
        cast_untyped(c, binary->rhs, expected);
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

        if (type->kind != TYPE_GENERIC) {
            check_int_limit(n, compile_sizeof(c, type));
        }
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        cast_untyped(c, ret->value, expected);
        n->type = ret->value->type;
    } break;

    default:
        unreachable();
    }
}

static bool try_auto_cast_untyped(Compiler *c, Node *n, Type expected) {
    // Untyped integer -> Typed integer
    if (type_is_integer(type_remove_ref(expected)) && n->type.kind == TYPE_INT) {
        // The indirection level of the typed and untyped integers must match
        if (expected.ref != n->type.ref) {
            return false;
        }

        if (expected.kind != TYPE_INT) {
            cast_untyped(c, n, expected);
        }

        return true;
    }

    return false;
}

static Type type_assert(Compiler *c, Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return n->type;
    }

    if (try_auto_cast_untyped(c, n, expected)) {
        return expected;
    }

    error_full(ERROR, n->token.pos, "Expected type '%s', got '%s'", type_to_cstr(expected), type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_node(Compiler *c, Node *a, Node *b) {
    if (type_eq(a->type, b->type)) {
        return a->type;
    }

    if (try_auto_cast_untyped(c, b, a->type)) {
        return a->type;
    }

    if (try_auto_cast_untyped(c, a, b->type)) {
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

static void error_use_of_private(const Token *token, const Node *defined, const char *label) {
    error_full(ERROR, token->pos, "Cannot use private %s '" SVFmt "' outside its package", label, SVArg(token->sv));
    fprintf(stderr, "\n");
    error_full(NOTE, defined->token.pos, "Defined here");
    exit(1);
}

static Node *ident_find(const Context *c, Package *package, const Token *token, bool is_type, bool is_imported) {
    if (c->fn.fn) {
        Node *n = context_fn_find(c->fn, c->locals, token->sv, is_type);
        if (n) {
            return n;
        }
    }

    Node *global = scope_find(package->globals, token->sv, is_type);
    if (global && is_imported) {
        bool        is_public = false;
        const char *label = NULL;

        switch (global->kind) {
        case NODE_FN:
            label = "identifier";
            is_public = ((NodeFn *) global)->is_public;
            break;

        case NODE_VAR:
            label = "identifier";
            is_public = ((NodeVar *) global)->is_public;
            break;

        case NODE_TYPE:
            label = "type";
            is_public = ((NodeType *) global)->is_public;
            break;

        case NODE_CONST:
            label = "identifier";
            is_public = ((NodeConst *) global)->is_public;
            break;

        case NODE_STRUCT:
            label = "type";
            is_public = ((NodeStruct *) global)->is_public;
            break;

        default:
            unreachable();
        }

        if (!is_public) {
            assert(label);
            error_use_of_private(token, global, label);
        }
    }

    return global;
}

static Node *nodes_find(Nodes ns, SV name, Node *until) {
    for (Node *it = ns.head; it && it != until; it = it->next) {
        if (sv_eq(it->token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

typedef enum {
    REF_NONE,
    REF_ADDR,
    REF_MUTATE,
} RefKind;

static void check_type(Compiler *c, Node *n, bool need_full_definition, Node *extra_generic_context);
static void check_expr(Compiler *c, Node *n, RefKind ref);
static void check_stmt(Compiler *c, Node *n);

static void resolve_ident_package(Node *n) {
    assert(n->kind == NODE_ATOM && n->token.kind == TOKEN_IDENT);

    NodeAtom *atom = (NodeAtom *) n;
    if (atom->scope_resolved || !atom->scope.sv.data) {
        return;
    }
    atom->scope_resolved = true;

    for (Import *i = atom->package->imports.head; i; i = i->next) {
        if (sv_eq(i->as, atom->scope.sv)) {
            atom->package = i->package;
            return;
        }
    }

    error_full(ERROR, atom->scope.pos, "Package '" SVFmt "' not imported", SVArg(atom->scope.sv));
    exit(1);
}

static_assert(COUNT_NODES == 23, "");
static ConstValue eval_const_expr_impl(Compiler *c, Node *n) {
    if (!n) {
        return (ConstValue) {0};
    }

#define const_int(n)  ((ConstValue) {.as.integer = (n)})
#define const_bool(b) ((ConstValue) {.as.boolean = (b)})
#define const_str(s)  ((ConstValue) {.as.sv = (s), .is_string = true})

    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_INT};
            return const_int(n->token.as.integer);

        case TOKEN_STR:
            n->type = c->context.str_type;
            return const_str(resolve_str_token(n->token, c->context.arena));

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            return const_bool(n->token.as.boolean);

        case TOKEN_CHAR:
            n->type = (Type) {.kind = TYPE_U8};
            return const_int(n->token.as.integer);

        case TOKEN_IDENT:
            resolve_ident_package(n);

            atom->definition = ident_find(&c->context, atom->package, &n->token, false, atom->scope_resolved);
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

    case NODE_CAST: {
        NodeCast  *cast = (NodeCast *) n;
        ConstValue value = eval_const_expr_impl(c, cast->from);
        check_type(c, cast->to, true, NULL);

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

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            ConstValue value = eval_const_expr_impl(c, unary->operand);
            n->type = type_assert_arith(unary->operand, false);
            return const_int(-value.as.integer);
        }

        case TOKEN_BNOT: {
            ConstValue value = eval_const_expr_impl(c, unary->operand);
            n->type = type_assert_arith(unary->operand, false);
            return const_int(~value.as.integer);
        }

        case TOKEN_LNOT: {
            ConstValue value = eval_const_expr_impl(c, unary->operand);
            n->type = type_assert(c, unary->operand, (Type) {.kind = TYPE_BOOL});
            return const_bool(!value.as.boolean);
        }

        case TOKEN_LEN: {
            ConstValue value = eval_const_expr_impl(c, unary->operand);
            if (!value.is_string) {
                error_full(
                    ERROR,
                    unary->operand->token.pos,
                    "Expected type '[u8]', got '%s'",
                    type_to_cstr(unary->operand->type));

                exit(1);
            }

            n->type = (Type) {.kind = TYPE_I64};
            return const_int(value.as.sv.count);
        }

        default:
            unreachable();
        }
    }

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;

        ConstValue lhs = eval_const_expr_impl(c, index->base);
        if (!lhs.is_string) {
            error_full(
                ERROR, index->base->token.pos, "Expected type '[u8]', got '%s'", type_to_cstr(index->base->type));
            exit(1);
        }

        ConstValue from = {0};
        if (index->from) {
            from = eval_const_expr_impl(c, index->from);
            type_assert_arith(index->from, false);
        }

        ConstValue to = {0};
        if (index->to) {
            to = eval_const_expr_impl(c, index->to);
            type_assert_arith(index->to, false);
        }

        if (index->ranged) {
            if (from.as.integer >= lhs.as.sv.count || to.as.integer >= lhs.as.sv.count) {
                error_full(
                    ERROR,
                    n->token.pos,
                    "Range (%ld..%ld) is out of bounds in slice of length %ld",
                    from.as.integer,
                    to.as.integer,
                    lhs.as.sv.count);

                exit(1);
            }

            lhs.as.sv.data += from.as.integer;
            lhs.as.sv.count = to.as.integer - from.as.integer;
            n->type = index->base->type;
            return lhs;
        } else {
            if (from.as.integer >= lhs.as.sv.count) {
                error_full(
                    ERROR,
                    n->token.pos,
                    "Index %ld is out of bounds in string of length %ld",
                    from.as.integer,
                    lhs.as.sv.count);

                exit(1);
            }

            n->type = (Type) {.kind = TYPE_U8};
            return const_int(lhs.as.sv.data[from.as.integer]);
        }
    }

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        ConstValue lhs = {0};
        ConstValue rhs = {0};

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
            lhs = eval_const_expr_impl(c, binary->lhs);
            rhs = eval_const_expr_impl(c, binary->rhs);

            if (!lhs.is_string) {
                type_assert_arith(binary->lhs, true);
            }

            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SUB:
            lhs = eval_const_expr_impl(c, binary->lhs);
            rhs = eval_const_expr_impl(c, binary->rhs);
            type_assert_arith(binary->lhs, true);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
            lhs = eval_const_expr_impl(c, binary->lhs);
            rhs = eval_const_expr_impl(c, binary->rhs);
            type_assert_arith(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
            lhs = eval_const_expr_impl(c, binary->lhs);
            rhs = eval_const_expr_impl(c, binary->rhs);
            type_assert_arith(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_LOR:
        case TOKEN_LAND:
            lhs = eval_const_expr_impl(c, binary->lhs);
            rhs = eval_const_expr_impl(c, binary->rhs);
            n->type = type_assert(c, binary->rhs, type_assert(c, binary->lhs, (Type) {.kind = TYPE_BOOL}));
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            lhs = eval_const_expr_impl(c, binary->lhs);
            rhs = eval_const_expr_impl(c, binary->rhs);
            type_assert_arith(binary->lhs, true);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        default:
            unreachable();
        }

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
            if (lhs.is_string) {
                char *s = temp_alloc(lhs.as.sv.count + rhs.as.sv.count);
                memcpy(s, lhs.as.sv.data, lhs.as.sv.count);
                memcpy(s + lhs.as.sv.count, rhs.as.sv.data, rhs.as.sv.count);
                const SV sv = {.data = s, .count = lhs.as.sv.count + rhs.as.sv.count};
                return const_str(sv);
            }

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

        case TOKEN_MOD:
            if (type_is_signed(binary->lhs->type)) {
                return const_int((long) lhs.as.integer % (long) rhs.as.integer);
            } else {
                return const_int(lhs.as.integer % rhs.as.integer);
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

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        check_type(c, sizeoff->type, true, NULL);
        check_expr(c, sizeoff->expr, false);
        n->type = (Type) {.kind = TYPE_INT};

        Type *type = NULL;
        if (sizeoff->type) {
            type = &sizeoff->type->type;
        } else {
            type = &sizeoff->expr->type;
        }

        if (type->kind == TYPE_GENERIC) {
            error_full(ERROR, n->token.pos, "Cannot calculate constant size of generic type '%s'", type_to_cstr(*type));
            exit(1);
        }
        return const_int(compile_sizeof(c, type));
    }

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;

        const ConstValue condition = eval_const_expr_impl(c, iff->condition);
        type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});

        const ConstValue consequence = eval_const_expr_impl(c, iff->consequence);
        const ConstValue antecedence = eval_const_expr_impl(c, iff->antecedence);
        n->type = type_assert_node(c, iff->antecedence, iff->consequence);

        return condition.as.boolean ? consequence : antecedence;
    }

    default:
        unreachable();
        break;
    }

#undef const_int
#undef const_bool
}

static ConstValue eval_const_expr(Compiler *c, Node *n) {
    const char *save = temp_alloc(0);

    ConstValue value = eval_const_expr_impl(c, n);
    if (value.is_string) {
        value.as.sv.data = arena_clone(c->context.arena, value.as.sv.data, value.as.sv.count);
    }

    temp_reset(save);
    return value;
}

static_assert(COUNT_TYPES == 18, "");
static Type instantiate_type(Compiler *c, Type type, Node *generics, size_t generics_count) {
    switch (type.kind) {
    case TYPE_UNIT:
    case TYPE_BOOL:
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
    case TYPE_INT:
    case TYPE_RAWPTR:
        return type;

    case TYPE_FN: {
        assert(type.spec_node);
        NodeFn *from = (NodeFn *) type.spec_node;
        NodeFn *to = arena_alloc(c->context.arena, sizeof(NodeFn));
        to->node = from->node;
        to->node.type.spec_node = (Node *) to;

        to->arity = from->arity;
        for (Node *it = from->args.head; it; it = it->next) {
            NodeVar *arg = arena_alloc(c->context.arena, sizeof(NodeVar));
            arg->node = *it;
            arg->node.type = instantiate_type(c, it->type, generics, generics_count);
            nodes_push(&to->args, (Node *) arg);
        }

        if (from->ret) {
            to->ret = arena_clone(c->context.arena, from->ret, sizeof(*from->ret));
            to->ret->type = instantiate_type(c, to->ret->type, generics, generics_count);
        }

        type.spec_node = (Node *) to;
        return type;
    }

    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_DSLICE: {
        const Type base = instantiate_type(c, *type.spec_type, generics, generics_count);
        type.spec_type = arena_clone(c->context.arena, &base, sizeof(base));
        return type;
    }

    case TYPE_STRUCT: {
        assert(type.spec_node);
        if (type.spec_struct_instance) {
            type.spec_node = (Node *) type.spec_struct_instance->definition;
            type.spec_struct_instance = NULL;
        }

        NodeStruct *from = (NodeStruct *) type.spec_node;

        Instantiation *instantiation =
            instantiations_get(&from->instantiations, generics, generics_count, c->context.arena);

        if (instantiation->structt_ok) {
            Type result = instantiation->structt_type;
            result.ref = type.ref;
            return result;
        }

        NodeStruct *to = arena_alloc(c->context.arena, sizeof(NodeStruct));
        to->node = from->node;
        to->package = from->package;
        to->node.type.spec_node = (Node *) to;

        const StructInstanace instance = {
            .generics = generics,
            .definition = from,
        };

        type.spec_node = (Node *) to;
        type.spec_struct_instance = arena_clone(c->context.arena, &instance, sizeof(instance));
        instantiation->structt_ok = true;
        instantiation->structt_type = type_remove_ref(type);

        for (Node *it = from->fields.head; it; it = it->next) {
            NodeField *field = arena_alloc(c->context.arena, sizeof(NodeField));
            field->node = *it;
            field->node.type = instantiate_type(c, it->type, generics, generics_count);
            nodes_push(&to->fields, (Node *) field);
        }

        return type;
    }

    case TYPE_GENERIC: {
        assert(type.spec_node);
        const size_t n = type.spec_node->token.as.integer;

        Node *instance = generics;
        for (size_t i = 0; i < n; i++) {
            assert(instance);
            instance = instance->next;
        }

        assert(instance);
        Type result = instance->type;
        result.ref += type.ref;
        return result;
    }

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_NODES == 23, "");
static_assert(COUNT_TYPES == 18, "");
static void check_type(Compiler *c, Node *n, bool need_full_definition, Node *extra_generic_context) {
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
            resolve_ident_package(n);

            if (extra_generic_context) {
                for (Node *it = extra_generic_context; it; it = it->next) {
                    if (sv_eq(it->token.sv, n->token.sv)) {
                        n->type = it->type;
                        return;
                    }
                }
            }

            NodeAtom *atom = (NodeAtom *) n;
            Node     *definition = ident_find(&c->context, atom->package, &n->token, true, atom->scope_resolved);
            if (!definition) {
                error_undefined(n, "type");
            }

            size_t definition_generics_count = 0;
            switch (definition->kind) {
            case NODE_TYPE: {
                NodeType *type = (NodeType *) definition;
                if (type->check_status != CHECK_STATUS_DONE) {
                    check_stmt(c, definition);
                }

                n->type = definition->type;
                definition_generics_count = type->generics_count;
            } break;

            case NODE_STRUCT: {
                NodeStruct *structt = (NodeStruct *) definition;
                if ((structt->check_status != CHECK_STATUS_DONE && need_full_definition) ||
                    (structt->check_status == CHECK_STATUS_TODO && structt->generics_count)) {
                    check_stmt(c, definition);
                }

                n->type = definition->type;
                definition_generics_count = structt->generics_count;
            } break;

            default:
                unreachable();
            }

            if (atom->generics.head) {
                if (!definition_generics_count) {
                    error_full(ERROR, n->token.pos, "Can only instantiate generic types");
                    exit(1);
                }

                if (atom->generics_count != definition_generics_count) {
                    error_full(
                        ERROR,
                        n->token.pos,
                        "Expected %zu generic parameter%s, got %zu",
                        definition_generics_count,
                        definition_generics_count == 1 ? "" : "s",
                        atom->generics_count);

                    exit(1);
                }

                for (Node *it = atom->generics.head; it; it = it->next) {
                    check_type(c, it, true, extra_generic_context);
                }

                n->type = instantiate_type(c, n->type, atom->generics.head, atom->generics_count);
            } else {
                if (definition_generics_count) {
                    error_full(ERROR, n->token.pos, "Cannot use generic type without instantiation");
                    exit(1);
                }
            }
        }
        break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        check_type(c, unary->operand, false, extra_generic_context);
        n->type = unary->operand->type;
        n->type.ref++;
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        check_type(c, index->base, true, extra_generic_context);

        if (index->from) {
            ConstValue count = eval_const_expr(c, index->from);
            type_assert_arith(index->from, false);

            if (!count.as.integer) {
                error_full(ERROR, index->from->token.pos, "Array cannot have zero items");
                exit(1);
            }

            n->type = (Type) {
                .kind = TYPE_ARRAY,
                .spec_type = &index->base->type,
                .spec_count = count.as.integer,
            };
        } else if (index->ranged) {
            n->type = (Type) {
                .kind = TYPE_DSLICE,
                .spec_type = &index->base->type,
            };
        } else {
            n->type = (Type) {
                .kind = TYPE_SLICE,
                .spec_type = &index->base->type,
            };
        }
    } break;

    case NODE_FN: {
        NodeFn *spec = (NodeFn *) n;
        for (Node *it = spec->args.head; it; it = it->next) {
            NodeVar *arg = (NodeVar *) it;
            check_type(c, arg->type, true, extra_generic_context);
            it->type = arg->type->type;
        }

        check_type(c, spec->ret, true, extra_generic_context);
        n->type = (Type) {.kind = TYPE_FN, .spec_node = n};
    } break;

    default:
        unreachable();
    }
}

static void check_fn(Compiler *c, Node *n);

static void check_if_expr(Compiler *c, NodeIf *iff) {
    check_expr(c, iff->condition, REF_NONE);
    type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});

    check_expr(c, iff->consequence, REF_NONE);
    check_expr(c, iff->antecedence, REF_NONE);
    iff->node.type = type_assert_node(c, iff->antecedence, iff->consequence);
}

static_assert(COUNT_TYPES == 18, "");
static void infer_generic_type(Type actual, Type expected, Node *generics) {
    switch (expected.kind) {
    case TYPE_UNIT:
    case TYPE_BOOL:
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
    case TYPE_INT:
    case TYPE_RAWPTR:
        // Pass
        break;

    case TYPE_FN:
        if (actual.ref == expected.ref && actual.kind == expected.kind) {
            NodeFn *actual_fn = (NodeFn *) actual.spec_node;
            assert(actual_fn);

            NodeFn *expected_fn = (NodeFn *) expected.spec_node;
            assert(expected_fn);

            for (Node *a = actual_fn->args.head, *e = expected_fn->args.head; a && e; a = a->next, e = e->next) {
                infer_generic_type(a->type, e->type, generics);
            }

            if (actual_fn->ret && expected_fn->ret) {
                infer_generic_type(actual_fn->ret->type, expected_fn->ret->type, generics);
            }
        }
        break;

    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_DSLICE:
        if (actual.ref == expected.ref && actual.kind == expected.kind) {
            assert(actual.spec_type);
            assert(expected.spec_type);
            infer_generic_type(*actual.spec_type, *expected.spec_type, generics);
        }
        break;

    case TYPE_STRUCT:
        if (actual.ref == expected.ref && actual.kind == expected.kind) {
            StructInstanace *a_instance = actual.spec_struct_instance;
            StructInstanace *e_instance = expected.spec_struct_instance;
            if (a_instance && e_instance && a_instance->definition == e_instance->definition) {
                for (Node *a = a_instance->generics, *e = e_instance->generics; a && e; a = a->next, e = e->next) {
                    infer_generic_type(a->type, e->type, generics);
                }
            }
        }
        break;

    case TYPE_GENERIC:
        if (actual.ref >= expected.ref) {
            actual.ref -= expected.ref;

            assert(expected.spec_node);
            const size_t n = expected.spec_node->token.as.integer;

            Node *instance = generics;
            for (size_t i = 0; i < n; i++) {
                assert(instance);
                instance = instance->next;
            }

            assert(instance);
            if (!instance->token.as.boolean) {
                instance->type = actual;
                instance->token.as.boolean = true;
            }
        }
        break;

    default:
        unreachable();
        break;
    }
}

static void check_generics(Compiler *c, Node *n, Node *generics_head, size_t generics_count, Node *definition) {
    bool *will_be_called = NULL;
    bool *generics_incomplete = NULL;
    if (n->kind == NODE_ATOM) {
        NodeAtom *atom = (NodeAtom *) n;
        will_be_called = &atom->will_be_called;
        generics_incomplete = &atom->generics_incomplete;
    } else if (n->kind == NODE_MEMBER) {
        NodeMember *member = (NodeMember *) n;
        will_be_called = &member->will_be_called;
        generics_incomplete = &member->generics_incomplete;
    } else {
        unreachable();
    }

    if (generics_head) {
        if (definition->kind != NODE_FN) {
            error_full(ERROR, n->token.pos, "Can only instantiate generic functions");
            exit(1);
        }

        const NodeFn *definition_fn = (const NodeFn *) definition;
        if (!definition_fn->generics.head) {
            error_full(ERROR, n->token.pos, "Can only instantiate generic functions");
            exit(1);
        }

        if (generics_count != definition_fn->generics_count) {
            if (*will_be_called && generics_count < definition_fn->generics_count) {
                *generics_incomplete = true;
            } else {
                error_full(
                    ERROR,
                    n->token.pos,
                    "Expected %zu generic parameter%s, got %zu",
                    definition_fn->generics_count,
                    definition_fn->generics_count == 1 ? "" : "s",
                    generics_count);

                exit(1);
            }
        }

        for (Node *it = generics_head; it; it = it->next) {
            if (it->kind == NODE_ATOM && it->token.kind == TOKEN_IDENT && sv_match(it->token.sv, "_")) {
                if (!*will_be_called) {
                    error_full(ERROR, it->token.pos, "Cannot infer generic types here");
                    exit(1);
                }

                *generics_incomplete = true;
            } else {
                check_type(c, it, true, NULL);
                it->token.as.boolean = true;
            }
        }

        if (!*generics_incomplete) {
            n->type = instantiate_type(c, n->type, generics_head, generics_count);
        }
    } else {
        if (definition->kind == NODE_FN && !*will_be_called) {
            const NodeFn *definition_fn = (const NodeFn *) definition;
            if (definition_fn->generics.head) {
                error_full(ERROR, n->token.pos, "Cannot use generic function without instantiation");
                exit(1);
            }
        }
    }
}

static_assert(COUNT_NODES == 23, "");
static void check_expr(Compiler *c, Node *n, RefKind ref) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_INT};
            break;

        case TOKEN_STR:
            n->type = c->context.str_type;
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_CHAR:
            n->type = (Type) {.kind = TYPE_U8};
            break;

        case TOKEN_IDENT: {
            resolve_ident_package(n);

            NodeAtom *atom = (NodeAtom *) n;
            atom->definition = ident_find(&c->context, atom->package, &n->token, false, atom->scope_resolved);
            if (!atom->definition) {
                error_undefined(n, "identifier");
            }

            if (c->context.checking_toplevels) {
                check_stmt(c, atom->definition);
            }
            n->type = atom->definition->type;

            check_generics(c, n, atom->generics.head, atom->generics_count, atom->definition);

            n->allow_ref = atom->definition->kind == NODE_VAR;
            if (ref) {
                if (atom->definition->kind == NODE_CONST && ref == REF_MUTATE) {
                    error_full(ERROR, n->token.pos, "Cannot mutate constant value");
                    exit(1);
                }

                if (n->allow_ref) {
                    NodeVar *var = (NodeVar *) atom->definition;
                    if (var->kind == NODE_VAR_ARG) {
                        var->kind = NODE_VAR_LOCAL;
                    }
                }
            }
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        check_expr(c, call->fn, REF_NONE);

        const Type *fn_type = &call->fn->type;
        if (fn_type->kind != TYPE_FN) {
            error_full(ERROR, call->fn->token.pos, "Cannot call type '%s'", type_to_cstr(*fn_type));
            exit(1);
        }

        if (fn_type->ref != 0) {
            error_full(
                ERROR,
                call->fn->token.pos,
                "Cannot call type '%s' without dereferencing it first",
                type_to_cstr(call->fn->type));

            exit(1);
        }

        bool is_method = false;

        const NodeFn *expected = (const NodeFn *) fn_type->spec_node;
        {
            size_t expected_arity = expected->arity;
            if (call->fn->kind == NODE_MEMBER) {
                NodeMember *member = (NodeMember *) call->fn;
                if (member->is_method) {
                    Node *self = member->lhs;
                    self->next = call->args.head;
                    call->args.head = self;

                    is_method = true;
                    expected_arity--;
                }
            }

            if (call->arity != expected_arity) {
                error_full(
                    ERROR,
                    n->token.pos,
                    "Expected %zu argument%s, got %zu",
                    expected_arity,
                    expected_arity == 1 ? "" : "s",
                    call->arity);

                exit(1);
            }
        }

        bool inferred = false;
        bool inferred_left = false;
        if (expected->generics_count) {
            Nodes  *generics = NULL;
            size_t *generics_count = NULL;
            bool   *generics_incomplete = NULL;

            if (call->fn->kind == NODE_ATOM) {
                NodeAtom *atom = (NodeAtom *) call->fn;
                generics = &atom->generics;
                generics_count = &atom->generics_count;
                generics_incomplete = &atom->generics_incomplete;
            } else if (call->fn->kind == NODE_MEMBER) {
                NodeMember *member = (NodeMember *) call->fn;
                generics = &member->generics;
                generics_count = &member->generics_count;
                generics_incomplete = &member->generics_incomplete;
            } else {
                unreachable();
            }

            if (!*generics_count || *generics_incomplete) {
                inferred = true;
                for (size_t i = *generics_count; i < expected->generics_count; i++) {
                    nodes_push(generics, arena_alloc(c->context.arena, sizeof(Node)));
                }
                *generics_count = expected->generics_count;

                for (Node *a = call->args.head, *e = expected->args.head; a; a = a->next, e = e->next) {
                    if (!is_method || a != call->args.head) {
                        check_expr(c, a, REF_NONE);
                    }
                    infer_generic_type(a->type, e->type, generics->head);
                }

                for (Node *it = generics->head; it; it = it->next) {
                    if (!it->token.as.boolean) {
                        inferred_left = true;
                        break;
                    }
                }

                call->fn->type = instantiate_type(c, call->fn->type, generics->head, *generics_count);
                expected = (const NodeFn *) fn_type->spec_node;
            }
        }

        for (Node *a = call->args.head, *e = expected->args.head; a; a = a->next, e = e->next) {
            if (!inferred && (!is_method || a != call->args.head)) {
                check_expr(c, a, REF_NONE);
            }

            type_assert(c, a, e->type);
        }

        if (inferred_left) {
            error_full(ERROR, n->token.pos, "Could not infer all the generic parameters, instance the call explicitly");
            fprintf(stderr, "\n");

            error_begin(NOTE, n->token.pos);

            Node *generics = NULL;
            if (call->fn->kind == NODE_ATOM) {
                generics = ((NodeAtom *) call->fn)->generics.head;
                fprintf(stderr, "Inferred ");
            } else if (call->fn->kind == NODE_MEMBER) {
                NodeMember *member = (NodeMember *) call->fn;
                generics = member->generics.head;
                fprintf(stderr, "Inferred (");
                write_message(stderr, MESSAGE_FG_YELLOW, "%s", type_to_cstr(member->lhs->type));
                fprintf(stderr, ").");
            } else {
                unreachable();
            }

            write_message(stderr, MESSAGE_FG_GREEN, SVFmt, SVArg(call->fn->token.sv));
            fprintf(stderr, "::<");
            for (Node *it = generics; it; it = it->next) {
                if (it->token.as.boolean) {
                    write_message(stderr, MESSAGE_FG_YELLOW, "%s", type_to_cstr(it->type));
                } else {
                    write_message(stderr, MESSAGE_FG_RED, "_");
                }

                if (it->next) {
                    fprintf(stderr, ", ");
                } else {
                    fprintf(stderr, ">\n");
                }
            }

            exit(1);
        }

        n->type = node_fn_return_type(expected);
    } break;

    case NODE_CAST: {
        NodeCast *cast = (NodeCast *) n;
        check_expr(c, cast->from, REF_NONE);
        check_type(c, cast->to, true, NULL);

        if (cast->from->type.kind == TYPE_SLICE && !cast->from->type.ref) {
            Type element_pointer = *cast->from->type.spec_type;
            element_pointer.ref++;

            if (!type_eq(cast->to->type, element_pointer)) {
                error_full(
                    ERROR,
                    n->token.pos,
                    "Can only cast '%s' to '%s', not '%s'",
                    type_to_cstr(cast->from->type),
                    type_to_cstr(element_pointer),
                    type_to_cstr(cast->to->type));

                exit(1);
            }

            cast->slice_lowering = true;
        } else {
            const Type from = type_assert_scalar(cast->from);
            const Type to = type_assert_scalar(cast->to);
            if (!type_eq(from, to) && is_type_cast_illegal(cast->from, cast->to)) {
                error_full(
                    ERROR, n->token.pos, "Cannot cast type '%s' to type '%s'", type_to_cstr(from), type_to_cstr(to));
                exit(1);
            }
        }

        n->type = cast->to->type;
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_expr(c, unary->operand, REF_NONE);
            n->type = type_assert_arith(unary->operand, false);
            break;

        case TOKEN_MUL:
            check_expr(c, unary->operand, REF_NONE);

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
            n->allow_ref = true;
            break;

        case TOKEN_BAND:
            check_expr(c, unary->operand, REF_ADDR);
            n->type = unary->operand->type;
            n->type.ref++;
            break;

        case TOKEN_BNOT:
            check_expr(c, unary->operand, REF_NONE);
            n->type = type_assert_arith(unary->operand, false);
            break;

        case TOKEN_LNOT:
            check_expr(c, unary->operand, REF_NONE);
            n->type = type_assert(c, unary->operand, (Type) {.kind = TYPE_BOOL});
            break;

        case TOKEN_LEN: {
            check_expr(c, unary->operand, REF_NONE);

            const Type operand = unary->operand->type;
            if (type_is_pointer(operand) ||
                (operand.kind != TYPE_SLICE && operand.kind != TYPE_DSLICE && operand.kind != TYPE_ARRAY)) {
                error_full(
                    ERROR, unary->operand->token.pos, "Expected slice or array type, got '%s'", type_to_cstr(operand));
                exit(1);
            }

            n->type = (Type) {.kind = TYPE_I64};
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        check_expr(c, index->base, ref);

        const Type base = index->base->type;
        if (index->ranged) {
            if (!base.ref && base.kind != TYPE_SLICE && base.kind != TYPE_DSLICE && base.kind != TYPE_ARRAY) {
                error_full(
                    ERROR,
                    index->base->token.pos,
                    "Expected typed pointer or slice or array, got '%s'",
                    type_to_cstr(base));

                exit(1);
            }

            if (base.ref && !index->to) {
                error_full(ERROR, index->base->token.pos, "Cannot infer range end of pointer type");
                exit(1);
            }
        } else {
            if (type_is_pointer(base) ||
                (base.kind != TYPE_SLICE && base.kind != TYPE_DSLICE && base.kind != TYPE_ARRAY)) {
                error_full(ERROR, index->base->token.pos, "Expected slice or array type, got '%s'", type_to_cstr(base));
                exit(1);
            }
        }

        if (index->from) {
            check_expr(c, index->from, REF_NONE);
            type_assert_arith(index->from, false);
        }

        if (index->ranged) {
            if (index->to) {
                check_expr(c, index->to, REF_NONE);
                type_assert_arith(index->to, false);
            }

            if (index->base->type.ref) {
                Type element = base;
                element.ref--;

                n->type = (Type) {
                    .kind = TYPE_SLICE,
                    .spec_type = arena_clone(c->context.arena, &element, sizeof(element)),
                };
            } else if (base.kind == TYPE_SLICE || base.kind == TYPE_DSLICE) {
                n->type = base;
                n->type.kind = TYPE_SLICE;
            } else if (base.kind == TYPE_ARRAY) {
                n->type = (Type) {
                    .kind = TYPE_SLICE,
                    .spec_type = base.spec_type,
                };
            } else {
                unreachable();
            }
        } else {
            n->type = *base.spec_type;
            n->allow_ref = true;
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert_arith(binary->lhs, true);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert_arith(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert_arith(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SET:
            check_expr(c, binary->lhs, REF_MUTATE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_ADD_SET:
        case TOKEN_SUB_SET:
            check_expr(c, binary->lhs, REF_MUTATE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert(c, binary->rhs, type_assert_arith(binary->lhs, true));
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_MUL_SET:
        case TOKEN_DIV_SET:
        case TOKEN_MOD_SET:
            check_expr(c, binary->lhs, REF_MUTATE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert(c, binary->rhs, type_assert_arith(binary->lhs, false));
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_SHL_SET:
        case TOKEN_SHR_SET:
        case TOKEN_BOR_SET:
        case TOKEN_BAND_SET:
            check_expr(c, binary->lhs, REF_MUTATE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert(c, binary->rhs, type_assert_arith(binary->lhs, false));
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_LOR:
        case TOKEN_LAND:
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            n->type = type_assert(c, binary->rhs, type_assert(c, binary->lhs, (Type) {.kind = TYPE_BOOL}));
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
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

        if (member->lhs->type.kind == TYPE_INT) {
            member->lhs->type.kind = TYPE_I64;
        }

        member->definition = (Node *) methods_find(&c->context.methods, member->lhs->type, n->token.sv);
        if (member->definition) {
            member->is_method = true;

            NodeFn *definition = (NodeFn *) member->definition;
            if (definition->package != member->package && !definition->is_public) {
                error_use_of_private(&n->token, member->definition, "method");
            }

            const size_t actual_ref = member->lhs->type.ref;
            const size_t expected_ref = definition->args.head->type.ref;
            if (actual_ref == expected_ref) {
                // OK
            } else if (actual_ref + 1 == expected_ref) {
                if (!member->lhs->allow_ref) {
                    error_full(ERROR, member->lhs->token.pos, "Cannot take reference to value not in memory");
                    exit(1);
                }

                NodeUnary *unary = arena_alloc(c->context.arena, sizeof(NodeUnary));
                unary->node.kind = NODE_UNARY;
                unary->node.token.kind = TOKEN_BAND;
                unary->node.token.sv = member->lhs->token.sv;
                unary->node.token.pos = member->lhs->token.pos;

                unary->operand = member->lhs;
                unary->node.type = member->lhs->type;
                unary->node.type.ref++;

                member->lhs = (Node *) unary;
            } else if (actual_ref > expected_ref) {
                NodeUnary *unary = arena_alloc(c->context.arena, sizeof(NodeUnary));
                unary->node.kind = NODE_UNARY;
                unary->node.token.kind = TOKEN_MUL;
                unary->node.token.sv = member->lhs->token.sv;
                unary->node.token.pos = member->lhs->token.pos;
                unary->node.token.as.integer = actual_ref - expected_ref;

                unary->operand = member->lhs;
                unary->node.type = member->lhs->type;
                unary->node.type.ref = expected_ref;

                member->lhs = (Node *) unary;
            } else {
                error_full(
                    ERROR,
                    n->token.pos,
                    "Method requires self to be '%s', got '%s'",
                    type_to_cstr(definition->args.head->type),
                    type_to_cstr(member->lhs->type));

                exit(1);
            }

            n->type = member->definition->type;
            check_generics(c, n, member->generics.head, member->generics_count, member->definition);
        } else if (member->lhs->type.kind == TYPE_STRUCT) {
            NodeStruct *structt = (NodeStruct *) member->lhs->type.spec_node;

            member->definition = nodes_find(structt->fields, n->token.sv, NULL);
            if (!member->definition) {
                error_undefined(n, "field or method");
            }

            n->allow_ref = true;
            n->type = member->definition->type;
        } else if (member->lhs->type.kind == TYPE_SLICE || member->lhs->type.kind == TYPE_DSLICE) {
            if (sv_match(n->token.sv, "data")) {
                n->type = *member->lhs->type.spec_type;
                n->type.ref++;
                n->token.as.integer = 0;
            } else if (sv_match(n->token.sv, "count")) {
                n->type = (Type) {.kind = TYPE_I64};
                n->token.as.integer = 8;
            } else if (sv_match(n->token.sv, "capacity") && member->lhs->type.kind == TYPE_DSLICE) {
                n->type = (Type) {.kind = TYPE_I64};
                n->token.as.integer = 16;
            } else {
                error_undefined(n, "field");
            }

            n->allow_ref = true;
        } else {
            error_undefined(n, "field or method");
        }
    } break;

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        check_type(c, sizeoff->type, true, NULL);
        check_expr(c, sizeoff->expr, REF_NONE);
        n->type = (Type) {.kind = TYPE_INT};
    } break;

    case NODE_COMPOUND: {
        NodeCompound *compound = (NodeCompound *) n;
        check_type(c, compound->type, true, NULL);

        n->type = compound->type->type;
        if (n->type.ref || (n->type.kind != TYPE_STRUCT && n->type.kind != TYPE_ARRAY && n->type.kind != TYPE_SLICE)) {
            error_full(
                ERROR,
                compound->type->token.pos,
                "Expected structure or array type, got '%s'",
                type_to_cstr(compound->type->type));

            exit(1);
        }

        // For structure literal
        NodeStruct *struct_spec = NULL;
        Node       *struct_fields_iota = NULL;
        if (n->type.kind == TYPE_STRUCT) {
            struct_spec = (NodeStruct *) n->type.spec_node;
            struct_fields_iota = struct_spec->fields.head;
        }

        // For array literal
        size_t array_items_count = 0;

        if (n->type.kind == TYPE_SLICE && !compound->nodes.head) {
            error_full(ERROR, n->token.pos, "Array cannot have zero items");
            exit(1);
        }

        for (Node *it = compound->nodes.head; it; it = it->next) {
            if (it->kind == NODE_BINARY && it->token.kind == TOKEN_COLON) {
                NodeBinary *assign = (NodeBinary *) it;

                Type expected = {0};
                if (n->type.kind == TYPE_STRUCT) {
                    if (assign->lhs->kind != NODE_ATOM || assign->lhs->token.kind != TOKEN_IDENT) {
                        error_full(ERROR, assign->lhs->token.pos, "Expected designated initializer to be field name");
                        exit(1);
                    }

                    NodeAtom *lhs = (NodeAtom *) assign->lhs;
                    lhs->definition = nodes_find(struct_spec->fields, lhs->node.token.sv, NULL);
                    if (!lhs->definition) {
                        error_undefined((Node *) lhs, "field");
                    }

                    expected = lhs->definition->type;
                } else {
                    const ConstValue index = eval_const_expr(c, assign->lhs);
                    type_assert_arith(assign->lhs, false);

                    if (n->type.kind == TYPE_ARRAY && index.as.integer >= n->type.spec_count) {
                        error_full(
                            ERROR,
                            assign->lhs->token.pos,
                            "Cannot assign to index %zu in array of length %zu",
                            index.as.integer,
                            n->type.spec_count);

                        exit(1);
                    } else {
                        array_items_count = max(array_items_count, index.as.integer + 1);
                    }

                    assign->lhs->token.as.integer = index.as.integer;
                    expected = *n->type.spec_type;
                }

                check_expr(c, assign->rhs, REF_NONE);
                type_assert(c, assign->rhs, expected);
            } else {
                if (n->type.kind == TYPE_STRUCT) {
                    if (!struct_fields_iota) {
                        error_full(ERROR, it->token.pos, "Too many ordered initializers");
                        exit(1);
                    }

                    check_expr(c, it, REF_NONE);
                    type_assert(c, it, struct_fields_iota->type);
                    struct_fields_iota = struct_fields_iota->next;
                } else {
                    if (n->type.kind == TYPE_ARRAY && array_items_count >= n->type.spec_count) {
                        error_full(ERROR, it->token.pos, "Too many ordered initializers");
                        exit(1);
                    }

                    array_items_count++;
                    check_expr(c, it, REF_NONE);
                    type_assert(c, it, *n->type.spec_type);
                }
            }
        }

        if (n->type.kind == TYPE_SLICE) {
            n->type.kind = TYPE_ARRAY;
            n->type.spec_count = array_items_count;
        }
    } break;

    case NODE_IF:
        check_if_expr(c, (NodeIf *) n);
        break;

    case NODE_FN:
        check_fn(c, n);
        break;

    default:
        unreachable();
    }

    if (!n->allow_ref && ref) {
        error_full(ERROR, n->token.pos, "Cannot take reference to value not in memory");
        exit(1);
    }
}

static void error_redefinition(const Node *n, const Node *previous, const char *label) {
    error_full(ERROR, n->token.pos, "Redefinition of %s '" SVFmt "'", label, SVArg(n->token.sv));
    if (previous) {
        fprintf(stderr, "\n");
        error_full(NOTE, previous->token.pos, "Defined here");
    }
    exit(1);
}

static_assert(COUNT_NODES == 23, "");
static bool loop_breaks(Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            if (loop_breaks(it)) {
                return true;
            }
        }
        return false;
    }

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        return loop_breaks(iff->consequence) || loop_breaks(iff->antecedence);
    }

    case NODE_JUMP:
        return n->token.kind == TOKEN_BREAK;

    default:
        return false;
    }
}

static_assert(COUNT_NODES == 23, "");
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
            return !loop_breaks(forr->body);
        }

        return false;
    }

    case NODE_RETURN:
        return true;

    default:
        return false;
    }
}

static void collect_extern_libraries(Compiler *c, NodeExtern *externn) {
    for (Node *it = externn->libraries.head; it; it = it->next) {
        const SV library = resolve_str_token(it->token, c->context.arena);
        da_push(&c->link_flags, "-l");
        da_push(&c->link_flags, library.data);
    }
}

static_assert(COUNT_NODES == 23, "");
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
                if (assertt->message) {
                    SV message = assertt->message->token.sv;
                    message.data++;
                    message.count -= 2;

                    resolve_escape_chars(temp_alloc(assertt->message->token.as.integer), &message);
                    fprintf(
                        stderr,
                        PosFmt " Assertion Failed: " SVFmt "\n",
                        PosArg(assertt->expr->token.pos),
                        SVArg(message));
                } else {
                    fprintf(stderr, PosFmt " Assertion Failed\n", PosArg(assertt->expr->token.pos));
                }

                exit(1);
            }
        } else {
            check_expr(c, assertt->expr, REF_NONE);
            type_assert(c, assertt->expr, (Type) {.kind = TYPE_BOOL});
        }
    } break;

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        if (iff->expr) {
            check_if_expr(c, iff);
        } else {
            check_expr(c, iff->condition, REF_NONE);
            type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});

            check_stmt(c, iff->consequence);
            check_stmt(c, iff->antecedence);
        }
    } break;

    case NODE_FOR: {
        NodeFor *forr = (NodeFor *) n;
        check_stmt(c, forr->init);

        if (forr->condition) {
            check_expr(c, forr->condition, REF_NONE);
            type_assert(c, forr->condition, (Type) {.kind = TYPE_BOOL});
        }

        check_expr(c, forr->update, REF_NONE);
        check_stmt(c, forr->body);
    } break;

    case NODE_BLOCK: {
        const size_t locals_count_save = c->context.locals.count;

        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }

        c->context.locals.count = locals_count_save;
    } break;

    case NODE_JUMP:
        // Pass
        break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;

        n->type = (Type) {.kind = TYPE_UNIT};
        if (ret->value) {
            check_expr(c, ret->value, REF_NONE);
            n->type = ret->value->type;
        }

        type_assert(c, n, node_fn_return_type(c->context.fn.fn));
    } break;

    case NODE_FN:
        check_fn(c, n);
        break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (var->check_status == CHECK_STATUS_DONE) {
            return;
        }

        if (var->kind == NODE_VAR_GLOBAL) {
            if (var->check_status == CHECK_STATUS_DOING) {
                error_full(ERROR, n->token.pos, "Reference loop");
                exit(1);
            }

            var->check_status = CHECK_STATUS_DOING;
        }

        if (var->type && var->type->type.kind == TYPE_UNIT) {
            check_type(c, var->type, true, NULL);
            n->type = var->type->type;
        }

        if (var->expr) {
            check_expr(c, var->expr, REF_NONE);
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
        if (type->check_status == CHECK_STATUS_DONE) {
            return;
        }

        if (!type->local) {
            if (type->check_status == CHECK_STATUS_DOING) {
                error_full(ERROR, n->token.pos, "Reference loop");
                exit(1);
            }

            type->check_status = CHECK_STATUS_DOING;
        }

        if (type->generics.head) {
            for (Node *it = type->generics.head; it; it = it->next) {
                Node *previous = nodes_find(type->generics, it->token.sv, it);
                if (previous) {
                    error_redefinition(it, previous, "generic parameter");
                }

                NodeType *type = (NodeType *) it;
                type->check_status = CHECK_STATUS_DONE;
                type->local = true;
                it->type = (Type) {.kind = TYPE_GENERIC, .spec_node = it};
            }
        }

        check_type(c, type->definition, true, type->generics.head);
        n->type = type->definition->type;

        type->check_status = CHECK_STATUS_DONE;
        if (type->local) {
            da_push(&c->context.locals, n);
        }
    } break;

    case NODE_CONST: {
        NodeConst *constt = (NodeConst *) n;
        if (constt->check_status == CHECK_STATUS_DONE) {
            return;
        }

        if (!constt->local) {
            if (constt->check_status == CHECK_STATUS_DOING) {
                error_full(ERROR, n->token.pos, "Reference loop");
                exit(1);
            }

            constt->check_status = CHECK_STATUS_DOING;
        }

        if (constt->type) {
            check_type(c, constt->type, true, NULL);
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
        if (structt->check_status == CHECK_STATUS_DONE) {
            return;
        }

        if (!structt->local) {
            if (structt->check_status == CHECK_STATUS_DOING) {
                error_full(ERROR, n->token.pos, "Reference loop");
                exit(1);
            }

            structt->check_status = CHECK_STATUS_DOING;
        }

        if (structt->generics.head) {
            for (Node *it = structt->generics.head; it; it = it->next) {
                Node *previous = nodes_find(structt->generics, it->token.sv, it);
                if (previous) {
                    error_redefinition(it, previous, "generic parameter");
                }

                NodeType *type = (NodeType *) it;
                type->check_status = CHECK_STATUS_DONE;
                type->local = true;
                it->type = (Type) {.kind = TYPE_GENERIC, .spec_node = it};
            }
        }

        for (Node *it = structt->fields.head; it; it = it->next) {
            const Node *previous = nodes_find(structt->fields, it->token.sv, it);
            if (previous) {
                error_redefinition(it, previous, "field");
            }

            NodeField *field = (NodeField *) it;
            check_type(c, field->type, true, structt->generics.head);

            it->type = field->type->type;
            if (it->type.kind == TYPE_UNIT) {
                error_full(ERROR, it->token.pos, "Cannot define field with type '%s'", type_to_cstr(it->type));
                exit(1);
            }
        }

        n->type = (Type) {.kind = TYPE_STRUCT, .spec_node = n};

        structt->check_status = CHECK_STATUS_DONE;
        if (structt->local) {
            da_push(&c->context.locals, n);
        }
    } break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        c->context.in_extern = true;
        for (Node *it = externn->definitions.head; it; it = it->next) {
            check_stmt(c, it);
        }
        c->context.in_extern = false;

        collect_extern_libraries(c, externn);
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        check_expr(c, print->operand, REF_NONE);
        type_assert_scalar(print->operand);
    } break;

    default:
        check_expr(c, n, REF_NONE);
        break;
    }
}

static_assert(COUNT_NODES == 23, "");
static void define_toplevel(Node *n) {
    switch (n->kind) {
    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        if (!fn->is_method) {
            Package *package = fn->package;

            const Node *previous = scope_find(package->globals, n->token.sv, false);
            if (previous) {
                error_redefinition(n, previous, "identifier");
            }

            da_push(&package->globals, n);
        }
    } break;

    case NODE_VAR: {
        Package *package = ((NodeVar *) n)->package;

        const Node *previous = scope_find(package->globals, n->token.sv, false);
        if (previous) {
            error_redefinition(n, previous, "identifier");
        }

        da_push(&package->globals, n);
    } break;

    case NODE_CONST: {
        Package *package = ((NodeConst *) n)->package;

        const Node *previous = scope_find(package->globals, n->token.sv, false);
        if (previous) {
            error_redefinition(n, previous, "identifier");
        }

        da_push(&package->globals, n);
    } break;

    case NODE_TYPE: {
        Package *package = ((NodeType *) n)->package;

        const Node *previous = scope_find(package->globals, n->token.sv, true);
        if (previous) {
            error_redefinition(n, previous, "type");
        }

        da_push(&package->globals, n);
    } break;

    case NODE_STRUCT: {
        Package *package = ((NodeStruct *) n)->package;

        const Node *previous = scope_find(package->globals, n->token.sv, true);
        if (previous) {
            error_redefinition(n, previous, "type");
        }

        n->type = (Type) {.kind = TYPE_STRUCT, .spec_node = n};
        da_push(&package->globals, n);
    } break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        for (Node *it = externn->definitions.head; it; it = it->next) {
            define_toplevel(it);
        }
    } break;

    case NODE_ASSERT:
        // Pass
        break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 23, "");
static void check_toplevel(Compiler *c, Node *n) {
    switch (n->kind) {
    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        if (fn->check_status == CHECK_STATUS_DONE) {
            return;
        }

        if (fn->check_status == CHECK_STATUS_DOING) {
            error_full(ERROR, n->token.pos, "Reference loop");
            exit(1);
        }

        fn->check_status = CHECK_STATUS_DOING;
        n->type = (Type) {.kind = TYPE_FN, .spec_node = n};

        if (fn->generics.head) {
            for (Node *it = fn->generics.head; it; it = it->next) {
                Node *previous = nodes_find(fn->generics, it->token.sv, it);
                if (previous) {
                    error_redefinition(it, previous, "generic parameter");
                }

                NodeType *type = (NodeType *) it;
                type->check_status = CHECK_STATUS_DONE;
                type->local = true;
                it->type = (Type) {.kind = TYPE_GENERIC, .spec_node = it};
            }
        }

        for (Node *it = fn->args.head; it; it = it->next) {
            if (it->token.kind == TOKEN_IDENT) {
                const Node *previous = nodes_find(fn->args, it->token.sv, it);
                if (previous) {
                    error_redefinition(it, previous, "argument");
                }
            }

            NodeVar *var = (NodeVar *) it;
            assert(var->type);
            check_type(c, var->type, true, fn->generics.head);
            it->type = var->type->type;
        }

        if (fn->is_method) {
            Type  self = fn->args.head->type;
            Node *previous = (Node *) methods_push(&c->context.methods, self, fn, c->context.arena);
            if (previous) {
                error_redefinition(n, previous, "method");
            } else if (self.kind == TYPE_STRUCT) {
                previous = nodes_find(((NodeStruct *) self.spec_node)->fields, n->token.sv, NULL);
                if (previous) {
                    error_redefinition(n, previous, "field");
                }
            } else if (self.kind == TYPE_SLICE) {
                if (sv_match(n->token.sv, "data") || sv_match(n->token.sv, "count")) {
                    error_redefinition(n, NULL, "field");
                }
            } else if (self.kind == TYPE_DSLICE) {
                if (sv_match(n->token.sv, "data") || sv_match(n->token.sv, "count") ||
                    sv_match(n->token.sv, "capacity")) {
                    error_redefinition(n, NULL, "field");
                }
            }
        }

        check_type(c, fn->ret, true, fn->generics.head);
        fn->check_status = CHECK_STATUS_DONE;
    } break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        for (Node *it = externn->definitions.head; it; it = it->next) {
            check_toplevel(c, it);
        }

        collect_extern_libraries(c, externn);
    } break;

    default:
        check_stmt(c, n);
        break;
    }
}

static void check_fn(Compiler *c, Node *n) {
    if (c->context.checking_toplevels) {
        check_toplevel(c, n);
        return;
    }

    NodeFn *fn = (NodeFn *) n;
    if (fn->local) {
        da_push(&c->context.locals, n);
    }
    n->type = (Type) {.kind = TYPE_FN, .spec_node = n};

    const ContextFn context_fn_save = context_fn_begin(&c->context, fn);
    for (Node *it = fn->generics.head; it; it = it->next) {
        if (fn->local) {
            Node *previous = nodes_find(fn->generics, it->token.sv, it);
            if (previous) {
                error_redefinition(it, previous, "generic parameter");
            }
        }
        da_push(&c->context.locals, it);
    }

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
        check_type(c, fn->ret, true, NULL);
    }

    if (fn->body) {
        check_stmt(c, fn->body);
        if (fn->ret && !always_returns(fn->body)) {
            assert(fn->body->kind == NODE_BLOCK);
            error_full(ERROR, ((NodeBlock *) fn->body)->rbrace_pos, "Expected return statement");
            exit(1);
        }
    }

    context_fn_end(&c->context, context_fn_save);
}

void check_packages(Compiler *c, Packages ps) {
    assert(c->context.arena);

    const Type u8_type = {.kind = TYPE_U8};
    c->context.str_type = (Type) {
        .kind = TYPE_SLICE,
        .spec_type = arena_clone(c->context.arena, &u8_type, sizeof(u8_type)),
    };

    for (Package *p = ps.head; p; p = p->next) {
        for (Node *it = p->nodes.head; it; it = it->next) {
            define_toplevel(it);
        }
    }

    c->context.checking_toplevels = true;
    for (Package *p = ps.head; p; p = p->next) {
        for (Node *it = p->nodes.head; it; it = it->next) {
            check_toplevel(c, it);
        }
    }
    c->context.checking_toplevels = false;

    for (Package *p = ps.head; p; p = p->next) {
        for (Node *it = p->nodes.head; it; it = it->next) {
            if (it->kind == NODE_FN) {
                check_stmt(c, it);
            }
        }
    }
}
