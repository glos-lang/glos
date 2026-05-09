#include "checker.h"
#include "ast.h"
#include "basic.h"
#include "context.h"
#include "token.h"

static void error_undefined(const Token *t, const char *label) {
    fprintf(stderr, Pos_Fmt "ERROR: Undefined %s '" SV_Fmt "'\n", Pos_Arg(t->pos), label, SV_Arg(t->sv));
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

static_assert(COUNT_AST_NODES == 16, "");
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
    if (ast_type_is_integer(expected) && ast_type_kind_eq(n->type, AST_TYPE_INT)) {
        if (!ast_type_kind_eq(expected, AST_TYPE_INT)) {
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

static AST_Type ast_type_assert_type(const AST_Node *n) {
    if (n->type.is_meta) {
        return n->type;
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected a type, got %s\n", Pos_Arg(n->token.pos), ast_type_to_cstr(n->type));
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
    if (ast_type_kind_eq(n->type, AST_TYPE_INT)) {
        n->type.kind = AST_TYPE_I64;
    }
}

static void ast_node_assert_can_be_referenced(AST_Node *n) {
    if (!n->is_memory) {
        fprintf(stderr, Pos_Fmt "ERROR: Cannot take reference to value not in memory\n", Pos_Arg(n->token.pos));
        exit(1);
    }
}

static_assert(COUNT_AST_NODES == 16, "");
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

static_assert(COUNT_AST_NODES == 16, "");
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

static_assert(COUNT_AST_NODES == 16, "");
static Const_Value eval_const_expr(Compiler *c, AST_Node *n) {
    if (!n) {
        return (Const_Value) {0};
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        AST_Node_Atom *atom = (AST_Node_Atom *) n;

        static_assert(COUNT_TOKENS == 41, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
        case TOKEN_INT:
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

        default:
            unreachable();
            break;
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        Const_Value     value = {0};

        static_assert(COUNT_TOKENS == 41, "");
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

        static_assert(COUNT_TOKENS == 41, "");
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

    case AST_NODE_MEMBER: {
        AST_Node_Member  *member = (AST_Node_Member *) n;
        const Const_Value value = eval_const_expr(c, member->lhs);
        assert(value.kind == CONST_VALUE_STRUCT);
        return value.as.structt.fields[member->field_index];
    }

    case AST_NODE_FN: {
        AST_Node_Fn *fn = (AST_Node_Fn *) n;
        if (fn->is_type) {
            return const_value_type(fn->node.type);
        } else {
            return const_value_fn(fn);
        }
    }

    case AST_NODE_STRUCT: {
        return const_value_type(n->type);
    }

    case AST_NODE_COMPOUND: {
        AST_Node_Compound *compound = (AST_Node_Compound *) n;

        AST_Type_Struct    struct_spec = {0};
        Const_Value_Struct struct_value = {0};
        if (n->type.kind == AST_TYPE_STRUCT) {
            struct_spec = n->type.spec.structt;
            struct_value.spec = struct_spec;
            struct_value.fields = arena_alloc(c->arena, struct_spec.fields_count * sizeof(*struct_value.fields));
        }

        size_t ordered_iota = 0;
        for (AST_Node *iter = compound->children.head; iter; iter = iter->next) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            AST_Node *it = iter;
            if (n->type.kind == AST_TYPE_STRUCT) {
                if (compound->is_designated) {
                    assert(it->kind == AST_NODE_BINARY && it->token.kind == TOKEN_SET);
                    AST_Node_Binary *it_binary = (AST_Node_Binary *) it;
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

static_assert(COUNT_AST_NODES == 16, "");
static void define_orderless_nodes(Compiler *c, AST_Node *n, const size_t block_start) {
    switch (n->kind) {
    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        assert(define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT);

        AST_Node_Atom *it = (AST_Node_Atom *) define->name;
        if (!sv_match(it->node.token.sv, "_")) {
            if (it->is_local) {
                if (it->is_const) {
                    const Context_Fn *fn = c->context.current;

                    assert(fn->end <= c->context.locals.count);
                    assert(block_start <= c->context.locals.count);
                    assert(block_start <= fn->end);
                    for (size_t i = fn->end; i > block_start; i--) {
                        AST_Node_Atom *previous = c->context.locals.data[i - 1];
                        if (!previous->is_const) {
                            continue;
                        }

                        if (sv_eq(it->node.token.sv, previous->node.token.sv)) {
                            error_redefinition(it, previous);
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

                AST_Node_Atom *previous = scope_find(c->globals, it->node.token.sv);
                if (previous) {
                    error_redefinition(it, previous);
                }

                da_push(&c->globals, it);
            }
        }
    } break;

    case AST_NODE_EXTERN: {
        AST_Node_Extern *externn = (AST_Node_Extern *) n;
        for (AST_Node *it = externn->nodes.head; it; it = it->next) {
            define_orderless_nodes(c, it, block_start);
        }
    } break;

    default:
        // Pass
        break;
    }
}

static void check_node(Compiler *c, AST_Node *n);

static void check_definition(Compiler *c, AST_Node_Atom *it, AST_Node *type, AST_Node *it_expr) {
    assert(it->check_status != CHECKING); // It is already checked
    if (it->check_status == CHECKED) {
        return;
    }
    it->check_status = CHECKING;

    if (type) {
        check_node(c, type);
        it->node.type = ast_type_assert_type(type);
        it->node.type.is_meta = false;
    }

    if (it_expr) {
        check_node(c, it_expr);

        if (ast_type_kind_eq(it_expr->type, AST_TYPE_UNIT) || (it_expr->type.is_meta && !it->is_const)) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot store %s in a %s\n",
                Pos_Arg(it_expr->token.pos),
                ast_type_to_cstr(it_expr->type),
                it->is_const ? "constant" : "variable");

            exit(1);
        }

        if (type) {
            ast_type_assert(c, it_expr, it->node.type);
        } else {
            if (!it->is_const) {
                ast_node_finalize_type_of_untyped(it_expr);
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
        n->is_memory = !definition->is_const;
        return;
    }

    AST_Type_Kind kind;
    if (get_builtin_type_kind(n->token.sv, &kind)) {
        n->type = (AST_Type) {.kind = kind, .is_meta = true};
        return;
    }

    error_undefined(&n->token, "identifier");
}

static_assert(COUNT_AST_NODES == 16, "");
static void check_node(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_ATOM: {
        static_assert(COUNT_TOKENS == 41, "");
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

        static_assert(COUNT_TOKENS == 41, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            n->type = ast_type_assert_numeric(unary->value, false);
            break;

        case TOKEN_MUL:
            if (!unary->value->type.ref) {
                if (ast_type_kind_eq(unary->value->type, AST_TYPE_RAWPTR)) {
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
            if (!n->type.is_meta) {
                ast_node_assert_can_be_referenced(unary->value);
            }
            n->type.ref++;
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

        static_assert(COUNT_TOKENS == 41, "");
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
            // TODO: Check whether lhs can be mutated as well
            //
            // Example:
            // ```
            // Vec2 :: struct {
            //     x: i32,
            //     y: i32
            // }

            // v :: Vec2 {69, 420}

            // main :: () {
            //     v.x = 1337 // This should be illegal, but it is currently not
            // }
            // ```
            ast_node_assert_can_be_referenced(binary->lhs);
            ast_type_assert_node(c, binary->rhs, binary->lhs);
            n->type = (AST_Type) {.kind = AST_TYPE_UNIT};
            break;

        default:
            unreachable();
        }
    } break;

    case AST_NODE_MEMBER: {
        AST_Node_Member *member = (AST_Node_Member *) n;
        check_node(c, member->lhs);

        if (!ast_type_kind_eq(member->lhs->type, AST_TYPE_STRUCT)) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot access field of %s\n",
                Pos_Arg(n->token.pos),
                ast_type_to_cstr(member->lhs->type));
            exit(1);
        }

        AST_Node_Atom  *definition = NULL;
        AST_Type_Struct spec = member->lhs->type.spec.structt;
        for (size_t i = 0; i < spec.fields_count; i++) {
            AST_Node_Atom *it = spec.fields[i];
            if (sv_eq(it->node.token.sv, member->field.sv)) {
                definition = it;
                member->field_index = i;
                break;
            }
        }

        if (!definition) {
            error_undefined(&member->field, "field");
        }

        n->is_memory = true;
        n->type = definition->node.type;
    } break;

    case AST_NODE_FN: {
        AST_Node_Fn *fn = (AST_Node_Fn *) n;

        Context_Fn context_fn = {.fn = fn, .outer = c->context.current};
        context_push_fn(&c->context, &context_fn);

        {
            AST_Type_Fn fn_type_spec = {
                .args = arena_alloc(c->arena, fn->args_count * sizeof(*fn_type_spec.args)),
            };

            for (AST_Node *arg = fn->args.head; arg; arg = arg->next) {
                assert(arg->kind == AST_NODE_DEFINE);
                AST_Node_Define *define = (AST_Node_Define *) arg;

                assert(define->name->kind == AST_NODE_ATOM);
                AST_Node_Atom *it = (AST_Node_Atom *) define->name;
                if (!sv_match(it->node.token.sv, "_")) {
                    for (size_t i = 0; i < fn_type_spec.args_count; i++) {
                        AST_Node_Atom *previous = fn_type_spec.args[i];
                        if (sv_eq(previous->node.token.sv, it->node.token.sv)) {
                            error_redefinition(it, previous);
                        }
                    }
                }
                fn_type_spec.args[fn_type_spec.args_count++] = it;

                check_node(c, arg);
            }

            if (fn->returnn) {
                check_node(c, fn->returnn);
                ast_type_assert_type(fn->returnn);
                fn_type_spec.returnn = arena_clone(c->arena, &fn->returnn->type, sizeof(AST_Type));
                fn_type_spec.returnn->is_meta = false;
            } else {
                fn_type_spec.returnn = arena_alloc(c->arena, sizeof(AST_Type));
                fn_type_spec.returnn->kind = AST_TYPE_UNIT;
            }

            n->type = (AST_Type) {.kind = AST_TYPE_FN, .spec.fn = fn_type_spec};

            if (fn->defined_as) {
                // The body of a function is irrelevant for outer expressions
                fn->defined_as->node.type = n->type;
                fn->defined_as->check_status = CHECKED;
            }

            if (fn->is_type) {
                n->type.is_meta = true;
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

    case AST_NODE_STRUCT: {
        AST_Node_Struct *structt = (AST_Node_Struct *) n;

        const AST_Type_Struct structt_type_spec = {
            .fields = arena_alloc(c->arena, structt->fields_count * sizeof(*structt_type_spec.fields)),
            .fields_count = structt->fields_count,
            .definition = structt,
        };

        n->type = (AST_Type) {.kind = AST_TYPE_STRUCT, .is_meta = true, .spec.structt = structt_type_spec};

        size_t iota = 0;
        for (AST_Node *field = structt->fields.head; field; field = field->next) {
            assert(field->kind == AST_NODE_DEFINE);
            AST_Node_Define *define = (AST_Node_Define *) field;

            assert(define->name->kind == AST_NODE_ATOM);
            AST_Node_Atom *it = (AST_Node_Atom *) define->name;
            if (!sv_match(it->node.token.sv, "_")) {
                for (size_t i = 0; i < iota; i++) {
                    AST_Node_Atom *previous = structt_type_spec.fields[i];
                    if (sv_eq(previous->node.token.sv, it->node.token.sv)) {
                        error_redefinition(it, previous);
                    }
                }
            }
            structt_type_spec.fields[iota++] = it;

            check_node(c, define->type);
            it->node.type = ast_type_assert_type(define->type);
            it->node.type.is_meta = false;
        }
    } break;

    case AST_NODE_COMPOUND: {
        AST_Node_Compound *compound = (AST_Node_Compound *) n;
        check_node(c, compound->lhs);
        ast_type_assert_type(compound->lhs);

        n->type = compound->lhs->type;
        n->type.is_meta = false;
        if (n->type.ref || (n->type.kind != AST_TYPE_STRUCT)) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Expected structure type, got %s\n",
                Pos_Arg(compound->lhs->token.pos),
                ast_type_to_cstr(n->type));
            exit(1);
        }

        // For structure literal
        AST_Type_Struct struct_spec = {0};
        if (n->type.kind == AST_TYPE_STRUCT) {
            struct_spec = n->type.spec.structt;
        }

        size_t ordered_iota = 0;
        for (AST_Node *iter = compound->children.head; iter; iter = iter->next) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            AST_Node *it = iter;
            if (n->type.kind == AST_TYPE_STRUCT) {
                if (compound->is_designated) {
                    assert(it->kind == AST_NODE_BINARY && it->token.kind == TOKEN_SET);
                    AST_Node_Binary *it_binary = (AST_Node_Binary *) it;

                    if (it_binary->lhs->kind != AST_NODE_ATOM || it_binary->lhs->token.kind != TOKEN_IDENT) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Expected designated initializer to be field name\n",
                            Pos_Arg(it_binary->lhs->token.pos));
                        exit(1);
                    }
                    AST_Node_Atom *it_field_name = (AST_Node_Atom *) it_binary->lhs;

                    bool ok = false;
                    for (size_t i = 0; i < struct_spec.fields_count; i++) {
                        AST_Node_Atom *field = struct_spec.fields[i];
                        if (sv_eq(field->node.token.sv, it_field_name->node.token.sv)) {
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
                } else if (it_iota >= struct_spec.fields_count) {
                    fprintf(stderr, Pos_Fmt "ERROR: Too many ordered initializers\n", Pos_Arg(it->token.pos));
                    exit(1);
                }

                check_node(c, it);
                ast_type_assert(c, it, struct_spec.fields[it_iota]->node.type);
            } else {
                unreachable();
            }
        }
    } break;

    case AST_NODE_CALL: {
        AST_Node_Call *call = (AST_Node_Call *) n;
        check_node(c, call->fn);

        const AST_Type fn_type = call->fn->type;
        if (fn_type.is_meta) {
            call->is_type_cast = true;
            n->type = fn_type;
            n->type.is_meta = false;

            if (!ast_type_is_scalar(n->type)) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot cast to %s\n",
                    Pos_Arg(call->fn->token.pos),
                    ast_type_to_cstr(n->type));
                exit(1);
            }

            if (!call->args.head) {
                error_too_few_arguments(call->end, 1);
                exit(1);
            } else if (call->args.head->next) {
                error_too_many_arguments(call->args.head->next->token.pos, 1);
                exit(1);
            }

            check_node(c, call->args.head);
            const AST_Type from_type = call->args.head->type;

            if (ast_type_is_scalar(n->type)) {
                ast_type_assert_scalar(call->args.head);

                bool ok = true;
                if (ast_type_kind_eq(from_type, AST_TYPE_FN) && !from_type.ref) {
                    // fn -> rawptr
                    ok = ast_type_eq(n->type, (AST_Type) {.kind = AST_TYPE_RAWPTR});
                } else if (ast_type_kind_eq(n->type, AST_TYPE_FN) && !n->type.ref) {
                    // rawptr -> fn
                    ok = ast_type_eq(from_type, (AST_Type) {.kind = AST_TYPE_RAWPTR});
                } else if (!ast_type_is_pointer(from_type) && ast_type_is_pointer(n->type)) {
                    // i64/u64 -> ptr
                    if (!ast_type_kind_eq(from_type, AST_TYPE_I64) && !ast_type_kind_eq(from_type, AST_TYPE_U64) &&
                        !ast_type_kind_eq(from_type, AST_TYPE_INT)) {
                        ok = false;
                    }
                } else if (ast_type_is_pointer(from_type) && !ast_type_is_pointer(n->type)) {
                    // ptr -> i64/u64
                    if (!ast_type_kind_eq(n->type, AST_TYPE_I64) && !ast_type_kind_eq(n->type, AST_TYPE_U64) &&
                        !ast_type_kind_eq(n->type, AST_TYPE_INT)) {
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
                unreachable();
            }

            if (ast_type_eq(n->type, from_type)) {
                call->type_cast = TYPE_CAST_NOP;
            } else if (ast_type_eq(n->type, (AST_Type) {.kind = AST_TYPE_BOOL})) {
                call->type_cast = TYPE_CAST_TO_BOOL;
            } else {
                call->type_cast = TYPE_CAST_NORMAL;
            }
        } else {
            if (!ast_type_kind_eq(fn_type, AST_TYPE_FN)) {
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

            call->args_count = 0;
            for (AST_Node *arg = call->args.head; arg; arg = arg->next) {
                check_node(c, arg);
                if (call->args_count >= fn_type.spec.fn.args_count) {
                    error_too_many_arguments(arg->token.pos, fn_type.spec.fn.args_count);
                }
                ast_type_assert(c, arg, fn_type.spec.fn.args[call->args_count++]->node.type);
            }

            if (call->args_count < fn_type.spec.fn.args_count) {
                error_too_few_arguments(call->end, fn_type.spec.fn.args_count);
            }

            n->type = *fn_type.spec.fn.returnn;
        }
    } break;

    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        assert(define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT);
        check_definition(c, (AST_Node_Atom *) define->name, define->type, define->expr);
    } break;

    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;

        const size_t context_end_save = c->context.current->end;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            define_orderless_nodes(c, it, context_end_save);
        }

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

void check_nodes(Compiler *c, AST_Nodes nodes) {
    for (AST_Node *it = nodes.head; it; it = it->next) {
        define_orderless_nodes(c, it, 0);
    }

    for (AST_Node *it = nodes.head; it; it = it->next) {
        check_node(c, it);
    }
}
