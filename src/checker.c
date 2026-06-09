#include "checker.h"
#include "basic.h"
#include "context.h"
#include "node.h"
#include "parser.h"
#include "token.h"
#include <assert.h>
#include <stdint.h>

static void error_undefined(const Token *t, const char *label, bool no_exit) {
    fprintf(stderr, Pos_Fmt "ERROR: Undefined %s '" SV_Fmt "'\n", Pos_Arg(t->pos), label, SV_Arg(t->sv));
    if (!no_exit) {
        exit(1);
    }
}

static void error_redefinition(const Node *n, const Pos *previous) {
    fprintf(stderr, Pos_Fmt "ERROR: Redefinition of '" SV_Fmt "'\n", Pos_Arg(n->token.pos), SV_Arg(n->token.sv));
    if (previous) {
        fprintf(stderr, Pos_Fmt "NOTE: Defined here\n", Pos_Arg(*previous));
    }
    exit(1);
}

static void error_number_of_return_values_mismatch(Pos pos, size_t expected, size_t actual) {
    fprintf(
        stderr,
        Pos_Fmt "ERROR: Too %s return values: Expected %zu, got %zu\n",
        Pos_Arg(pos),
        actual < expected ? "few" : "many",
        expected,
        actual);
    exit(1);
}

static void print_quoted_char(FILE *f, char ch, char quote) {
    switch (ch) {
    case 033:
        fputs("\\e", f);
        break;

    case '\n':
        fputs("\\n", f);
        break;

    case '\r':
        fputs("\\r", f);
        break;

    case '\t':
        fputs("\\t", f);
        break;

    case '\0':
        fputs("\\0", f);
        break;

    case '\\':
        fputs("\\\\", f);
        break;

    case '\'':
        if (quote == '\'') {
            fputs("\\'", f);
        } else {
            fputc(ch, f);
        }
        break;

    case '"':
        if (quote == '"') {
            fputs("\\\"", f);
        } else {
            fputc(ch, f);
        }
        break;

    default:
        fputc(ch, f);
        break;
    }
}

static void check_that_type_is_known(const Node *n) {
    if (type_is_unknown(n->type)) {
        fprintf(stderr, Pos_Fmt "ERROR: Cannot infer type of this expression\n", Pos_Arg(n->token.pos));
        exit(1);
    }
}

static_assert(COUNT_TYPES == 23, "");
static void check_int_limit(Node *n, const void *ptr) {
    if (type_is_signed(n->type)) {
        typedef struct {
            i64 min;
            i64 max;
        } Limit;

        const Limit limits[COUNT_TYPES] = {
            [TYPE_I8] = {.min = INT8_MIN, .max = INT8_MAX},
            [TYPE_I16] = {.min = INT16_MIN, .max = INT16_MAX},
            [TYPE_I32] = {.min = INT32_MIN, .max = INT32_MAX},
            [TYPE_I64] = {.min = INT64_MIN, .max = INT64_MAX},
            [TYPE_INT] = {.min = INT64_MIN, .max = INT64_MAX},
        };

        const i64   value = *(const i64 *) ptr;
        const Limit limit = limits[n->type.kind];
        if (value < limit.min || value > limit.max) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Number '%zd' is invalid for %s, which must be in range [%zd, %zd]\n",
                Pos_Arg(n->token.pos),
                value,
                type_to_cstr(n->type),
                limit.min,
                limit.max);
            exit(1);
        }
    } else {
        const size_t limits[COUNT_TYPES] = {
            [TYPE_U8] = UINT8_MAX,
            [TYPE_U16] = UINT16_MAX,
            [TYPE_U32] = UINT32_MAX,
            [TYPE_U64] = UINT64_MAX,
        };

        const size_t value = *(const size_t *) ptr;
        const size_t limit = limits[n->type.kind];
        if (value > limit) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Number '%zu' is invalid for %s, which must be in range [0, %zu]\n",
                Pos_Arg(n->token.pos),
                value,
                type_to_cstr(n->type),
                limit);
            exit(1);
        }
    }
}

// TODO: What about sign?
static i64 enum_get_value(Node_Enum *enumm, SV name, const Token *t) {
    ll_foreach(it, &enumm->values) {
        if (sv_eq(it->token.sv, name)) {
            return it->token.as.integer;
        }
    }

    error_undefined(t, "enumeration value", true);
    fprintf(stderr, Pos_Fmt "NOTE: Enumeration defined here\n", Pos_Arg(enumm->node.token.pos));
    exit(1);
}

static_assert(COUNT_NODES == 27, "");
static void cast_untyped(Compiler *c, Node *n, Type expected) {
    switch (n->kind) {
    case NODE_ATOM: {
        static_assert(COUNT_TOKENS == 72, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = expected;
            check_int_limit(n, &n->token.as.integer);
            break;

        case TOKEN_IDENT: {
            Node_Atom *atom = (Node_Atom *) n;
            assert(atom->definition->definition_spec->is_const); // Only constants can be defined as untyped int

            n->type = expected;
            check_int_limit(n, &atom->definition->definition_spec->const_value.as.integer);
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        n->type = expected;
        if (n->token.kind == TOKEN_SIZEOF) {
            const size_t value = compile_sizeof(c, &unary->value->type);
            check_int_limit(n, &value);
        } else {
            if (!type_is_signed(expected) && n->token.kind == TOKEN_SUB) {
                fprintf(stderr, Pos_Fmt "ERROR: Cannot negate unsigned constant value\n", Pos_Arg(n->token.pos));
                exit(1);
            }
            cast_untyped(c, unary->value, expected);
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        cast_untyped(c, binary->lhs, expected);
        cast_untyped(c, binary->rhs, expected);
        n->type = expected;
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        if (member->is_enum) {
            assert(type_kind_eq(member->node.type, TYPE_UNKNOWN_ENUM));
            assert(type_kind_eq(expected, TYPE_ENUM));
            member->enum_value = enum_get_value(expected.spec.enumm.definition, member->field.sv, &member->field);
            n->type = expected;
        } else {
            assert(member->module_access_definition); // Must be a module access

            const Definition_Spec *definition_spec = member->module_access_definition->definition_spec;
            assert(definition_spec->is_const); // Only constants can be defined as untyped int

            n->type = expected;
            check_int_limit(n, &definition_spec->const_value.as.integer);
        }
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

static Const_Value eval_const_expr(Compiler *c, Node *n);

static bool try_auto_cast_untyped(Compiler *c, Node *n, Type expected) {
    if (type_is_integer(expected) && type_kind_eq(n->type, TYPE_INT)) {
        if (!type_kind_eq(expected, TYPE_INT)) {
            cast_untyped(c, n, expected);

            // Only constant expressions can be untyped integers
            const Const_Value value = eval_const_expr(c, n);
            assert(value.kind == CONST_VALUE_INT);

            check_int_limit(n, &value.as.integer);
        }
        return true;
    }

    if (type_kind_eq(expected, TYPE_ENUM) && type_kind_eq(n->type, TYPE_UNKNOWN_ENUM)) {
        cast_untyped(c, n, expected);
        return true;
    }

    return false;
}

static bool type_eq_without_distinct(Type a, Type b) {
    a.distinct = NULL;
    b.distinct = NULL;
    return type_eq(a, b);
}

// Nice name
static void maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(Node *n, Type expected) {
    if (type_eq_without_distinct(n->type, expected)) {
        fprintf(
            stderr,
            Pos_Fmt "NOTE: The underlying types seem to be equal, but distinct. Try an explicit cast.\n",
            Pos_Arg(n->token.pos));
    }
}

static bool try_auto_cast_literal(Compiler *c, Node *n, Type expected) {
    // untyped 'null' -> typed 'null'
    if (n->kind == NODE_ATOM && n->token.kind == TOKEN_NULL && (expected.ref || type_kind_eq(expected, TYPE_RAWPTR))) {
        // NOTE: We are also checking for rawptr because distinct types exist
        n->type = expected;
        return true;
    }

    if (n->kind == NODE_COMPOUND && type_kind_eq(n->type, TYPE_ARRAY) && type_kind_eq(expected, TYPE_SLICE)) {
        if (type_eq(*n->type.spec.array.element, *expected.spec.slice.element)) {
            Node_Compound *compound = (Node_Compound *) n;
            compound->memory_type = arena_clone(c->arena, &n->type, sizeof(n->type));
            compound->auto_cast_array_to_slice = true;
            n->type = expected;
            return true;
        }
    }

    return false;
}

static Type type_assert(Compiler *c, Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return expected;
    }

    if (try_auto_cast_untyped(c, n, expected)) {
        return expected;
    }

    if (try_auto_cast_literal(c, n, expected)) {
        return expected;
    }

    check_that_type_is_known(n);

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected %s, got %s\n",
        Pos_Arg(n->token.pos),
        type_to_cstr(expected),
        type_to_cstr(n->type));

    maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(n, expected);
    exit(1);
}

static Type type_assert_grouped(Compiler *c, Node *n, Type expected, i64 group_index, Pos *requirement) {
    Type actual = n->type;

    const bool is_group = group_index != -1 && type_kind_eq(actual, TYPE_GROUP);
    if (is_group) {
        actual = n->type.spec.group.data[group_index];
    }

    if (type_eq(actual, expected)) {
        return expected;
    }

    if (!is_group) {
        if (try_auto_cast_untyped(c, n, expected)) {
            return expected;
        }

        if (try_auto_cast_literal(c, n, expected)) {
            return expected;
        }

        check_that_type_is_known(n);

        fprintf(
            stderr,
            Pos_Fmt "ERROR: Expected %s, got %s\n",
            Pos_Arg(n->token.pos),
            type_to_cstr(expected),
            type_to_cstr(actual));

        maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(n, expected);
    } else {
        check_that_type_is_known(n);

        const char *postfix = "th";
        switch ((group_index + 1) % 10) {
        case 1:
            postfix = "st";
            break;

        case 2:
            postfix = "nd";
            break;

        case 3:
            postfix = "rd";
            break;
        }

        fprintf(
            stderr,
            Pos_Fmt "ERROR: Expected %zd%s value of this to be %s, got %s. The type of this entire expression is %s\n",
            Pos_Arg(n->token.pos),
            group_index + 1,
            postfix,
            type_to_cstr(expected),
            type_to_cstr(actual),
            type_to_cstr(n->type));

        if (requirement) {
            fprintf(stderr, Pos_Fmt "NOTE: Required here\n", Pos_Arg(*requirement));
        }
    }

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

    if (try_auto_cast_literal(c, a, b->type)) {
        return a->type;
    }

    if (try_auto_cast_literal(c, b, a->type)) {
        return b->type;
    }

    check_that_type_is_known(a);
    check_that_type_is_known(b);

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Expected %s, got %s\n",
        Pos_Arg(a->token.pos),
        type_to_cstr(b->type),
        type_to_cstr(a->type));

    maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(a, b->type);
    exit(1);
}

static Type type_assert_numeric(const Node *n, bool pointers_allowed) {
    if (type_is_numeric(n->type)) {
        return n->type;
    }

    if (pointers_allowed && type_is_pointer(n->type)) {
        return n->type;
    }

    check_that_type_is_known(n);

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

    check_that_type_is_known(n);
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
    static_assert(COUNT_TYPES == 23, "");
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

static void node_finalize_type_of_unknown(Type *t) {
    if (type_kind_eq(*t, TYPE_INT)) {
        t->kind = TYPE_I64;
    }
}

static_assert(COUNT_NODES == 27, "");
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
        if (iff->is_compile_time) {
            return loop_breaks(iff->compile_time_real_block);
        }
        return loop_breaks(iff->consequence) || loop_breaks(iff->antecedence);
    }

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        return loop_breaks(case_->body);
    }

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            return loop_breaks(sw->compile_time_real_block);
        }

        for (Node *it = sw->cases.head; it; it = it->next) {
            if (!loop_breaks(it)) {
                return false;
            }
        }
        return sw->fallback != NULL;
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

static_assert(COUNT_NODES == 27, "");
static bool always_returns(Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        return is_atom_false(assertt->expr);
    }

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
        if (iff->is_compile_time) {
            return always_returns(iff->compile_time_real_block);
        }

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

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        return always_returns(case_->body);
    }

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            return always_returns(sw->compile_time_real_block);
        }

        for (Node *it = sw->cases.head; it; it = it->next) {
            if (!always_returns(it)) {
                return false;
            }
        }
        return sw->fallback != NULL;
    }

    case NODE_RETURN:
        return true;

    default:
        return false;
    }
}

typedef enum {
    REF_NONE,
    REF_ADDR,
    REF_ASSIGN,
} Ref_Kind;

static void check_expr(Compiler *c, Node *n, Ref_Kind ref, const Type *expected_type);
static void check_stmt(Compiler *c, Node *n);

// TODO: Should this be moved back into 'compiler.c'?
static Node_Fn *get_main(Compiler *c) {
    if (c->main_fn) {
        return c->main_fn;
    }

    Node_Atom *main = global_scope_find(&c->main_module->globals, sv_from_cstr("main"));
    if (!main) {
        fprintf(
            stderr,
            "ERROR: Function 'main' is not defined\n"
            "\n"
            "```\n"
            "main :: () {\n"
            "}\n"
            "```\n");
        exit(1);
    }

    if (!main->definition_spec->is_const || main->definition_spec->assignment_node->kind != NODE_FN) {
        fprintf(stderr, Pos_Fmt "ERROR: Identifier 'main' must be a function literal\n", Pos_Arg(main->node.token.pos));
        fprintf(
            stderr,
            "\n"
            "```\n"
            "main :: () {\n"
            "}\n"
            "```\n");
        exit(1);
    }

    c->main_fn = (Node_Fn *) main->definition_spec->assignment_node;
    check_stmt(c, (Node *) main->definition_spec->definition_node);

    const Type_Fn signature = main->node.type.spec.fn;
    if (signature.args_count) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot take any arguments\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }

    if (signature.returns_count) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot return anything\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }

    return c->main_fn;
}

// Is this valid for signedness?
static_assert(COUNT_NODES == 27, "");
static Const_Value eval_const_expr(Compiler *c, Node *n) {
    if (!n) {
        return (Const_Value) {0};
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;

        static_assert(COUNT_TOKENS == 72, "");
        switch (n->token.kind) {
        case TOKEN_INT:
        case TOKEN_BOOL:
        case TOKEN_CHAR:
            return const_value_int(n->token.as.integer);

        case TOKEN_NULL:
            return const_value_int(0);

        case TOKEN_IDENT:
            if (n->type.is_meta) {
                return const_value_type(n->type);
            }

            assert(atom->definition);
            if (!atom->definition->definition_spec->is_const) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot use variables in a constant expression\n", Pos_Arg(n->token.pos));
                exit(1);
            }

            return atom->definition->definition_spec->const_value;

        case TOKEN_STRING:
            return const_value_string(n->token.sv);

        case TOKEN_DIRECTIVE_MAIN:
            return const_value_fn(get_main(c));

        case TOKEN_DIRECTIVE_PLATFORM:
            return get_platform(c, NULL);

        default:
            unreachable();
        }
    }

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        Const_Value value = {0};

        static_assert(COUNT_TOKENS == 72, "");
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

        static_assert(COUNT_TOKENS == 72, "");
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
            return const_value_int(const_value_eq(lhs, rhs));

        case TOKEN_NE:
            lhs = eval_const_expr(c, binary->lhs);
            rhs = eval_const_expr(c, binary->rhs);
            return const_value_int(!const_value_eq(lhs, rhs));

        default:
            unreachable();
            break;
        }
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        if (member->is_enum) {
            return const_value_int(member->enum_value);
        }

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

        case CONST_VALUE_MODULE: {
            Node_Atom *definition = member->module_access_definition;
            assert(definition);

            if (n->type.is_meta) {
                return const_value_type(n->type);
            }

            if (!definition->definition_spec->is_const) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot use variables in a constant expression\n", Pos_Arg(n->token.pos));
                exit(1);
            }

            return definition->definition_spec->const_value;
        }

        default:
            unreachable();
        }
    }

    case NODE_IMPORT:
        assert(n->type.kind == TYPE_MODULE);
        return const_value_module(n->type.spec.module);

    case NODE_DISTINCT:
        assert(n->type.is_meta);
        return const_value_type(n->type);

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;
        if (fn->is_type) {
            return const_value_type(fn->node.type);
        } else {
            return const_value_fn(fn);
        }
    }

    case NODE_ENUM:
    case NODE_STRUCT:
        return const_value_type(n->type);

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;

        Const_Value_Struct struct_value = {0};
        if (n->type.kind == TYPE_STRUCT) {
            struct_value.spec = n->type.spec.structt;
            struct_value.fields = arena_alloc(c->arena, struct_value.spec->fields_count * sizeof(*struct_value.fields));
            // TODO: This broken for nested compound values
        } else {
            todo(); // TODO(@array)
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
                        Pos_Fmt "ERROR: Range (%zd..%zd) is invalid: Beginning of range is more than end\n",
                        Pos_Arg(n->token.pos),
                        begin,
                        end);
                    exit(1);
                }

                if (begin < 0 || end < 0 || (size_t) begin > sv.count || (size_t) end > sv.count) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Range (%zd..%zd) is out of bounds in string of length %zu\n",
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
                        Pos_Fmt "ERROR: Index %zd is out of bounds in string of length %zu\n",
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

    case NODE_INDEXABLE:
        return const_value_type(n->type);

    default:
        unreachable();
        break;
    }
}

static void check_switch_expr_and_alloc_preds(Compiler *c, Node_Switch *sw) {
    check_expr(c, sw->expr, REF_NONE, NULL);
    if (type_kind_eq(sw->expr->type, TYPE_ENUM)) {
        sw->enumeration = sw->expr->type.spec.enumm.definition;
    }

    node_finalize_type_of_unknown(&sw->expr->type);
    check_that_type_is_known(sw->expr);

    if (!type_is_numeric(sw->expr->type) && !type_kind_eq(sw->expr->type, TYPE_CHAR)) {
        fprintf(
            stderr,
            Pos_Fmt "ERROR: Expected numeric or character value, got %s\n",
            Pos_Arg(sw->expr->token.pos),
            type_to_cstr(sw->expr->type));
        exit(1);
    }

    if (!sw->preds) {
        sw->preds = arena_alloc(c->arena, sw->preds_count * sizeof(*sw->preds));
    }
}

static Const_Value check_switch_pred(Compiler *c, Node_Switch *sw, Node *pred, size_t *iota) {
    bool        checked = false;
    Const_Value value = {0};

    if (!checked) {
        check_expr(c, pred, REF_NONE, &sw->expr->type);
        type_assert(c, pred, sw->expr->type);
        value = eval_const_expr(c, pred);
        checked = true;
    }

    for (size_t i = 0; i < *iota; i++) {
        if (const_value_eq(sw->preds[i].value, value)) {
            fprintf(stderr, Pos_Fmt "ERROR: Duplicate case ", Pos_Arg(pred->token.pos));

            static_assert(COUNT_CONST_VALUES == 6, "");
            switch (value.kind) {
            case CONST_VALUE_INT:
                if (type_kind_eq(pred->type, TYPE_CHAR)) {
                    fprintf(stderr, "'");
                    print_quoted_char(stderr, value.as.integer, '\'');
                    fprintf(stderr, "'");
                } else if (type_is_signed(pred->type)) {
                    fprintf(stderr, "%zd", value.as.integer);
                } else {
                    fprintf(stderr, "%zu", value.as.integer);
                }
                break;

            case CONST_VALUE_FN:
            case CONST_VALUE_TYPE:
            case CONST_VALUE_STRUCT:
            case CONST_VALUE_STRING:
            case CONST_VALUE_MODULE:
                unreachable();

            default:
                unreachable();
            }

            fprintf(stderr, "\n");
            fprintf(stderr, Pos_Fmt "NOTE: Already here\n", Pos_Arg(sw->preds[i].pred->token.pos));
            exit(1);
        }
    }

    sw->preds[*iota].pred = pred;
    sw->preds[*iota].value = value;
    (*iota)++;
    return value;
}

static void check_switch_exhaustive(Node_Switch *sw) {
    if (sw->enumeration) {
        if (sw->preds_count < sw->enumeration->values_count && !sw->fallback) {
            fprintf(stderr, Pos_Fmt "ERROR: This switch statement is not complete\n", Pos_Arg(sw->node.token.pos));

            fprintf(stderr, "\n");
            fprintf(stderr, "The following enumeration values are not handled:\n");
            ll_foreach(it, &sw->enumeration->values) {
                bool handled = false;
                for (size_t i = 0; i < sw->preds_count; i++) {
                    const Const_Value *pred_value = &sw->preds[i].value;
                    assert(pred_value->kind == CONST_VALUE_INT);
                    if (pred_value->as.integer == it->token.as.integer) {
                        handled = true;
                        break;
                    }
                }

                if (!handled) {
                    fprintf(stderr, "    - " SV_Fmt "\n", SV_Arg(it->token.sv));
                }
            }
            fprintf(stderr, "\n");

            fprintf(stderr, Pos_Fmt "NOTE: Enumeration defined here\n", Pos_Arg(sw->enumeration->node.token.pos));
            exit(1);
        }
    }
}

static_assert(COUNT_NODES == 27, "");
static void define_orderless_nodes(Compiler *c, Node *n, const size_t block_start) {
    switch (n->kind) {
    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;

        Node_Atom *it = NULL;
        while ((it = (Node_Atom *) node_iter((Node *) it, define->name))) {
            if (!sv_match(it->node.token.sv, "_")) {
                if (it->definition_spec->is_local) {
                    if (it->definition_spec->is_const) {
                        const Context_Fn *fn = c->context.current;

                        assert(fn->end <= c->context.locals.count);
                        assert(block_start <= c->context.locals.count);
                        assert(block_start <= fn->end);
                        for (size_t i = fn->end; i > block_start; i--) {
                            Node_Atom *previous = c->context.locals.data[i - 1];
                            if (!previous->definition_spec->is_const) {
                                continue;
                            }

                            if (sv_eq(it->node.token.sv, previous->node.token.sv)) {
                                error_redefinition((Node *) it, &previous->node.token.pos);
                                break;
                            }
                        }

                        it->definition_spec->context = c->context.current;
                        context_push_local(&c->context, it);
                    }
                } else {
                    if (get_builtin_type_kind(it->node.token.sv, NULL)) {
                        error_redefinition((Node *) it, NULL);
                    }

                    Node_Atom *previous = global_scope_find(&it->module->globals, it->node.token.sv);
                    if (previous) {
                        error_redefinition((Node *) it, &previous->node.token.pos);
                    }

                    global_scope_push(&it->module->globals, it);
                }
            }
        }
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            define_orderless_nodes(c, it, block_start);
        }
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            check_expr(c, iff->condition, REF_NONE, NULL);
            type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});

            const Const_Value value = eval_const_expr(c, iff->condition);
            iff->compile_time_real_block = value.as.integer ? iff->consequence : iff->antecedence;

            if (iff->compile_time_real_block) {
                if (iff->compile_time_real_block->kind == NODE_IF) {
                    define_orderless_nodes(c, iff->compile_time_real_block, block_start);
                } else if (iff->compile_time_real_block->kind == NODE_BLOCK) {
                    Node_Block *block = (Node_Block *) iff->compile_time_real_block;
                    for (Node *it = block->body.head; it; it = it->next) {
                        define_orderless_nodes(c, it, block_start);
                    }
                } else {
                    unreachable();
                }
            }
        }
    } break;

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            check_switch_expr_and_alloc_preds(c, sw);

            const Const_Value value = eval_const_expr(c, sw->expr);

            size_t iota = 0;
            for (Node *it = sw->cases.head; it; it = it->next) {
                Node_Case *case_ = (Node_Case *) it;
                for (Node *pred = case_->preds.head; pred; pred = pred->next) {
                    const Const_Value pred_value = check_switch_pred(c, sw, pred, &iota);
                    if (const_value_eq(pred_value, value)) {
                        sw->compile_time_real_block = case_->body;
                    }
                }
            }
            assert(iota == sw->preds_count);

            if (!sw->compile_time_real_block && sw->fallback) {
                assert(sw->fallback->kind == NODE_CASE);
                Node_Case *fallback = (Node_Case *) sw->fallback;
                sw->compile_time_real_block = fallback->body;
            }

            if (sw->compile_time_real_block) {
                assert(sw->compile_time_real_block->kind == NODE_BLOCK);
                Node_Block *block = (Node_Block *) sw->compile_time_real_block;
                for (Node *it = block->body.head; it; it = it->next) {
                    define_orderless_nodes(c, it, block_start);
                }
            }

            check_switch_exhaustive(sw);
        }
    } break;

    default:
        // Pass
        break;
    }
}

static bool is_node_caller_location(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_DIRECTIVE_CALLER_LOCATION;
}

static Node *get_node_from_group(Node *n, size_t index, i64 *group_index) {
    if (!type_kind_eq(n->type, TYPE_GROUP)) {
        if (group_index) {
            *group_index = 0;
        }
        return n;
    }

    if (n->kind == NODE_GROUP) {
        Node_Group *group = (Node_Group *) n;
        size_t      iota = 0;
        ll_foreach(it, &group->nodes) {
            size_t count = 1;
            if (type_kind_eq(it->type, TYPE_GROUP)) {
                count = it->type.spec.group.count;
            }

            const size_t start = iota;
            iota += count;
            if (iota > index) {
                if (group_index) {
                    *group_index = (i64) (index - start);
                }
                return it;
            }
        }
        unreachable();
    }

    if (n->kind == NODE_CALL) {
        assert(index < n->type.spec.group.count);
        *group_index = index;
    }

    return n;
}

static void check_definition(Compiler *c, Node_Atom *it, Node *it_expr, Node *type) {
    assert(it->definition_spec->check_status != CHECKING); // It is already checked
    if (it->definition_spec->check_status == CHECKED) {
        return;
    }
    it->definition_spec->check_status = CHECKING;

    if (type) {
        if (type_kind_eq(type->type, TYPE_UNIT)) {
            check_expr(c, type, REF_NONE, NULL);
            type_assert_type(type);
            type->type.is_meta = false;
        }
        it->node.type = type->type;
    }

    if (it_expr) {
        Node_Define *definition = it->definition_spec->definition_node;

        if (type_kind_eq(it_expr->type, TYPE_UNIT)) {
            if (it->definition_spec->arg_index && is_node_caller_location(it_expr)) {
                // TODO: There should be a more sophisticated type for this, something like `Source_Code_Location`
                // maybe?
                it_expr->type = (Type) {.kind = TYPE_STRING};
            } else {
                if (it->definition_spec->is_const) {
                    assert(it_expr);
                    if (it_expr->kind == NODE_DISTINCT) {
                        Node_Distinct *distinct = (Node_Distinct *) it_expr;
                        distinct->defined_as = it;
                    }
                }

                check_expr(c, it_expr, REF_NONE, type ? &type->type : NULL);
                if (!type) {
                    if (it_expr->kind == NODE_GROUP) {
                        Node_Group *group = (Node_Group *) it_expr;
                        ll_foreach(it, &group->nodes) {
                            check_that_type_is_known(it);
                        }
                    } else {
                        check_that_type_is_known(it_expr);
                    }
                }

                const bool is_it_a_module = type_kind_eq(it_expr->type, TYPE_MODULE) && !it->definition_spec->is_const;
                const bool is_it_a_type = it_expr->type.is_meta && !it->definition_spec->is_const;
                if (is_it_a_module || is_it_a_type) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot store %s in a %s\n",
                        Pos_Arg(it_expr->token.pos),
                        type_to_cstr(it_expr->type),
                        it->definition_spec->is_const ? "constant" : "variable");

                    exit(1);
                }
            }
        }

        bool type_determined = false;
        if (!definition->is_value_known_at_compile_time) {
            size_t lhs_count = definition->count;
            size_t rhs_count = 1;
            if (type_kind_eq(it_expr->type, TYPE_GROUP)) {
                rhs_count = it_expr->type.spec.group.count;
            }

            if (lhs_count != rhs_count) {
                error_number_of_values_mismatch(
                    definition->node.token.pos,
                    lhs_count,
                    rhs_count,
                    add_trailing_s_if_plural("definition", lhs_count),
                    add_trailing_s_if_plural("assignment", rhs_count));
            }

            if (type_kind_eq(it_expr->type, TYPE_GROUP)) {
                assert(it->definition_spec->group_index < it_expr->type.spec.group.count);

                if (type) {
                    i64   group_index = -1;
                    Node *n = get_node_from_group(it_expr, it->definition_spec->group_index, &group_index);
                    type_assert_grouped(c, n, it->node.type, group_index, &it->node.token.pos);
                } else {
                    it->node.type = it_expr->type.spec.group.data[it->definition_spec->group_index];
                    node_finalize_type_of_unknown(&it->node.type);
                }

                type_determined = true;
            }
        }

        if (!type_determined) {
            if (type) {
                type_assert(c, it_expr, it->node.type);
            } else {
                if (!it->definition_spec->is_const) {
                    node_finalize_type_of_unknown(&it_expr->type);
                }
                it->node.type = it_expr->type;
            }
        }
    }

    if (it_expr && it->definition_spec->definition_node->is_value_known_at_compile_time) {
        if (!it->definition_spec->is_const_value_evaluated) {
            it->definition_spec->const_value = eval_const_expr(c, it_expr);
            it->definition_spec->is_const_value_evaluated = true;
        }
    }

    if (it->definition_spec->is_local) {
        if (!it->definition_spec->is_const && !sv_match(it->node.token.sv, "_")) {
            context_push_local(&c->context, it);
        }
    }

    it->definition_spec->check_status = CHECKED;
}

static void check_ident(Compiler *c, Node *n, Ref_Kind ref) {
    Node_Atom   *atom = NULL;
    Node_Member *member = NULL;

    Token   token = {0};
    Module *module = NULL;
    bool    importing = false;
    if (n->kind == NODE_ATOM) {
        atom = (Node_Atom *) n;
        token = n->token;
        module = atom->module;
    } else if (n->kind == NODE_MEMBER) {
        member = (Node_Member *) n;
        assert(member->lhs->type.kind == TYPE_MODULE);

        token = member->field;
        module = member->lhs->type.spec.module;
        importing = true;
    } else {
        unreachable();
    }

    if (sv_match(token.sv, "_")) {
        fprintf(stderr, Pos_Fmt "ERROR: Identifier '_' cannot be used as a value\n", Pos_Arg(token.pos));
        exit(1);
    }

    Node_Atom *definition = NULL;
    if (atom) {
        definition = context_find_local(&c->context, token.sv);
    }

    if (!definition) {
        definition = global_scope_find(&module->globals, token.sv);
        if (!definition && atom) {
            module = c->builtin_module;
            importing = true;
            definition = global_scope_find(&module->globals, token.sv);
        }

        if (definition && definition->definition_spec->is_private && importing) {
            definition = NULL;
        }
    }

    if (atom) {
        atom->definition = definition;
    } else if (member) {
        member->module_access_definition = definition;
    }

    if (definition) {
        switch (definition->definition_spec->check_status) {
        case UNCHECKED: {
            Context_Fn *context_fn_save = c->context.current;
            c->context.current = definition->definition_spec->context;

            // Only orderless definitions can be uninffered, and the assignment of such definitions must be constant
            assert(definition->definition_spec->definition_node->is_value_known_at_compile_time);

            check_definition(
                c,
                definition,
                definition->definition_spec->assignment_node,
                definition->definition_spec->definition_node->type);
            context_restore_fn(&c->context, context_fn_save);
        } break;

        case CHECKING:
            if (ref == REF_ADDR && definition->node.type.is_meta) {
                // Reference to incomplete type definition is allowed
            } else {
                fprintf(stderr, Pos_Fmt "ERROR: Cyclic definition\n", Pos_Arg(definition->node.token.pos));
                exit(1);
            }
            break;

        case CHECKED:
            // Pass
            break;
        }

        n->type = definition->node.type;
        if (definition->definition_spec->is_const) {
            switch (ref) {
            case REF_NONE:
                // OK
                break;

            case REF_ADDR:
                if (!n->type.is_meta) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot take reference to compile time constant value\n",
                        Pos_Arg(token.pos));
                    exit(1);
                }
                break;

            case REF_ASSIGN:
                fprintf(stderr, Pos_Fmt "ERROR: Cannot assign to compile time constant value\n", Pos_Arg(token.pos));
                exit(1);
                break;
            }
        }
    } else {
        if (atom) {
            Type_Kind kind;
            if (get_builtin_type_kind(token.sv, &kind)) {
                n->type = (Type) {.kind = kind, .is_meta = true};
                return;
            }
        }

        error_undefined(&token, "identifier", false);
    }
}

static void check_assignment_lhs_for_arithmetics(Node *n, Token_Kind op) {
    switch (op) {
    case TOKEN_ADD_SET:
    case TOKEN_SUB_SET:
        type_assert_numeric(n, true);
        break;

    case TOKEN_MUL_SET:
    case TOKEN_DIV_SET:
    case TOKEN_MOD_SET:
        type_assert_numeric(n, false);
        break;

    case TOKEN_SHL_SET:
    case TOKEN_SHR_SET:
    case TOKEN_BOR_SET:
    case TOKEN_BAND_SET:
        type_assert_numeric(n, false);
        break;

    default:
        // Pass
        break;
    }
}

static void check_assignment(Compiler *c, Node_Binary *binary) {
    check_expr(c, binary->lhs, REF_ASSIGN, NULL);
    check_expr(c, binary->rhs, REF_NONE, &binary->lhs->type);

    const bool is_lhs_group = type_kind_eq(binary->lhs->type, TYPE_GROUP);
    const bool is_rhs_group = type_kind_eq(binary->rhs->type, TYPE_GROUP);

    const size_t lhs_count = is_lhs_group ? binary->lhs->type.spec.group.count : 1;
    const size_t rhs_count = is_rhs_group ? binary->rhs->type.spec.group.count : 1;
    if (lhs_count != rhs_count) {
        error_number_of_values_mismatch(binary->node.token.pos, lhs_count, rhs_count, NULL, NULL);
    }

    if (is_lhs_group) {
        assert(is_rhs_group);
        for (size_t i = 0; i < lhs_count; i++) {
            i64   lhs_group_index = -1;
            Node *lhs = get_node_from_group(binary->lhs, i, &lhs_group_index);
            i64   rhs_group_index = -1;
            Node *rhs = get_node_from_group(binary->rhs, i, &rhs_group_index);
            type_assert_grouped(c, rhs, lhs->type, rhs_group_index, &lhs->token.pos);
            check_assignment_lhs_for_arithmetics(lhs, binary->node.token.kind);
        }
    } else {
        type_assert(c, binary->rhs, binary->lhs->type);
        check_assignment_lhs_for_arithmetics(binary->lhs, binary->node.token.kind);
    }

    binary->node.type = (Type) {.kind = TYPE_UNIT};
}

// If this is a cast, then do not pass 'fn_type_spec'
static void check_call_arguments(Compiler *c, Node_Call *call, const Type_Fn *fn_type_spec) {
    size_t args_count_min = 1;
    size_t args_count_max = 1;
    if (fn_type_spec) {
        args_count_min = fn_type_spec->args_count_min;
        args_count_max = fn_type_spec->is_variadic ? UINT64_MAX : fn_type_spec->args_count;
    }

    ll_foreach(it, &call->args) {
        const Type *arg_expected_type = NULL;
        if (fn_type_spec && call->args_count < args_count_max) {
            arg_expected_type = &fn_type_spec->args[call->args_count].type;
        }

        check_expr(c, it, REF_NONE, arg_expected_type);
        check_that_type_is_known(it);
        call->args_count += type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
    }

    bool   has_minimum = true;
    bool   has_maximum = true;
    size_t expected = 0;

    const char *situation = "";
    const char *extra = "";

    if (call->args_count < args_count_min) {
        has_minimum = false;
        expected = args_count_min;
        situation = "Not enough";
        extra = " atleast";
    }

    if (call->args_count > args_count_max) {
        has_maximum = false;
        expected = args_count_max;
        situation = "Too many";
        // Not setting the extra here, since that situation does not exist
    }

    if (has_minimum && has_maximum) {
        return;
    }

    if (args_count_min == args_count_max) {
        extra = "";
    }

    Pos pos = call->end;
    if (!has_maximum) {
        size_t iota = 0;
        ll_foreach(it, &call->args) {
            iota += type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
            if (iota > args_count_max) {
                pos = it->token.pos;
                break;
            }
        }
    }

    fprintf(
        stderr,
        Pos_Fmt "ERROR: %s arguments: Expected%s %zu, got %zu\n",
        Pos_Arg(pos),
        situation,
        extra,
        expected,
        call->args_count);
    exit(1);
}

// The argument 'expected_type' is a hint in order to infer the types of implicit expressions. Checking against it is
// NOT the responsibility of this function.
static_assert(COUNT_NODES == 27, "");
static void check_expr(Compiler *c, Node *n, Ref_Kind ref, const Type *expected_type) {
    if (!n) {
        return;
    }

    bool is_ref_valid = false;
    switch (n->kind) {
    case NODE_ATOM: {
        static_assert(COUNT_TOKENS == 72, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_INT};
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_CHAR:
            n->type = (Type) {.kind = TYPE_CHAR};
            break;

        case TOKEN_NULL:
            n->type = (Type) {.kind = TYPE_RAWPTR};
            break;

        case TOKEN_IDENT:
            check_ident(c, n, ref);
            is_ref_valid = true; // check_ident() has already checked whether the reference is valid
            break;

        case TOKEN_STRING:
            n->type = (Type) {.kind = TYPE_STRING};
            break;

        case TOKEN_DIRECTIVE_MAIN:
            n->type = c->main_fn_type;
            break;

        case TOKEN_DIRECTIVE_PLATFORM:
            get_platform(c, &n->type);
            break;

        case TOKEN_DIRECTIVE_CALLER_LOCATION:
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot use %s here. It can only be used as the default value for a function argument\n",
                Pos_Arg(n->token.pos),
                token_kind_to_cstr(n->token.kind));
            exit(1);

        default:
            unreachable();
        }
    } break;

    case NODE_GROUP: {
        Node_Group *group = (Node_Group *) n;

        Type_Group spec = {0};
        ll_foreach(it, &group->nodes) {
            const Type *it_expected_type = NULL;
            if (expected_type && type_kind_eq(*expected_type, TYPE_GROUP)) {
                const Type_Group expected_spec = expected_type->spec.group;
                if (spec.count < expected_spec.count) {
                    it_expected_type = &expected_spec.data[spec.count];
                }
            }

            check_expr(c, it, ref, it_expected_type);
            if (type_kind_eq(it->type, TYPE_GROUP)) {
                spec.count += it->type.spec.group.count;
            } else {
                spec.count++;
            }
        }

        spec.data = arena_alloc(c->arena, spec.count * sizeof(*spec.data));

        size_t iota = 0;
        ll_foreach(it, &group->nodes) {
            if (type_kind_eq(it->type, TYPE_GROUP)) {
                for (size_t i = 0; i < it->type.spec.group.count; i++) {
                    spec.data[iota++] = it->type.spec.group.data[i];
                }
            } else {
                spec.data[iota++] = it->type;
            }
        }

        n->type = (Type) {.kind = TYPE_GROUP, .spec.group = spec};
        is_ref_valid = true;
    } break;

    case NODE_GHOST:
        unreachable();

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        static_assert(COUNT_TOKENS == 72, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_expr(c, unary->value, REF_NONE, expected_type);
            n->type = type_assert_numeric(unary->value, false);
            break;

        case TOKEN_MUL: {
            bool checked = false;
            if (expected_type) {
                Type expected_type_referenced = *expected_type;
                if (!expected_type_referenced.is_meta) {
                    expected_type_referenced.ref++;
                    check_expr(c, unary->value, REF_NONE, &expected_type_referenced);
                    checked = true;
                }
            }

            if (!checked) {
                check_expr(c, unary->value, REF_NONE, NULL);
            }
            check_that_type_is_known(unary->value);

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
            if (n->type.distinct && n->type.distinct->node.type.ref > n->type.ref) {
                n->type.distinct = NULL;
            }

            is_ref_valid = true;
        } break;

        case TOKEN_BAND: {
            bool checked = false;
            if (expected_type) {
                Type expected_type_referenced = *expected_type;
                if (!expected_type_referenced.is_meta && expected_type_referenced.ref) {
                    expected_type_referenced.ref--;
                    check_expr(c, unary->value, REF_ADDR, &expected_type_referenced);
                    checked = true;
                }
            }

            if (!checked) {
                check_expr(c, unary->value, REF_ADDR, NULL);
            }

            check_that_type_is_known(unary->value);
            n->type = unary->value->type;
            n->type.ref++;
        } break;

        case TOKEN_BNOT:
            check_expr(c, unary->value, REF_NONE, expected_type);
            n->type = type_assert_numeric(unary->value, false);
            break;

        case TOKEN_LNOT:
            check_expr(c, unary->value, REF_NONE, expected_type);
            n->type = type_assert(c, unary->value, (Type) {.kind = TYPE_BOOL});
            break;

        case TOKEN_SIZEOF:
            check_expr(c, unary->value, REF_NONE, NULL);
            check_that_type_is_known(unary->value);
            n->type = (Type) {.kind = TYPE_INT};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        static_assert(COUNT_TOKENS == 72, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
            check_expr(c, binary->lhs, REF_NONE, expected_type);
            check_expr(c, binary->rhs, REF_NONE, expected_type);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = type_assert_numeric(binary->lhs, true);
            break;

        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
            check_expr(c, binary->lhs, REF_NONE, expected_type);
            check_expr(c, binary->rhs, REF_NONE, expected_type);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = type_assert_numeric(binary->lhs, false);
            break;

        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
            check_expr(c, binary->lhs, REF_NONE, expected_type);
            check_expr(c, binary->rhs, REF_NONE, expected_type);
            type_assert_node(c, binary->rhs, binary->lhs);
            n->type = type_assert_numeric(binary->lhs, false);
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
            check_expr(c, binary->lhs, REF_NONE, NULL);
            check_expr(c, binary->rhs, REF_NONE, &binary->lhs->type);
            type_assert_node(c, binary->rhs, binary->lhs);
            type_assert_numeric(binary->lhs, true);
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_EQ:
        case TOKEN_NE:
            check_expr(c, binary->lhs, REF_NONE, NULL);
            check_expr(c, binary->rhs, REF_NONE, &binary->lhs->type);
            type_assert_node(c, binary->rhs, binary->lhs);
            check_that_type_is_known(binary->lhs);
            if (!type_is_scalar(binary->lhs->type) && !type_eq(binary->lhs->type, (Type) {.kind = TYPE_STRING})) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Expected scalar or string value, got %s\n",
                    Pos_Arg(binary->lhs->token.pos),
                    type_to_cstr(binary->lhs->type));
                exit(1);
            }
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_SET:
        case TOKEN_ADD_SET:
        case TOKEN_SUB_SET:
        case TOKEN_MUL_SET:
        case TOKEN_DIV_SET:
        case TOKEN_MOD_SET:
        case TOKEN_SHL_SET:
        case TOKEN_SHR_SET:
        case TOKEN_BOR_SET:
        case TOKEN_BAND_SET:
            check_assignment(c, binary);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        if (sv_match(member->field.sv, "_")) {
            fprintf(stderr, Pos_Fmt "ERROR: Field '_' cannot be accessed\n", Pos_Arg(member->field.pos));
            exit(1);
        }

        if (member->lhs) {
            check_expr(c, member->lhs, ref, NULL);
            check_that_type_is_known(member->lhs);

            is_ref_valid = true; // check_node() has already determined that the reference is valid

            if (member->lhs->type.is_meta && member->lhs->type.kind == TYPE_ENUM) {
                Node_Enum *enumm = member->lhs->type.spec.enumm.definition;
                member->enum_value = enum_get_value(enumm, member->field.sv, &member->field);
                member->is_enum = true;
                n->type = member->lhs->type;
                n->type.is_meta = false;
            } else if (type_kind_eq(member->lhs->type, TYPE_STRUCT)) {
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
                    error_undefined(&member->field, "field", true);
                    fprintf(
                        stderr, Pos_Fmt "NOTE: Structure defined here\n", Pos_Arg(spec->definition->node.token.pos));
                    exit(1);
                }

                n->type = definition->type;
            } else if (type_kind_eq(member->lhs->type, TYPE_ARRAY)) {
                if (sv_match(member->field.sv, "data")) {
                    n->type = *member->lhs->type.spec.array.element;
                    n->type.ref++;
                    member->field_index = 0;
                } else if (sv_match(member->field.sv, "count")) {
                    n->type = (Type) {.kind = TYPE_I64};
                    member->field_index = 1;
                } else {
                    error_undefined(&member->field, "field", false);
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_SLICE)) {
                if (sv_match(member->field.sv, "data")) {
                    n->type = *member->lhs->type.spec.slice.element;
                    n->type.ref++;
                    member->field_index = 0;
                } else if (sv_match(member->field.sv, "count")) {
                    n->type = (Type) {.kind = TYPE_I64};
                    member->field_index = 1;
                } else {
                    error_undefined(&member->field, "field", false);
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_STRING)) {
                if (sv_match(member->field.sv, "data")) {
                    n->type = (Type) {.kind = TYPE_CHAR, .ref = 1};
                    member->field_index = 0;
                } else if (sv_match(member->field.sv, "count")) {
                    n->type = (Type) {.kind = TYPE_I64};
                    member->field_index = 1;
                } else {
                    error_undefined(&member->field, "field", false);
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_MODULE)) {
                check_ident(c, n, ref);
            } else {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot access field of %s\n",
                    Pos_Arg(n->token.pos),
                    type_to_cstr(member->lhs->type));
                exit(1);
            }
        } else {
            n->type = (Type) {.kind = TYPE_UNKNOWN_ENUM};
            member->is_enum = true;

            if (expected_type && type_kind_eq(*expected_type, TYPE_ENUM)) {
                Node_Enum *enumm = expected_type->spec.enumm.definition;
                member->enum_value = enum_get_value(enumm, member->field.sv, &member->field);
                n->type = *expected_type;
            }
        }
    } break;

    case NODE_IMPORT: {
        Node_Import *import = (Node_Import *) n;
        if (import->libraries.head) {
            ll_foreach(it, &import->libraries) {
                link_flags_add_libname(c->link_flags, it->token.sv);
            }
        } else {
            if (!import->module) {
                parser_import(c->parser, import);

                const Context context_save = c->context;
                memset(&c->context, 0, sizeof(c->context));
                {
                    for (Node *it = import->module->nodes.head; it; it = it->next) {
                        define_orderless_nodes(c, it, 0);
                    }
                }
                c->context = context_save;
            }
        }
        n->type = (Type) {.kind = TYPE_MODULE, .spec.module = import->module};
    } break;

    case NODE_DISTINCT: {
        Node_Distinct *distinct = (Node_Distinct *) n;
        if (!distinct->defined_as) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: A distinct type must be defined as a constant before it can be used\n",
                Pos_Arg(n->token.pos));
            exit(1);
        }

        check_expr(c, distinct->value, REF_NONE, NULL);
        type_assert_type(distinct->value);
        n->type = distinct->value->type;
        n->type.distinct = distinct->defined_as;
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;

        Context_Fn context_fn = {.fn = fn, .outer = c->context.current};
        context_push_fn(&c->context, &context_fn);

        {
            Type_Fn fn_type_spec = {
                .args = arena_alloc(c->arena, fn->args_count * sizeof(*fn_type_spec.args)),
                .args_count_min = fn->args_count_min,
                .is_variadic = fn->is_variadic,
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
                            error_redefinition((Node *) it, &previous.pos);
                        }
                    }
                }

                fn_type_spec.args[fn_type_spec.args_count].name = it->node.token.sv;
                fn_type_spec.args[fn_type_spec.args_count].pos = it->node.token.pos;

                check_stmt(c, arg);

                if (define->expr) {
                    if (is_node_caller_location(define->expr)) {
                        fn_type_spec.args[fn_type_spec.args_count].default_value_is_caller_location = true;
                    } else {
                        it->definition_spec->const_value = eval_const_expr(c, define->expr);
                        fn_type_spec.args[fn_type_spec.args_count].default_value = &it->definition_spec->const_value;
                    }
                }
                fn_type_spec.args[fn_type_spec.args_count].type = it->node.type;

                fn_type_spec.args_count += define->count;
            }

            if (fn->returns.head) {
                fn_type_spec.returns = arena_alloc(c->arena, fn->returns_count * sizeof(*fn_type_spec.returns));

                size_t iota = 0;
                ll_foreach(it, &fn->returns) {
                    check_expr(c, it, REF_NONE, NULL);
                    type_assert_type(it);

                    fn_type_spec.returns[iota] = it->type;
                    fn_type_spec.returns[iota].is_meta = false;
                    iota++;
                }
            }
            fn_type_spec.returns_count = fn->returns_count;

            Type return_type = {0};
            if (fn_type_spec.returns_count == 0) {
                return_type.kind = TYPE_UNIT;
            } else if (fn_type_spec.returns_count == 1) {
                return_type = *fn_type_spec.returns;
            } else {
                return_type.kind = TYPE_GROUP;
                return_type.spec.group.data = fn_type_spec.returns;
                return_type.spec.group.count = fn_type_spec.returns_count;
            }
            fn_type_spec.return_type = arena_clone(c->arena, &return_type, sizeof(return_type));

            n->type = (Type) {.kind = TYPE_FN, .spec.fn = fn_type_spec};

            if (fn->defined_as) {
                // The body of a function is irrelevant for outer expressions
                fn->defined_as->node.type = n->type;
                fn->defined_as->definition_spec->check_status = CHECKED;
            }

            if (fn->is_type) {
                n->type.is_meta = true;
                is_ref_valid = ref == REF_ADDR;
            } else if (fn->body) {
                check_stmt(c, fn->body);
                if (fn_type_spec.returns_count && !always_returns(fn->body)) {
                    assert(fn->body->kind == NODE_BLOCK);
                    const Pos end = ((Node_Block *) fn->body)->end;
                    fprintf(stderr, Pos_Fmt "ERROR: Expected return statement\n", Pos_Arg(end));
                    exit(1);
                }
            }
        }

        context_pop_fn(&c->context);
    } break;

    case NODE_ENUM: {
        Node_Enum *enumm = (Node_Enum *) n;

        Type_Enum spec = {.underlying = TYPE_INT, .definition = enumm};
        Type      underlying = {.kind = spec.underlying};
        if (enumm->underlying) {
            check_expr(c, enumm->underlying, REF_NONE, NULL);
            type_assert_type(enumm->underlying);

            underlying = enumm->underlying->type;
            underlying.is_meta = false;
            if (!type_is_integer(underlying)) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Expected underlying type of the enumeration to be an integer, got %s\n",
                    Pos_Arg(enumm->underlying->token.pos),
                    type_to_cstr(underlying));
                exit(1);
            }

            spec.underlying = underlying.kind;
        }

        i64 iota = 0;
        i64 iota_max = 0;
        ll_foreach(it, &enumm->values) {
            ll_foreach(prev, &enumm->values) {
                if (prev == it) {
                    break;
                }

                if (sv_eq(it->token.sv, prev->token.sv)) {
                    error_redefinition(it, &prev->token.pos);
                }
            }

            assert(it->kind == NODE_UNARY);
            Node_Unary *unary = (Node_Unary *) it;
            if (unary->value) {
                check_expr(c, unary->value, REF_NONE, NULL);
                type_assert(c, unary->value, underlying);

                const Const_Value value = eval_const_expr(c, unary->value);
                assert(value.kind == CONST_VALUE_INT);
                iota = value.as.integer;
            }
            iota_max = max(iota_max, iota);

            it->type.kind = underlying.kind;
            check_int_limit(it, &iota);
            it->type.kind = TYPE_UNIT;
            it->token.as.integer = iota++;
        }

        // TODO: Should the type be shortened to the minimum integer to fit this?
        n->type = (Type) {.kind = TYPE_ENUM, .is_meta = true, .spec.enumm = spec};
    } break;

    case NODE_STRUCT: {
        Node_Struct *structt = (Node_Struct *) n;

        Type_Struct *spec = arena_alloc(c->arena, sizeof(*spec));
        spec->definition = structt;

        n->type = (Type) {
            .kind = TYPE_STRUCT,
            .is_meta = true,
            .spec.structt = spec,
        };

        if (structt->defined_as) {
            structt->defined_as->node.type = n->type;
        }

        const size_t fields_start = c->struct_fields.count;

        size_t fields_count = 0;
        ll_foreach(field, &structt->fields) {
            if (field->kind == NODE_DEFINE) {
                Node_Define *define = (Node_Define *) field;

                Node_Atom *it = NULL;
                while ((it = (Node_Atom *) node_iter((Node *) it, define->name))) {
                    if (!sv_match(it->node.token.sv, "_")) {
                        for (size_t i = 0; i < fields_count; i++) {
                            Type_Struct_Field previous = c->struct_fields.data[fields_start + i];
                            if (sv_eq(previous.name, it->node.token.sv)) {
                                error_redefinition((Node *) it, &previous.pos);
                            }
                        }
                    }

                    Type_Struct_Field it_field = {.name = it->node.token.sv, .pos = it->node.token.pos};
                    da_push(&c->struct_fields, it_field);
                    fields_count++;
                }

                check_expr(c, define->type, REF_NONE, NULL);
                type_assert_type(define->type);
                while ((it = (Node_Atom *) node_iter((Node *) it, define->name))) {
                    it->node.type = define->type->type;
                    it->node.type.is_meta = false;
                }
            } else if (field->kind == NODE_UNARY && field->token.kind == TOKEN_SPREAD) {
                Node_Unary *unary = (Node_Unary *) field;
                check_expr(c, unary->value, REF_NONE, NULL);
                type_assert_type(unary->value);

                Type from = unary->value->type;
                from.is_meta = false;
                if (!type_kind_eq(from, TYPE_STRUCT)) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Expected structure type, got %s\n",
                        Pos_Arg(unary->value->token.pos),
                        type_to_cstr(from));
                    exit(1);
                }

                if (from.ref) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot spread %s without dereferencing it first\n",
                        Pos_Arg(unary->value->token.pos),
                        type_to_cstr(from));
                    exit(1);
                }

                for (size_t i = 0; i < from.spec.structt->fields_count; i++) {
                    Type_Struct_Field it = from.spec.structt->fields[i];
                    if (!sv_match(it.name, "_")) {
                        for (size_t i = 0; i < fields_count; i++) {
                            Type_Struct_Field previous = c->struct_fields.data[fields_start + i];
                            if (sv_eq(previous.name, it.name)) {
                                fprintf(
                                    stderr,
                                    Pos_Fmt "ERROR: While spreading this structure, we encountered a field '" SV_Fmt
                                            "' that is already defined\n",
                                    Pos_Arg(unary->value->token.pos),
                                    SV_Arg(it.name));
                                fprintf(stderr, Pos_Fmt "NOTE: Defined here\n", Pos_Arg(previous.pos));
                                exit(1);
                            }
                        }
                    }

                    it.pos = unary->value->token.pos;
                    da_push(&c->struct_fields, it);
                    fields_count++;
                }
            } else {
                unreachable();
            }
        }

        if (fields_count) {
            spec->fields = arena_clone(
                c->arena, &c->struct_fields.data[fields_start], fields_count * sizeof(*c->struct_fields.data));
            spec->fields_count = fields_count;

            size_t iota = 0;
            ll_foreach(field, &structt->fields) {
                if (field->kind == NODE_DEFINE) {
                    Node_Define *define = (Node_Define *) field;

                    Node_Atom *it = NULL;
                    while ((it = (Node_Atom *) node_iter((Node *) it, define->name))) {
                        spec->fields[iota++].type = it->node.type;
                    }
                } else if (field->kind == NODE_UNARY && field->token.kind == TOKEN_SPREAD) {
                    Node_Unary *unary = (Node_Unary *) field;
                    assert(unary->value->type.kind == TYPE_STRUCT);
                    iota += unary->value->type.spec.structt->fields_count;
                } else {
                    unreachable();
                }
            }
        }

        c->struct_fields.count = fields_start;
        is_ref_valid = ref == REF_ADDR;
    } break;

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        if (compound->lhs) {
            check_expr(c, compound->lhs, REF_NONE, NULL);
            type_assert_type(compound->lhs);

            n->type = compound->lhs->type;
            n->type.is_meta = false;
            if (n->type.ref ||
                (n->type.kind != TYPE_STRUCT && n->type.kind != TYPE_ARRAY && n->type.kind != TYPE_SLICE)) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Expected structure or array type, got %s\n",
                    Pos_Arg(compound->lhs->token.pos),
                    type_to_cstr(n->type));
                exit(1);
            }
        } else {
            n->type = (Type) {.kind = TYPE_UNKNOWN_COMPOUND};
            if (expected_type && type_kind_eq(*expected_type, TYPE_STRUCT)) {
                n->type = *expected_type;
            }
        }

        // TODO: This should not error out immediately
        //
        // Currently there is no mechanism which can allow us to test implicit compounds, since expected type context
        // exists. After custom operators are implemented, only then can this be tested
        check_that_type_is_known(n);

        // For structure literal
        Type_Struct *struct_spec = NULL;
        if (n->type.kind == TYPE_STRUCT) {
            struct_spec = n->type.spec.structt;
        }

        size_t array_count = 0;
        size_t ordered_iota = 0;
        for (Node *iter = compound->children.head; iter; iter = iter->next) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            Node *it = iter;
            if (compound->is_designated) {
                assert(it->kind == NODE_BINARY && it->token.kind == TOKEN_SET);
                Node_Binary *it_binary = (Node_Binary *) it;

                if (n->type.kind == TYPE_STRUCT) {
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
                        error_undefined(&it_field_name->node.token, "field", true);
                        fprintf(
                            stderr,
                            Pos_Fmt "NOTE: Structure defined here\n",
                            Pos_Arg(struct_spec->definition->node.token.pos));
                        exit(1);
                    }
                } else if (n->type.kind == TYPE_ARRAY || n->type.kind == TYPE_SLICE) {
                    check_expr(c, it_binary->lhs, REF_NONE, NULL);
                    type_assert_numeric(it_binary->lhs, false);

                    const Const_Value value = eval_const_expr(c, it_binary->lhs);
                    assert(value.kind == CONST_VALUE_INT);

                    if (n->type.kind == TYPE_ARRAY && (size_t) value.as.integer >= n->type.spec.array.count) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Index %zd is out of bounds in array of length %zu\n",
                            Pos_Arg(it_binary->lhs->token.pos),
                            value.as.integer,
                            n->type.spec.array.count);
                        exit(1);
                    }

                    it->token.as.integer = value.as.integer;
                } else {
                    unreachable();
                }

                it_iota = it->token.as.integer;
                it = it_binary->rhs;
            } else {
                if (n->type.kind == TYPE_STRUCT) {
                    if (it_iota >= struct_spec->fields_count) {
                        fprintf(stderr, Pos_Fmt "ERROR: Too many ordered initializers\n", Pos_Arg(it->token.pos));
                        exit(1);
                    }
                } else if (n->type.kind == TYPE_ARRAY) {
                    if (it_iota >= n->type.spec.array.count) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Index %zu is out of bounds in array of length %zu\n",
                            Pos_Arg(it->token.pos),
                            it_iota,
                            n->type.spec.array.count);
                        exit(1);
                    }
                } else if (n->type.kind == TYPE_SLICE) {
                    // Pass
                } else {
                    unreachable();
                }
            }

            const Type *it_type = NULL;
            if (n->type.kind == TYPE_STRUCT) {
                it_type = &struct_spec->fields[it_iota].type;
            } else if (n->type.kind == TYPE_ARRAY) {
                it_type = n->type.spec.array.element;
            } else if (n->type.kind == TYPE_SLICE) {
                it_type = n->type.spec.slice.element;
                array_count = max(array_count, it_iota + 1);
            } else {
                unreachable();
            }

            check_expr(c, it, REF_NONE, it_type);
            type_assert(c, it, *it_type);
        }

        if (n->type.kind == TYPE_SLICE) {
            Type *element = n->type.spec.slice.element;
            n->type.spec.array.element = element;
            n->type.spec.array.count = array_count;
            n->type.kind = TYPE_ARRAY;
        }

        compound->memory_type = &n->type;
        is_ref_valid = ref == REF_ADDR;
    } break;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        check_expr(c, call->fn, REF_NONE, NULL);
        check_that_type_is_known(call->fn);

        const Type fn_type = call->fn->type;
        if (fn_type.is_meta) {
            call->is_type_cast = true;
            n->type = fn_type;
            n->type.is_meta = false;

            check_call_arguments(c, call, NULL);
            const Type from_type = call->args.head->type;
            const Type to_type = n->type;

            bool same = false;
            if (type_eq_without_distinct(to_type, from_type)) {
                same = true;
            } else if (!type_is_scalar(to_type)) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot cast to %s\n", Pos_Arg(call->fn->token.pos), type_to_cstr(to_type));
                exit(1);
            }

            if (!same) {
                if (type_is_scalar(to_type)) {
                    type_assert_scalar(call->args.head);

                    bool ok = true;
                    if (type_kind_eq(from_type, TYPE_FN) && !from_type.ref) {
                        // fn -> rawptr
                        ok = type_eq(to_type, (Type) {.kind = TYPE_RAWPTR});
                    } else if (type_kind_eq(to_type, TYPE_FN) && !to_type.ref) {
                        // rawptr -> fn
                        ok = type_eq(from_type, (Type) {.kind = TYPE_RAWPTR});
                    } else if (!type_is_pointer(from_type) && type_is_pointer(to_type)) {
                        // i64/u64 -> ptr
                        if (!type_kind_eq(from_type, TYPE_I64) && !type_kind_eq(from_type, TYPE_U64) &&
                            !type_kind_eq(from_type, TYPE_INT)) {
                            ok = false;
                        }
                    } else if (type_is_pointer(from_type) && !type_is_pointer(to_type)) {
                        // ptr -> i64/u64
                        if (!type_kind_eq(to_type, TYPE_I64) && !type_kind_eq(to_type, TYPE_U64) &&
                            !type_kind_eq(to_type, TYPE_INT)) {
                            ok = false;
                        }
                    }

                    if (!ok) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Cannot cast %s to %s\n",
                            Pos_Arg(call->fn->token.pos),
                            type_to_cstr(from_type),
                            type_to_cstr(to_type));
                        exit(1);
                    }
                } else {
                    unreachable();
                }
            }

            if (same) {
                call->type_cast = TYPE_CAST_NOP;
            } else if (type_eq(to_type, (Type) {.kind = TYPE_BOOL})) {
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
            const Type_Fn fn_type_spec = fn_type.spec.fn;

            check_call_arguments(c, call, &fn_type_spec);

            size_t iota = 0;
            bool   check_args = true;
            for (Node *arg = call->args.head; arg; arg = arg->next) {
                const bool   is_group = type_kind_eq(arg->type, TYPE_GROUP);
                const size_t arg_parts = is_group ? arg->type.spec.group.count : 1;

                for (size_t i = 0; i < arg_parts; i++) {
                    if (iota >= fn_type_spec.args_count) {
                        check_args = false;
                    }

                    if (check_args) {
                        type_assert_grouped(c, arg, fn_type_spec.args[iota].type, i, NULL);
                    }

                    iota++;
                }
            }

            for (size_t i = call->args_count; i < fn_type_spec.args_count; i++) {
                const Token ghost_token = {.pos = call->fn->token.pos};
                Node_Ghost *ghost = (Node_Ghost *) node_alloc(c->arena, NODE_GHOST, ghost_token);
                ghost->arg = &fn_type_spec.args[i];
                ghost->node.type = fn_type_spec.args[i].type;

                nodes_push(&call->args, (Node *) ghost);
                call->args_count++;
            }

            n->type = *fn_type_spec.return_type;
            if (!call->is_stmt && type_kind_eq(n->type, TYPE_UNIT)) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: This call cannot be used as a value as it does not return anything\n",
                    Pos_Arg(n->token.pos));
                exit(1);
            }
        }
    } break;

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;
        check_expr(c, index->lhs, ref, NULL);
        check_that_type_is_known(index->lhs);
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
                    check_expr(c, index->a, REF_NONE, NULL);
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

                check_expr(c, index->b, REF_NONE, NULL);
                type_assert_numeric(index->b, false);

                Type element_type = index->lhs->type;
                element_type.ref--;
                n->type = (Type) {
                    .kind = TYPE_SLICE,
                    .spec.slice.element = arena_clone(c->arena, &element_type, sizeof(element_type)),
                };
            } else if (
                type_kind_eq(index->lhs->type, TYPE_ARRAY) || type_kind_eq(index->lhs->type, TYPE_SLICE) ||
                type_kind_eq(index->lhs->type, TYPE_STRING)) {
                // The beginning can be inferred to be the beginning of the slice
                if (index->a) {
                    check_expr(c, index->a, REF_NONE, NULL);
                    type_assert_numeric(index->a, false);
                }

                // The ending can be inferred to be the ending of the slice
                if (index->b) {
                    check_expr(c, index->b, REF_NONE, NULL);
                    type_assert_numeric(index->b, false);
                }

                n->type = index->lhs->type;
                if (type_kind_eq(n->type, TYPE_ARRAY)) {
                    n->type.kind = TYPE_SLICE;
                }
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
            if (type_kind_eq(index->lhs->type, TYPE_ARRAY) && !index->lhs->type.ref) {
                check_expr(c, index->a, REF_NONE, NULL);
                type_assert_numeric(index->a, false);
                n->type = *index->lhs->type.spec.array.element;
            } else if (type_kind_eq(index->lhs->type, TYPE_SLICE) && !index->lhs->type.ref) {
                check_expr(c, index->a, REF_NONE, NULL);
                type_assert_numeric(index->a, false);
                n->type = *index->lhs->type.spec.slice.element;
            } else if (type_kind_eq(index->lhs->type, TYPE_STRING) && !index->lhs->type.ref) {
                check_expr(c, index->a, REF_NONE, NULL);
                type_assert_numeric(index->a, false);
                n->type = (Type) {.kind = TYPE_CHAR};
            } else {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot index into %s",
                    Pos_Arg(index->lhs->token.pos),
                    type_to_cstr(index->lhs->type));

                if (index->lhs->type.ref) {
                    fprintf(
                        stderr,
                        ". Pointers must be converted into slices before they can be indexed\n"
                        "\n"
                        "```\n"
                        "slice := pointer[begin..end];\n"
                        "slice[index];\n"
                        "```\n");

                    if (type_kind_eq(index->lhs->type, TYPE_SLICE) || type_kind_eq(index->lhs->type, TYPE_STRING)) {
                        fprintf(
                            stderr,
                            "\n"
                            "NOTE: Here the value is a %s. Perhaps it was meant to be dereferenced before indexing?\n",
                            type_to_cstr(index->lhs->type));
                    }
                } else {
                    fprintf(stderr, "\n");
                }

                exit(1);
            }
        }
    } break;

    case NODE_INDEXABLE: {
        Node_Indexable *indexable = (Node_Indexable *) n;

        size_t array_count = 0;
        if (indexable->count) {
            check_expr(c, indexable->count, REF_NONE, NULL);
            type_assert_numeric(indexable->count, false);

            const Const_Value value = eval_const_expr(c, indexable->count);
            assert(value.kind == CONST_VALUE_INT);
            array_count = value.as.integer;

            const u64 max_count = INT64_MAX; // TODO: Signed vs unsigned arithmetics for array index and range
            if (array_count > max_count) {
                if (type_is_signed(indexable->count->type)) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Number '%zd' is invalid for array capacity, which must be in range [0, %zu]\n",
                        Pos_Arg(indexable->count->token.pos),
                        array_count,
                        max_count);
                } else {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Number '%zu' is invalid for array capacity, which must be in range [0, %zu]\n",
                        Pos_Arg(indexable->count->token.pos),
                        array_count,
                        max_count);
                }
                exit(1);
            }

            check_expr(c, indexable->element, REF_NONE, NULL);
        } else {
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
            check_expr(c, indexable->element, REF_ADDR, NULL);
        }

        Type *element_type = arena_alloc(c->arena, sizeof(*element_type));
        *element_type = type_assert_type(indexable->element);
        element_type->is_meta = false;

        if (indexable->count) {
            n->type = (Type) {
                .kind = TYPE_ARRAY,
                .is_meta = true,
                .spec.array.element = element_type,
                .spec.array.count = array_count,
            };
        } else {
            n->type = (Type) {
                .kind = TYPE_SLICE,
                .is_meta = true,
                .spec.slice.element = element_type,
            };
        }

        is_ref_valid = ref == REF_ADDR;
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

static_assert(COUNT_NODES == 27, "");
static void check_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        check_expr(c, assertt->expr, REF_NONE, NULL);
        type_assert(c, assertt->expr, (Type) {.kind = TYPE_BOOL});

        if (assertt->message) {
            check_expr(c, assertt->message, REF_NONE, NULL);
            type_assert(c, assertt->message, (Type) {.kind = TYPE_STRING});
        }

        const bool ok = eval_const_expr(c, assertt->expr).as.integer;

        if (!ok) {
            fprintf(stderr, Pos_Fmt "Assertion failed", Pos_Arg(n->token.pos));
            if (assertt->message) {
                const SV message = eval_const_expr(c, assertt->message).as.string;
                fprintf(stderr, ": " SV_Fmt, SV_Arg(message));
            }
            fprintf(stderr, "\n");
            exit(1);
        }
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        if (define->expr && define->is_value_known_at_compile_time) {
            Node_Atom *lhs = NULL;
            Node      *rhs = NULL;
            while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
                rhs = node_iter(rhs, define->expr);
                assert(rhs);
                check_definition(c, lhs, rhs, define->type);
            }
        } else {
            Node_Atom *lhs = NULL;
            while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
                check_definition(c, lhs, define->expr, define->type);
            }
        }
    } break;

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;

        const size_t context_end_save = c->context.current->end;
        for (Node *it = block->body.head; it; it = it->next) {
            define_orderless_nodes(c, it, context_end_save);
        }

        for (Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }
        context_set_end(&c->context, context_end_save);
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            if (iff->compile_time_real_block) {
                if (iff->compile_time_real_block->kind == NODE_IF) {
                    check_stmt(c, iff->compile_time_real_block);
                } else if (iff->compile_time_real_block->kind == NODE_BLOCK) {
                    Node_Block *block = (Node_Block *) iff->compile_time_real_block;
                    for (Node *it = block->body.head; it; it = it->next) {
                        check_stmt(c, it);
                    }
                } else {
                    unreachable();
                }
            }
        } else {
            check_expr(c, iff->condition, REF_NONE, NULL);
            type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});
            check_stmt(c, iff->consequence);
            check_stmt(c, iff->antecedence);
        }
    } break;

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;

        const size_t context_end_save = c->context.current->end;
        {
            check_stmt(c, forr->init);
            if (forr->condition) {
                check_expr(c, forr->condition, REF_NONE, NULL);
                type_assert(c, forr->condition, (Type) {.kind = TYPE_BOOL});
            }
            check_stmt(c, forr->update);
            check_stmt(c, forr->body);
        }
        context_set_end(&c->context, context_end_save);
    } break;

    case NODE_CASE:
        unreachable();

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        check_switch_expr_and_alloc_preds(c, sw);
        if (sw->is_compile_time) {
            if (sw->compile_time_real_block) {
                assert(sw->compile_time_real_block->kind == NODE_BLOCK);
                Node_Block *block = (Node_Block *) sw->compile_time_real_block;
                for (Node *it = block->body.head; it; it = it->next) {
                    check_stmt(c, it);
                }
            }
        } else {
            size_t iota = 0;
            for (Node *it = sw->cases.head; it; it = it->next) {
                Node_Case *case_ = (Node_Case *) it;
                for (Node *pred = case_->preds.head; pred; pred = pred->next) {
                    check_switch_pred(c, sw, pred, &iota);
                }
                check_stmt(c, case_->body);
            }
            assert(iota == sw->preds_count);

            check_switch_exhaustive(sw);
        }
    } break;

    case NODE_JUMP:
        // Pass
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        check_stmt(c, defer->stmt);
    } break;

    case NODE_RETURN: {
        Node_Return  *returnn = (Node_Return *) n;
        const Type_Fn fn_type = c->context.current->fn->node.type.spec.fn;
        if (returnn->value) {
            check_expr(c, returnn->value, REF_NONE, fn_type.return_type);

            const bool   is_group = type_kind_eq(returnn->value->type, TYPE_GROUP);
            const size_t actual_count = is_group ? returnn->value->type.spec.group.count : 1;

            if (actual_count != fn_type.returns_count) {
                error_number_of_return_values_mismatch(n->token.pos, fn_type.returns_count, actual_count);
            }

            assert(actual_count == fn_type.returns_count);
            for (size_t i = 0; i < fn_type.returns_count; i++) {
                i64   group_index = -1;
                Node *n = get_node_from_group(returnn->value, i, &group_index);
                type_assert_grouped(c, n, fn_type.returns[i], group_index, NULL);
            }

            // The inference of the individual group items might not have reflected here
            returnn->value->type = *fn_type.return_type;
        } else {
            if (fn_type.returns_count) {
                error_number_of_return_values_mismatch(n->token.pos, fn_type.returns_count, 0);
            }
        }

        n->type = *fn_type.return_type;
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
    } break;

    case NODE_PRINT: {
        Node_Print *print = (Node_Print *) n;
        check_expr(c, print->value, REF_NONE, NULL);
        type_assert_scalar(print->value);
    } break;

    default:
        check_expr(c, n, REF_NONE, NULL);
        check_that_type_is_known(n);
        break;
    }
}

Const_Value get_platform(Compiler *c, Type *type) {
    Const_Value platform = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Platform"), NULL);
    assert(platform.kind == CONST_VALUE_TYPE);

    Type platform_type = platform.as.type;
    assert(platform_type.is_meta);
    platform_type.is_meta = false;
    assert(platform_type.kind == TYPE_ENUM);

    if (type) {
        *type = platform_type;
    }

    // TODO: This is unsafe

#ifdef PLATFORM_X86_64_LINUX
    return const_value_int(0);
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    return const_value_int(1);
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
    return const_value_int(2);
#endif // PLATFORM_X86_64_WINDOWS

    unreachable();
}

Const_Value get_const_definition_value(Compiler *c, Module *m, SV name, Type *type) {
    Node_Atom *atom = global_scope_find(&m->globals, name);
    assert(atom);
    assert(atom->definition_spec->is_const);
    check_stmt(c, (Node *) atom->definition_spec->definition_node);

    if (type) {
        *type = atom->node.type;
    }
    return atom->definition_spec->const_value;
}

void check_nodes(Compiler *c) {
    assert(c->parser);
    assert(c->modules);
    assert(c->main_module);
    assert(c->builtin_module);

    const Type unit = {.kind = TYPE_UNIT};
    c->main_fn_type = (Type) {
        .kind = TYPE_FN,
        .spec.fn.return_type = arena_clone(c->arena, &unit, sizeof(unit)),
    };

    for (Module *m = c->modules->head; m; m = m->next) {
        for (Node *it = m->nodes.head; it; it = it->next) {
            define_orderless_nodes(c, it, 0);
        }
    }

    for (Module *m = c->modules->head; m; m = m->next) {
        for (Node *it = m->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
    }

    get_main(c);
}
