#include "checker.h"
#include "basic.h"
#include "context.h"
#include "node.h"
#include "token.h"

static void error_undefined(const Token *t, const char *label) {
    fprintf(stderr, Pos_Fmt "ERROR: Undefined %s '" SV_Fmt "'\n", Pos_Arg(t->pos), label, SV_Arg(t->sv));
    exit(1);
}

static void error_redefinition(const Node_Atom *n, const Pos *previous) {
    fprintf(
        stderr, Pos_Fmt "ERROR: Redefinition of '" SV_Fmt "'\n", Pos_Arg(n->node.token.pos), SV_Arg(n->node.token.sv));
    if (previous) {
        fprintf(stderr, Pos_Fmt "NOTE: Defined here\n", Pos_Arg(*previous));
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

static void check_int_limit(Node *n, size_t value) {
    static_assert(COUNT_TYPES == 17, "");
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
        fprintf(
            stderr,
            Pos_Fmt "ERROR: Number '%zu' is too large for %s\n",
            Pos_Arg(n->token.pos),
            value,
            type_to_cstr(n->type));
        exit(1);
    }
}

static_assert(COUNT_NODES == 18, "");
static void cast_untyped(Compiler *c, Node *n, Type expected) {
    switch (n->kind) {
    case NODE_ATOM:
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = expected;
            check_int_limit(n, n->token.as.integer);
            break;

        case TOKEN_IDENT: {
            Node_Atom *atom = (Node_Atom *) n;
            assert(atom->definition->is_const); // Only constants can be defined as untyped int

            n->type = expected;
            check_int_limit(n, atom->definition->const_value.as.integer);
        } break;

        default:
            unreachable();
        }
        break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        n->type = expected;
        if (n->token.kind == TOKEN_SIZEOF) {
            check_int_limit(n, compile_sizeof(c, &unary->value->type));
        } else {
            cast_untyped(c, unary->value, expected);
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        cast_untyped(c, binary->lhs, expected);
        cast_untyped(c, binary->rhs, expected);
        n->type = expected;
    } break;

    case NODE_RETURN: {
        Node_Return *ret = (Node_Return *) n;
        cast_untyped(c, ret->value, expected);
        n->type = ret->value->type;
    } break;

    default:
        unreachable();
    }
}

static bool try_auto_cast_untyped(Compiler *c, Node *n, Type expected) {
    if (type_is_integer(expected) && type_kind_eq(n->type, TYPE_INT)) {
        if (!type_kind_eq(expected, TYPE_INT)) {
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

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected %s, got %s\n",
        Pos_Arg(n->token.pos),
        type_to_cstr(expected),
        type_to_cstr(n->type));

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

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected %s, got %s\n",
        Pos_Arg(a->token.pos),
        type_to_cstr(b->type),
        type_to_cstr(a->type));

    exit(1);
}

static Type type_assert_numeric(const Node *n, bool pointers_allowed) {
    if (type_is_numeric(n->type)) {
        return n->type;
    }

    if (type_is_pointer(n->type) && pointers_allowed) {
        return n->type;
    }

    const char *label = "arithmetic";
    if (!pointers_allowed) {
        label = "numeric";
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected %s value, got %s\n", Pos_Arg(n->token.pos), label, type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_scalar(const Node *n) {
    if (type_is_scalar(n->type)) {
        return n->type;
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected scalar value, got %s\n", Pos_Arg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_type(const Node *n) {
    if (n->type.is_meta) {
        return n->type;
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected a type, got %s\n", Pos_Arg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static bool get_builtin_type_kind(SV name, Type_Kind *kind) {
    static_assert(COUNT_TYPES == 17, "");
    static const char *names[COUNT_TYPES] = {
        [TYPE_BOOL] = "bool",
        [TYPE_CHAR] = "char",

        [TYPE_I8] = "i8",
        [TYPE_I16] = "i16",
        [TYPE_I32] = "i32",
        [TYPE_I64] = "i64",

        [TYPE_U8] = "u8",
        [TYPE_U16] = "u16",
        [TYPE_U32] = "u32",
        [TYPE_U64] = "u64",

        [TYPE_RAWPTR] = "rawptr",

        [TYPE_STRING] = "string",
    };

    for (Type_Kind k = 0; k < len(names); k++) {
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

static void node_finalize_type_of_untyped(Node *n) {
    if (type_kind_eq(n->type, TYPE_INT)) {
        n->type.kind = TYPE_I64;
    }
}

static_assert(COUNT_NODES == 18, "");
static bool loop_breaks(Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            if (loop_breaks(it)) {
                return true;
            }
        }
        return false;
    }

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        return loop_breaks(iff->consequence) || loop_breaks(iff->antecedence);
    }

    case NODE_JUMP:
        return n->token.kind == TOKEN_BREAK;

    default:
        return false;
    }
}

static bool is_atom_true(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_BOOL && n->token.as.integer;
}

static bool is_atom_false(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_BOOL && !n->token.as.integer;
}

static_assert(COUNT_NODES == 18, "");
static bool always_returns(Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            if (always_returns(it)) {
                return true;
            }
        }
        return false;
    }

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
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

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;
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

    case NODE_RETURN:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_NODES == 18, "");
static Const_Value eval_const_expr(Compiler *c, Node *n) {
    if (!n) {
        return (Const_Value) {0};
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;

        static_assert(COUNT_TOKENS == 46, "");
        switch (n->token.kind) {
        case TOKEN_INT:
        case TOKEN_BOOL:
        case TOKEN_CHAR:
            return const_value_int(n->token.as.integer);

        case TOKEN_IDENT: {
            if (n->type.is_meta) {
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

        case TOKEN_STRING:
            return const_value_string(n->token.sv);

        default:
            unreachable();
            break;
        }
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        Const_Value value = {0};

        static_assert(COUNT_TOKENS == 46, "");
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

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        Const_Value  lhs = {0};
        Const_Value  rhs = {0};

        static_assert(COUNT_TOKENS == 46, "");
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
            if (rhs.as.integer == 0) {
                fprintf(stderr, Pos_Fmt "ERROR: Cannot divide by zero\n", Pos_Arg(binary->rhs->token.pos));
                exit(1);
            }
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

    case NODE_MEMBER: {
        Node_Member      *member = (Node_Member *) n;
        const Const_Value lhs = eval_const_expr(c, member->lhs);

        // TODO(@slice)
        switch (lhs.kind) {
        case CONST_VALUE_STRUCT:
            return lhs.as.structt.fields[member->field_index];

        case CONST_VALUE_STRING:
            if (member->field_index == 0) {
                // TODO: Pointers in constant expressions
                todo();
            } else if (member->field_index == 1) {
                return const_value_int(lhs.as.string.count);
            } else {
                unreachable();
            }

        default:
            unreachable();
        }
    }

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;
        if (fn->is_type) {
            return const_value_type(fn->node.type);
        } else {
            return const_value_fn(fn);
        }
    }

    case NODE_STRUCT:
        return const_value_type(n->type);

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;

        Const_Value_Struct struct_value = {0};
        if (n->type.kind == TYPE_STRUCT) {
            struct_value.spec = n->type.spec.structt;
            struct_value.fields = arena_alloc(c->arena, struct_value.spec->fields_count * sizeof(*struct_value.fields));
        }

        size_t ordered_iota = 0;
        for (Node *iter = compound->children.head; iter; iter = iter->next) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            Node *it = iter;
            if (n->type.kind == TYPE_STRUCT) {
                if (compound->is_designated) {
                    assert(it->kind == NODE_BINARY && it->token.kind == TOKEN_SET);
                    Node_Binary *it_binary = (Node_Binary *) it;
                    it_iota = it->token.as.integer;
                    it = it_binary->rhs;
                }

                struct_value.fields[it_iota] = eval_const_expr(c, it);
            } else {
                unreachable();
            }
        }

        return const_value_struct(struct_value);
    }

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
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

    case NODE_SLICE:
        return const_value_type(n->type);

    case NODE_INDEX: {
        Node_Index       *index = (Node_Index *) n;
        const Const_Value lhs = eval_const_expr(c, index->lhs);
        if (index->is_ranged) {
            // TODO(@slice)
            switch (lhs.kind) {
            case CONST_VALUE_STRING: {
                SV sv = lhs.as.string;

                i64 begin = 0;
                if (index->a) {
                    begin = eval_const_expr(c, index->a).as.integer;
                }

                i64 end = sv.count;
                if (index->b) {
                    end = eval_const_expr(c, index->b).as.integer;
                }

                if (begin > end) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Range (%lld..%lld) is invalid: Beginning of range is more than end\n",
                        Pos_Arg(n->token.pos),
                        begin,
                        end);
                    exit(1);
                }

                if (begin < 0 || end < 0 || (size_t) begin > sv.count || (size_t) end > sv.count) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Range (%lld..%lld) is out of bounds in string of length %zu\n",
                        Pos_Arg(n->token.pos),
                        begin,
                        end,
                        sv.count);
                    exit(1);
                }

                sv.data += begin;
                sv.count = end - begin;
                return const_value_string(sv);
            }

            default:
                unreachable();
            }
        } else {
            const i64 at = eval_const_expr(c, index->a).as.integer;

            // TODO(@slice)
            switch (lhs.kind) {
            case CONST_VALUE_STRING: {
                if (at < 0 || (size_t) at >= lhs.as.string.count) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Index %lld is out of bounds in string of length %zu\n",
                        Pos_Arg(n->token.pos),
                        at,
                        lhs.as.string.count);
                    exit(1);
                };

                return const_value_int(lhs.as.string.data[at]);
            }

            default:
                unreachable();
            }
        }
    }

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_NODES == 18, "");
static void define_orderless_nodes(Compiler *c, Node *n, const size_t block_start) {
    switch (n->kind) {
    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        assert(define->name->kind == NODE_ATOM && define->name->token.kind == TOKEN_IDENT);

        Node_Atom *it = (Node_Atom *) define->name;
        if (!sv_match(it->node.token.sv, "_")) {
            if (it->is_local) {
                if (it->is_const) {
                    const Context_Fn *fn = c->context.current;

                    assert(fn->end <= c->context.locals.count);
                    assert(block_start <= c->context.locals.count);
                    assert(block_start <= fn->end);
                    for (size_t i = fn->end; i > block_start; i--) {
                        Node_Atom *previous = c->context.locals.data[i - 1];
                        if (!previous->is_const) {
                            continue;
                        }

                        if (sv_eq(it->node.token.sv, previous->node.token.sv)) {
                            error_redefinition(it, &previous->node.token.pos);
                            break;
                        }
                    }

                    it->context = c->context.current;
                    context_push_local(&c->context, it);
                }
            } else {
                if (get_builtin_type_kind(it->node.token.sv, NULL)) {
                    error_redefinition(it, NULL);
                }

                Node_Atom *previous = scope_find(c->globals, it->node.token.sv);
                if (previous) {
                    error_redefinition(it, &previous->node.token.pos);
                }

                da_push(&c->globals, it);
            }
        }
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            define_orderless_nodes(c, it, block_start);
        }
    } break;

    default:
        // Pass
        break;
    }
}

typedef enum {
    REF_NONE,
    REF_ADDR,
    REF_ASSIGN,
} Ref_Kind;

static void check_node(Compiler *c, Node *n, Ref_Kind ref);

static void check_definition(Compiler *c, Node_Atom *it, Node *type, Node *it_expr) {
    assert(it->check_status != CHECKING); // It is already checked
    if (it->check_status == CHECKED) {
        return;
    }
    it->check_status = CHECKING;

    if (type) {
        check_node(c, type, REF_NONE);
        it->node.type = type_assert_type(type);
        it->node.type.is_meta = false;
    }

    if (it_expr) {
        check_node(c, it_expr, REF_NONE);

        if (type_kind_eq(it_expr->type, TYPE_UNIT) || (it_expr->type.is_meta && !it->is_const)) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot store %s in a %s\n",
                Pos_Arg(it_expr->token.pos),
                type_to_cstr(it_expr->type),
                it->is_const ? "constant" : "variable");

            exit(1);
        }

        if (type) {
            type_assert(c, it_expr, it->node.type);
        } else {
            if (!it->is_const) {
                node_finalize_type_of_untyped(it_expr);
            }
            it->node.type = it_expr->type;
        }
    }

    if (it->is_const) {
        it->const_value = eval_const_expr(c, it_expr);
    } else if (!it->is_local && it_expr) {
        it->const_value = eval_const_expr(c, it_expr);
    }

    if (it->is_local) {
        if (!it->is_const && !sv_match(it->node.token.sv, "_")) {
            context_push_local(&c->context, it);
        }
    }

    it->check_status = CHECKED;
}

static void check_ident(Compiler *c, Node *n, Ref_Kind ref) {
    Node_Atom *atom = (Node_Atom *) n;
    if (sv_match(n->token.sv, "_")) {
        fprintf(stderr, Pos_Fmt "ERROR: Identifier '_' cannot be used as a value\n", Pos_Arg(n->token.pos));
        exit(1);
    }

    Node_Atom *definition = context_find_local(&c->context, n->token.sv);
    if (!definition) {
        definition = scope_find(c->globals, n->token.sv);
    }
    atom->definition = definition;

    if (definition) {
        switch (definition->check_status) {
        case UNCHECKED: {
            Context_Fn *context_fn_save = c->context.current;
            c->context.current = definition->context;

            // Only orderless definitions can be uninffered, and the assignment of such definitions must be constant
            check_definition(c, definition, definition->definition_node->type, definition->assignment_node);
            context_restore_fn(&c->context, context_fn_save);
        } break;

        case CHECKING:
            fprintf(stderr, Pos_Fmt "ERROR: Cyclic definition\n", Pos_Arg(definition->node.token.pos));
            exit(1);
            break;

        case CHECKED:
            // Pass
            break;
        }

        n->type = definition->node.type;
        if (definition->is_const) {
            switch (ref) {
            case REF_NONE:
                // OK
                break;

            case REF_ADDR:
                if (!n->type.is_meta) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot take reference to compile time constant value\n",
                        Pos_Arg(n->token.pos));
                    exit(1);
                }
                break;

            case REF_ASSIGN:
                fprintf(stderr, Pos_Fmt "ERROR: Cannot assign to compile time constant value\n", Pos_Arg(n->token.pos));
                exit(1);
                break;
            }
        }
        return;
    }

    Type_Kind kind;
    if (get_builtin_type_kind(n->token.sv, &kind)) {
        n->type = (Type) {.kind = kind, .is_meta = true};
        return;
    }

    error_undefined(&n->token, "identifier");
}

static_assert(COUNT_NODES == 18, "");
static void check_node(Compiler *c, Node *n, Ref_Kind ref) {
    if (!n) {
        return;
    }

    bool is_ref_valid = false;
    switch (n->kind) {
    case NODE_ATOM: {
        static_assert(COUNT_TOKENS == 46, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_INT};
            break;

        case TOKEN_CHAR:
            n->type = (Type) {.kind = TYPE_CHAR};
            break;

        case TOKEN_IDENT:
            check_ident(c, n, ref);
            is_ref_valid = true; // check_ident() has already checked whether the reference is valid
            break;

        case TOKEN_STRING:
            n->type = (Type) {.kind = TYPE_STRING};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        static_assert(COUNT_TOKENS == 46, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_node(c, unary->value, REF_NONE);
            n->type = type_assert_numeric(unary->value, false);
            break;

        case TOKEN_MUL:
            check_node(c, unary->value, REF_NONE);
            if (!unary->value->type.ref) {
                if (type_kind_eq(unary->value->type, TYPE_RAWPTR)) {
                    fprintf(
                        stderr, Pos_Fmt "ERROR: Cannot dereference raw pointer\n", Pos_Arg(unary->value->token.pos));
                    exit(1);
                }

                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Expected typed pointer, got %s\n",
                    Pos_Arg(unary->value->token.pos),
                    type_to_cstr(unary->value->type));
                exit(1);
            }

            n->type = unary->value->type;
            n->type.ref--;
            is_ref_valid = true;
            break;

        case TOKEN_BAND:
            check_node(c, unary->value, REF_ADDR);
            n->type = unary->value->type;
            n->type.ref++;
            break;

        case TOKEN_BNOT:
            check_node(c, unary->value, REF_NONE);
            n->type = type_assert_numeric(unary->value, false);
            break;

        case TOKEN_LNOT:
            check_node(c, unary->value, REF_NONE);
            n->type = type_assert(c, unary->value, (Type) {.kind = TYPE_BOOL});
            break;

        case TOKEN_SIZEOF:
            check_node(c, unary->value, REF_NONE);
            n->type = (Type) {.kind = TYPE_INT};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        static_assert(COUNT_TOKENS == 46, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
            check_node(c, binary->lhs, REF_NONE);
            check_node(c, binary->rhs, REF_NONE);
            type_assert_numeric(binary->lhs, true);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
            check_node(c, binary->lhs, REF_NONE);
            check_node(c, binary->rhs, REF_NONE);
            type_assert_numeric(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
            check_node(c, binary->lhs, REF_NONE);
            check_node(c, binary->rhs, REF_NONE);
            type_assert_numeric(binary->lhs, false);
            n->type = type_assert_node(c, binary->rhs, binary->lhs);
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            check_node(c, binary->lhs, REF_NONE);
            check_node(c, binary->rhs, REF_NONE);
            type_assert_numeric(binary->lhs, true);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_SET:
            check_node(c, binary->lhs, REF_ASSIGN);
            check_node(c, binary->rhs, REF_NONE);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;

        check_node(c, member->lhs, ref);
        is_ref_valid = true; // check_node() has already determined that the reference is valid

        if (type_kind_eq(member->lhs->type, TYPE_STRUCT)) {
            Type_Struct_Field *definition = NULL;

            Type_Struct *spec = member->lhs->type.spec.structt;
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];
                if (sv_eq(it->name, member->field.sv)) {
                    definition = it;
                    member->field_index = i;
                    break;
                }
            }

            if (!definition) {
                error_undefined(&member->field, "field");
            }

            n->type = definition->type;
        } else if (type_kind_eq(member->lhs->type, TYPE_SLICE)) {
            if (sv_match(member->field.sv, "data")) {
                n->type = *member->lhs->type.spec.slice.element;
                n->type.ref++;
                member->field_index = 0;
            } else if (sv_match(member->field.sv, "count")) {
                n->type = (Type) {.kind = TYPE_I64};
                member->field_index = 1;
            } else {
                error_undefined(&member->field, "field");
            }
        } else if (type_kind_eq(member->lhs->type, TYPE_STRING)) {
            if (sv_match(member->field.sv, "data")) {
                n->type = (Type) {.kind = TYPE_CHAR, .ref = 1};
                member->field_index = 0;
            } else if (sv_match(member->field.sv, "count")) {
                n->type = (Type) {.kind = TYPE_I64};
                member->field_index = 1;
            } else {
                error_undefined(&member->field, "field");
            }
        } else {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot access field of %s\n",
                Pos_Arg(n->token.pos),
                type_to_cstr(member->lhs->type));
            exit(1);
        }
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;

        Context_Fn context_fn = {.fn = fn, .outer = c->context.current};
        context_push_fn(&c->context, &context_fn);

        {
            Type_Fn fn_type_spec = {
                .args = arena_alloc(c->arena, fn->args_count * sizeof(*fn_type_spec.args)),
            };

            for (Node *arg = fn->args.head; arg; arg = arg->next) {
                assert(arg->kind == NODE_DEFINE);
                Node_Define *define = (Node_Define *) arg;

                assert(define->name->kind == NODE_ATOM);
                Node_Atom *it = (Node_Atom *) define->name;
                if (!sv_match(it->node.token.sv, "_")) {
                    for (size_t i = 0; i < fn_type_spec.args_count; i++) {
                        Type_Fn_Arg previous = fn_type_spec.args[i];
                        if (sv_eq(previous.name, it->node.token.sv)) {
                            error_redefinition(it, &previous.pos);
                        }
                    }
                }

                Type_Fn_Arg *arg_spec = &fn_type_spec.args[fn_type_spec.args_count++];
                *arg_spec = (Type_Fn_Arg) {
                    .name = it->node.token.sv,
                    .pos = it->node.token.pos,
                };

                check_node(c, arg, REF_NONE);
                arg_spec->type = it->node.type;
            }

            if (fn->returnn) {
                check_node(c, fn->returnn, REF_NONE);
                type_assert_type(fn->returnn);
                fn_type_spec.returnn = arena_clone(c->arena, &fn->returnn->type, sizeof(Type));
                fn_type_spec.returnn->is_meta = false;
            } else {
                fn_type_spec.returnn = arena_alloc(c->arena, sizeof(Type));
                fn_type_spec.returnn->kind = TYPE_UNIT;
            }

            n->type = (Type) {.kind = TYPE_FN, .spec.fn = fn_type_spec};

            if (fn->defined_as) {
                // The body of a function is irrelevant for outer expressions
                fn->defined_as->node.type = n->type;
                fn->defined_as->check_status = CHECKED;
            }

            if (fn->is_type) {
                n->type.is_meta = true;
                is_ref_valid = ref == REF_ADDR;
            } else if (fn->body) {
                check_node(c, fn->body, REF_NONE);
                if (fn->returnn && !always_returns(fn->body)) {
                    assert(fn->body->kind == NODE_BLOCK);
                    const Pos end = ((Node_Block *) fn->body)->end;
                    fprintf(stderr, Pos_Fmt "ERROR: Expected return statement\n", Pos_Arg(end));
                    exit(1);
                }
            }
        }

        context_pop_fn(&c->context);
    } break;

    case NODE_STRUCT: {
        Node_Struct *structt = (Node_Struct *) n;

        const Type_Struct structt_type_spec = {
            .fields = arena_alloc(c->arena, structt->fields_count * sizeof(*structt_type_spec.fields)),
            .fields_count = structt->fields_count,
            .definition = structt,
        };

        n->type = (Type) {
            .kind = TYPE_STRUCT,
            .is_meta = true,
            .spec.structt = arena_clone(c->arena, &structt_type_spec, sizeof(structt_type_spec)),
        };

        size_t iota = 0;
        for (Node *field = structt->fields.head; field; field = field->next) {
            assert(field->kind == NODE_DEFINE);
            Node_Define *define = (Node_Define *) field;

            assert(define->name->kind == NODE_ATOM);
            Node_Atom *it = (Node_Atom *) define->name;
            if (!sv_match(it->node.token.sv, "_")) {
                for (size_t i = 0; i < iota; i++) {
                    Type_Struct_Field previous = structt_type_spec.fields[i];
                    if (sv_eq(previous.name, it->node.token.sv)) {
                        error_redefinition(it, &previous.pos);
                    }
                }
            }

            Type_Struct_Field *it_spec = &structt_type_spec.fields[iota++];
            *it_spec = (Type_Struct_Field) {
                .name = it->node.token.sv,
                .pos = it->node.token.pos,
            };

            check_node(c, define->type, REF_NONE);
            it->node.type = type_assert_type(define->type);
            it->node.type.is_meta = false;
            it_spec->type = it->node.type;
        }

        is_ref_valid = ref == REF_ADDR;
    } break;

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        check_node(c, compound->lhs, REF_NONE);
        type_assert_type(compound->lhs);

        n->type = compound->lhs->type;
        n->type.is_meta = false;
        if (n->type.ref || (n->type.kind != TYPE_STRUCT)) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Expected structure type, got %s\n",
                Pos_Arg(compound->lhs->token.pos),
                type_to_cstr(n->type));
            exit(1);
        }

        // For structure literal
        Type_Struct *struct_spec = NULL;
        if (n->type.kind == TYPE_STRUCT) {
            struct_spec = n->type.spec.structt;
        }

        size_t ordered_iota = 0;
        for (Node *iter = compound->children.head; iter; iter = iter->next) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            Node *it = iter;
            if (n->type.kind == TYPE_STRUCT) {
                if (compound->is_designated) {
                    assert(it->kind == NODE_BINARY && it->token.kind == TOKEN_SET);
                    Node_Binary *it_binary = (Node_Binary *) it;

                    if (it_binary->lhs->kind != NODE_ATOM || it_binary->lhs->token.kind != TOKEN_IDENT) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Expected designated initializer to be field name\n",
                            Pos_Arg(it_binary->lhs->token.pos));
                        exit(1);
                    }
                    Node_Atom *it_field_name = (Node_Atom *) it_binary->lhs;

                    bool ok = false;
                    for (size_t i = 0; i < struct_spec->fields_count; i++) {
                        Type_Struct_Field field = struct_spec->fields[i];
                        if (sv_eq(field.name, it_field_name->node.token.sv)) {
                            it->token.as.integer = i;
                            ok = true;
                            break;
                        }
                    }

                    if (!ok) {
                        error_undefined(&it_field_name->node.token, "field");
                    }

                    it_iota = it->token.as.integer;
                    it = it_binary->rhs;
                } else if (it_iota >= struct_spec->fields_count) {
                    fprintf(stderr, Pos_Fmt "ERROR: Too many ordered initializers\n", Pos_Arg(it->token.pos));
                    exit(1);
                }

                check_node(c, it, REF_NONE);
                type_assert(c, it, struct_spec->fields[it_iota].type);
            } else {
                unreachable();
            }
        }

        is_ref_valid = ref == REF_ADDR;
    } break;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        check_node(c, call->fn, REF_NONE);

        const Type fn_type = call->fn->type;
        if (fn_type.is_meta) {
            call->is_type_cast = true;
            n->type = fn_type;
            n->type.is_meta = false;

            if (!type_is_scalar(n->type)) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot cast to %s\n", Pos_Arg(call->fn->token.pos), type_to_cstr(n->type));
                exit(1);
            }

            if (!call->args.head) {
                error_too_few_arguments(call->end, 1);
                exit(1);
            } else if (call->args.head->next) {
                error_too_many_arguments(call->args.head->next->token.pos, 1);
                exit(1);
            }

            check_node(c, call->args.head, REF_NONE);
            const Type from_type = call->args.head->type;

            if (type_is_scalar(n->type)) {
                type_assert_scalar(call->args.head);

                bool ok = true;
                if (type_kind_eq(from_type, TYPE_FN) && !from_type.ref) {
                    // fn -> rawptr
                    ok = type_eq(n->type, (Type) {.kind = TYPE_RAWPTR});
                } else if (type_kind_eq(n->type, TYPE_FN) && !n->type.ref) {
                    // rawptr -> fn
                    ok = type_eq(from_type, (Type) {.kind = TYPE_RAWPTR});
                } else if (!type_is_pointer(from_type) && type_is_pointer(n->type)) {
                    // i64/u64 -> ptr
                    if (!type_kind_eq(from_type, TYPE_I64) && !type_kind_eq(from_type, TYPE_U64) &&
                        !type_kind_eq(from_type, TYPE_INT)) {
                        ok = false;
                    }
                } else if (type_is_pointer(from_type) && !type_is_pointer(n->type)) {
                    // ptr -> i64/u64
                    if (!type_kind_eq(n->type, TYPE_I64) && !type_kind_eq(n->type, TYPE_U64) &&
                        !type_kind_eq(n->type, TYPE_INT)) {
                        ok = false;
                    }
                }

                if (!ok) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot cast %s to %s\n",
                        Pos_Arg(call->fn->token.pos),
                        type_to_cstr(from_type),
                        type_to_cstr(n->type));
                    exit(1);
                }
            } else {
                unreachable();
            }

            if (type_eq(n->type, from_type)) {
                call->type_cast = TYPE_CAST_NOP;
            } else if (type_eq(n->type, (Type) {.kind = TYPE_BOOL})) {
                call->type_cast = TYPE_CAST_TO_BOOL;
            } else {
                call->type_cast = TYPE_CAST_NORMAL;
            }
        } else {
            if (!type_kind_eq(fn_type, TYPE_FN)) {
                fprintf(stderr, Pos_Fmt "ERROR: Cannot call %s\n", Pos_Arg(call->fn->token.pos), type_to_cstr(fn_type));
                exit(1);
            }

            if (fn_type.ref) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot call %s without deferencing it first\n",
                    Pos_Arg(call->fn->token.pos),
                    type_to_cstr(fn_type));
                exit(1);
            }

            call->args_count = 0;
            for (Node *arg = call->args.head; arg; arg = arg->next) {
                check_node(c, arg, REF_NONE);
                if (call->args_count >= fn_type.spec.fn.args_count) {
                    error_too_many_arguments(arg->token.pos, fn_type.spec.fn.args_count);
                }
                type_assert(c, arg, fn_type.spec.fn.args[call->args_count++].type);
            }

            if (call->args_count < fn_type.spec.fn.args_count) {
                error_too_few_arguments(call->end, fn_type.spec.fn.args_count);
            }

            n->type = *fn_type.spec.fn.returnn;
        }
    } break;

    case NODE_SLICE: {
        Node_Slice *slice = (Node_Slice *) n;

        // The type `[]T` gets compiled to:
        //
        // ```
        // struct {
        //     T  *data;
        //     i64 count;
        // }
        // ```
        //
        // It is not immediately necessary to calculate the properties of T, which allows for recursive definitions.
        check_node(c, slice->element, REF_ADDR);

        Type element_type = type_assert_type(slice->element);
        element_type.is_meta = false;

        const Type_Slice slice_type_spec = {
            .element = arena_clone(c->arena, &element_type, sizeof(element_type)),
        };

        n->type = (Type) {
            .kind = TYPE_SLICE,
            .is_meta = true,
            .spec.slice = slice_type_spec,
        };

        is_ref_valid = ref == REF_ADDR;
    } break;

    case NODE_INDEX: {
        // TODO: What about the signedness of indexing operations
        Node_Index *index = (Node_Index *) n;
        check_node(c, index->lhs, ref);
        is_ref_valid = true; // check_node() has already determined that the reference is valid

        if (index->is_ranged) {
            if (index->lhs->type.is_meta) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot take slice into %s\n",
                    Pos_Arg(index->lhs->token.pos),
                    type_to_cstr(index->lhs->type));

                exit(1);
            }

            if (index->lhs->type.ref) {
                // The beginning can be inferred to be 0
                if (index->a) {
                    check_node(c, index->a, REF_NONE);
                    type_assert_numeric(index->a, false);
                }

                // The ending CANNOT be inferred
                if (!index->b) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot infer end of range from %s\n",
                        Pos_Arg(index->lhs->token.pos),
                        type_to_cstr(index->lhs->type));

                    exit(1);
                }

                check_node(c, index->b, REF_NONE);
                type_assert_numeric(index->b, false);

                Type element_type = index->lhs->type;
                element_type.ref--;
                n->type = (Type) {
                    .kind = TYPE_SLICE,
                    .spec.slice.element = arena_clone(c->arena, &element_type, sizeof(element_type)),
                };
            } else if (type_kind_eq(index->lhs->type, TYPE_SLICE) || type_kind_eq(index->lhs->type, TYPE_STRING)) {
                // The beginning can be inferred to be the beginning of the slice
                if (index->a) {
                    check_node(c, index->a, REF_NONE);
                    type_assert_numeric(index->a, false);
                }

                // The ending can be inferred to be the ending of the slice
                if (index->b) {
                    check_node(c, index->b, REF_NONE);
                    type_assert_numeric(index->b, false);
                }

                n->type = index->lhs->type;
            } else {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot take slice into %s\n",
                    Pos_Arg(index->lhs->token.pos),
                    type_to_cstr(index->lhs->type));

                exit(1);
            }

            is_ref_valid = false;
        } else {
            if (type_kind_eq(index->lhs->type, TYPE_SLICE) && !index->lhs->type.ref) {
                check_node(c, index->a, REF_NONE);
                type_assert_numeric(index->a, false);
                n->type = *index->lhs->type.spec.slice.element;
            } else if (type_kind_eq(index->lhs->type, TYPE_STRING) && !index->lhs->type.ref) {
                check_node(c, index->a, REF_NONE);
                type_assert_numeric(index->a, false);
                n->type = (Type) {.kind = TYPE_CHAR};
            } else {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot index into %s\n",
                    Pos_Arg(index->lhs->token.pos),
                    type_to_cstr(index->lhs->type));

                // TODO(@slice): if `index->lhs` is a pointer, show a helper message on converting to a slice first
                // TODO(@slice): if `index->lhs` is a pointer to a slice, show a helper message about dereferencing
                exit(1);
            }
        }
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        assert(define->name->kind == NODE_ATOM && define->name->token.kind == TOKEN_IDENT);
        check_definition(c, (Node_Atom *) define->name, define->type, define->expr);
    } break;

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;

        const size_t context_end_save = c->context.current->end;
        for (Node *it = block->body.head; it; it = it->next) {
            define_orderless_nodes(c, it, context_end_save);
        }

        for (Node *it = block->body.head; it; it = it->next) {
            check_node(c, it, REF_NONE);
        }
        context_set_end(&c->context, context_end_save);
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        check_node(c, iff->condition, REF_NONE);
        type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});
        check_node(c, iff->consequence, REF_NONE);
        check_node(c, iff->antecedence, REF_NONE);
    } break;

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;

        const size_t context_end_save = c->context.current->end;
        {
            check_node(c, forr->init, REF_NONE);
            check_node(c, forr->condition, REF_NONE);
            if (forr->condition) {
                type_assert(c, forr->condition, (Type) {.kind = TYPE_BOOL});
            }
            check_node(c, forr->update, REF_NONE);
            check_node(c, forr->body, REF_NONE);
        }
        context_set_end(&c->context, context_end_save);
    } break;

    case NODE_JUMP:
        // Pass
        break;

    case NODE_RETURN: {
        Node_Return *returnn = (Node_Return *) n;
        const Type   expected = *c->context.current->fn->node.type.spec.fn.returnn;

        n->type.kind = TYPE_UNIT;
        if (returnn->value) {
            check_node(c, returnn->value, REF_NONE);
            type_assert(c, returnn->value, expected);
            n->type = returnn->value->type;
        } else {
            type_assert(c, n, expected);
        }
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            check_node(c, it, REF_NONE);
        }
    } break;

    case NODE_PRINT: {
        Node_Print *print = (Node_Print *) n;
        check_node(c, print->value, REF_NONE);
        type_assert_scalar(print->value);
    } break;

    default:
        unreachable();
    }

    if (!is_ref_valid) {
        switch (ref) {
        case REF_NONE:
            // OK
            break;

        case REF_ADDR:
            if (!n->type.is_meta) {
                fprintf(stderr, Pos_Fmt "ERROR: Cannot take reference to value not in memory\n", Pos_Arg(n->token.pos));
                exit(1);
            }
            break;

        case REF_ASSIGN:
            fprintf(stderr, Pos_Fmt "ERROR: Cannot assign to value not in memory\n", Pos_Arg(n->token.pos));
            exit(1);
            break;
        }
    }
}

void check_nodes(Compiler *c, Nodes nodes) {
    for (Node *it = nodes.head; it; it = it->next) {
        define_orderless_nodes(c, it, 0);
    }

    for (Node *it = nodes.head; it; it = it->next) {
        check_node(c, it, REF_NONE);
    }
}
