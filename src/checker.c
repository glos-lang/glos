#include "checker.h"
#include "basic.h"
#include "compiler.h"
#include "context.h"
#include "contract.h"
#include "node.h"
#include "parser.h"
#include "token.h"
#include <assert.h>
#include <stdint.h>

static bool type_is_union(Type type) {
    return !type.ref && type_kind_eq(type, TYPE_UNION);
}

static bool node_is_null(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_NULL;
}

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

static_assert(COUNT_TYPES == 25, "");
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
static i64 get_enum_value(Node_Enum *enumm, SV name, const Token *t) {
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
        static_assert(COUNT_TOKENS == 77, "");
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
            member->enum_value = get_enum_value(expected.spec.enumm.definition, n->token.sv, &n->token);
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

static size_t get_union_type_index(Node *n, Type unionn) {
    assert(unionn.kind == TYPE_UNION);
    const Type_Union *spec = unionn.spec.unionn;

    Type type = n->type;
    type.is_meta = false;

    for (size_t i = 0; i < spec->variants_count; i++) {
        if (type_eq(spec->variants[i].type, type)) {
            return i + 1;
        }
    }

    fprintf(
        stderr,
        Pos_Fmt "ERROR: Type %s is not a variant of %s\n",
        Pos_Arg(n->token.pos),
        type_to_cstr(type),
        type_to_cstr(unionn));
    fprintf(stderr, Pos_Fmt "NOTE: Union defined here\n", Pos_Arg(spec->definition->node.token.pos));
    exit(1);
}

static bool try_auto_cast_type_to_rtti(Compiler *c, Node *n, Type expected) {
    if (n->type.is_meta && type_eq(expected, c->type_info_pointer_type)) {
        n->emit_type_info = arena_clone(c->arena, &n->type, sizeof(n->type));
        n->emit_type_info->is_meta = false;
        n->type = c->type_info_pointer_type;
        return true;
    }

    return false;
}

static bool type_eq_without_distinct(Type a, Type b) {
    a.distinct = NULL;
    b.distinct = NULL;
    return type_eq(a, b);
}

static void finalize_untyped_type(Type *t) {
    if (type_kind_eq(*t, TYPE_INT)) {
        t->kind = TYPE_I64;
    }
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

static bool try_auto_cast_literal(Node *n, Type expected) {
    // untyped 'null' -> typed 'null'
    if (node_is_null(n) && (expected.ref || type_kind_eq(expected, TYPE_RAWPTR) || type_kind_eq(expected, TYPE_FN))) {
        // NOTE: We are also checking for rawptr because distinct types exist
        n->type = expected;
        return true;
    }

    return false;
}

static bool try_auto_cast(Compiler *c, Node *n, Type expected) {
    if (try_auto_cast_untyped(c, n, expected)) {
        return true;
    }

    if (try_auto_cast_literal(n, expected)) {
        return true;
    }

    if (try_auto_cast_type_to_rtti(c, n, expected)) {
        return true;
    }

    if (type_is_union(expected)) {
        finalize_untyped_type(&n->type);
        n->auto_cast_from = arena_clone(c->arena, &n->type, sizeof(n->type));
        n->auto_cast_kind = AUTO_CAST_TO_UNION;
        n->auto_cast_data = get_union_type_index(n, expected);
        n->type = expected;
        return true;
    }

    if (type_kind_eq(n->type, TYPE_ARRAY) && type_kind_eq(expected, TYPE_SLICE) && !n->type.ref && !expected.ref) {
        n->auto_cast_from = arena_clone(c->arena, &n->type, sizeof(n->type));
        n->auto_cast_kind = AUTO_CAST_ARRAY_TO_SLICE;
        n->type = expected;
        return true;
    }

    if (type_eq(expected, (Type) {.kind = TYPE_ANY})) {
        finalize_untyped_type(&n->type);
        try_auto_cast_type_to_rtti(c, n, c->type_info_pointer_type);
        n->auto_cast_from = arena_clone(c->arena, &n->type, sizeof(n->type));
        n->auto_cast_kind = AUTO_CAST_TO_ANY;
        n->type = expected;
        return true;
    }

    return false;
}

static Type type_assert(Compiler *c, Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return expected;
    }

    if (try_auto_cast(c, n, expected)) {
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
        if (try_auto_cast(c, n, expected)) {
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

    if (try_auto_cast(c, b, a->type)) {
        return a->type;
    }

    if (try_auto_cast(c, a, b->type)) {
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
    check_that_type_is_known(n);
    if (n->type.is_meta) {
        return n->type;
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected a type, got %s\n", Pos_Arg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static bool get_builtin_type_kind(SV name, Type_Kind *kind) {
    static_assert(COUNT_TYPES == 25, "");
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
        [TYPE_ANY] = "any",
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
            return loop_breaks(iff->compile_time_real);
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
            if (!sw->compile_time_real) {
                return false;
            }
            return loop_breaks(sw->compile_time_real->body);
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
            return always_returns(iff->compile_time_real);
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
            if (!sw->compile_time_real) {
                return false;
            }
            return always_returns(sw->compile_time_real->body);
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

    REF_ADDR_MEMBER,
    REF_ASSIGN_MEMBER,
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

    const Type_Fn *signature = main->node.type.spec.fn;
    if (signature->args_count) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot take any arguments\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }

    if (signature->returns_count) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot return anything\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }

    return c->main_fn;
}

static_assert(COUNT_TYPES == 25, "");
static Const_Value default_const_value(Compiler *c, Type type) {
    if (type.ref) {
        return const_value_int(0); // TODO: Pointers in constant expressions
    }

    switch (type.kind) {
    case TYPE_BOOL:
    case TYPE_CHAR:

    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_INT:

    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:

    case TYPE_RAWPTR: // TODO: Pointers in constant expressions
    case TYPE_FN:
    case TYPE_ENUM:
        return const_value_int(0);

    case TYPE_UNION:
        return const_value_union((Const_Value_Union) {.spec = type.spec.unionn});

    case TYPE_STRUCT: {
        Const_Value_Struct structure = {0};
        structure.spec = type.spec.structt;
        structure.fields = arena_alloc(c->arena, structure.spec->fields_count * sizeof(*structure.fields));
        for (size_t i = 0; i < structure.spec->fields_count; i++) {
            structure.fields[i] = default_const_value(c, structure.spec->fields[i].type);
        }
        return const_value_struct(structure);
    }

    case TYPE_ARRAY: {
        Const_Value_Array array = {0};
        array.count = type.spec.array.count;
        array.data = arena_alloc(c->arena, array.count * sizeof(*array.data));
        array.element_type = type.spec.array.element;
        for (size_t i = 0; i < array.count; i++) {
            array.data[i] = default_const_value(c, *array.element_type);
        }
        return const_value_array(array);
    }

    case TYPE_SLICE: {
        todo(); // TODO: Slices in constant expressions
    }

    case TYPE_STRING:
        return const_value_string((SV) {0});

    case TYPE_ANY:
        return const_value_any((Const_Value_Any) {0});

    default:
        unreachable();
    }
}

static Const_Value const_value_to_union(Compiler *c, Type union_type, size_t union_index, Const_Value value) {
    Const_Value_Union unionn = {0};
    assert(union_type.kind == TYPE_UNION);
    unionn.spec = union_type.spec.unionn;
    unionn.index = union_index;
    unionn.real = arena_clone(c->arena, &value, sizeof(value));
    return const_value_union(unionn);
}

static bool eval_const_binary_equality(Compiler *c, Node_Binary *binary) {
    Const_Value lhs = eval_const_expr(c, binary->lhs);
    Const_Value rhs = eval_const_expr(c, binary->rhs);

    if (binary->any_check) {
        Const_Value any;
        Const_Value type;
        if (binary->any_check == binary->lhs) {
            any = lhs;
            type = rhs;
        } else {
            any = rhs;
            type = lhs;
        }
        assert(any.kind == CONST_VALUE_ANY);

        // TODO: Pointers in constant expressions
        if (type.kind == CONST_VALUE_INT && type.as.integer == 0) {
            return !any.as.any.type;
        }

        assert(type.kind == CONST_VALUE_TYPE);
        if (!any.as.any.type) {
            return false;
        }

        Type expected = type.as.type;
        expected.is_meta = false;
        return type_eq(*any.as.any.type, expected);
    }

    if (binary->union_check) {
        Const_Value unionn;
        Const_Value variant;
        if (binary->union_check == binary->lhs) {
            unionn = lhs;
            variant = rhs;
        } else {
            unionn = rhs;
            variant = lhs;
        }
        assert(unionn.kind == CONST_VALUE_UNION);

        // TODO: Pointers in constant expressions
        if (variant.kind == CONST_VALUE_INT && variant.as.integer == 0) {
            return unionn.as.unionn.index == 0;
        }

        assert(variant.kind == CONST_VALUE_TYPE);
        if (unionn.as.unionn.index == 0) {
            return false;
        }

        Type expected = variant.as.type;
        expected.is_meta = false;
        return type_eq(unionn.as.unionn.spec->variants[unionn.as.unionn.index - 1].type, expected);
    }

    return const_value_eq(lhs, rhs);
}

// Is this valid for signedness?
static_assert(COUNT_NODES == 27, "");
static Const_Value eval_const_expr_impl(Compiler *c, Node *n) {
    if (!n) {
        return (Const_Value) {0};
    }

    if (n->emit_type_info) {
        return const_value_type(*n->emit_type_info);
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;

        static_assert(COUNT_TOKENS == 77, "");
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
                fprintf(
                    stderr,
                    Pos_Fmt "NOTE: Here is the variable being used\n",
                    Pos_Arg(atom->definition->node.token.pos));
                exit(1);
            }

            return atom->definition->definition_spec->const_value;

        case TOKEN_STRING:
            return const_value_string(n->token.sv);

        case TOKEN_ISTRING:
            unreachable();

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

        static_assert(COUNT_TOKENS == 77, "");
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

        case TOKEN_TYPEOF: {
            Type type = unary->value->type;
            finalize_untyped_type(&type);
            type.is_meta = true;
            return const_value_type(type);
        }

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        if (binary->overload) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot call operator overload in compile time expressions\n",
                Pos_Arg(n->token.pos));
            fprintf(
                stderr,
                Pos_Fmt "NOTE: This is the overload used\n",
                Pos_Arg(binary->overload->defined_as->node.token.pos));
            exit(1);
        }

        Const_Value lhs = {0};
        Const_Value rhs = {0};

        static_assert(COUNT_TOKENS == 77, "");
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
            return const_value_int(eval_const_binary_equality(c, binary));

        case TOKEN_NE:
            return const_value_int(!eval_const_binary_equality(c, binary));

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

        if (member->method) {
            return const_value_fn(member->method);
        }

        const Const_Value lhs = eval_const_expr(c, member->lhs);

        static_assert(COUNT_CONST_VALUES == 9, "");
        switch (lhs.kind) {
        case CONST_VALUE_UNION:
            if (member->rhs) {
                if (member->union_index != lhs.as.unionn.index) {
                    const Type_Union *spec = lhs.as.unionn.spec;
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Type mismatch: Accessing %s, but real type is %s\n",
                        Pos_Arg(n->token.pos),
                        member->union_index ? type_to_cstr(spec->variants[member->union_index - 1].type) : "null",
                        lhs.as.unionn.index ? type_to_cstr(spec->variants[lhs.as.unionn.index - 1].type) : "null");
                    exit(1);
                }

                assert(lhs.as.unionn.real); // The type is checked, that means a real value exists
                return *lhs.as.unionn.real;
            } else if (member->field_index == 0) {
                return const_value_int(lhs.as.unionn.index);
            } else {
                unreachable();
            }

        case CONST_VALUE_STRUCT:
            return lhs.as.structt.fields[member->field_index];

        case CONST_VALUE_ARRAY:
            if (member->field_index == 0) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot access pointers in constant expressions\n", Pos_Arg(n->token.pos));
                exit(1);
            } else if (member->field_index == 1) {
                return const_value_int(lhs.as.array.count);
            } else {
                unreachable();
            }

        case CONST_VALUE_STRING:
            if (member->field_index == 0) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot access pointers in constant expressions\n", Pos_Arg(n->token.pos));
                exit(1);
            } else if (member->field_index == 1) {
                return const_value_int(lhs.as.string.count);
            } else {
                unreachable();
            }

        case CONST_VALUE_ANY:
            if (member->rhs) {
                if (!lhs.as.any.type || !type_eq(n->type, *lhs.as.any.type)) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Type mismatch: Accessing %s, but real type is %s\n",
                        Pos_Arg(n->token.pos),
                        type_to_cstr(n->type),
                        lhs.as.any.type ? type_to_cstr(*lhs.as.any.type) : "null");
                    exit(1);
                }

                assert(lhs.as.any.value); // The type is checked, that means a real value exists
                return *lhs.as.any.value;
            } else if (member->field_index == 0) {
                if (lhs.as.any.type) {
                    return const_value_type(*lhs.as.any.type);
                }
                return const_value_int(0);
            } else if (member->field_index == 1) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot access pointers in constant expressions\n", Pos_Arg(n->token.pos));
                exit(1);
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

    case NODE_INTERPOLATION:
        unreachable();

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;
        if (fn->is_type) {
            return const_value_type(fn->node.type);
        } else {
            return const_value_fn(fn);
        }
    }

    case NODE_ENUM:
    case NODE_UNION:
    case NODE_STRUCT:
        return const_value_type(n->type);

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        Const_Value    value = default_const_value(c, n->type);

        size_t ordered_iota = 0;
        ll_foreach(iter, &compound->children) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            Node *it = iter;
            if (compound->is_designated) {
                assert(it->kind == NODE_BINARY && it->token.kind == TOKEN_SET);
                Node_Binary *it_binary = (Node_Binary *) it;
                it_iota = it->token.as.integer;
                it = it_binary->rhs;
            }

            if (n->type.kind == TYPE_STRUCT) {
                value.as.structt.fields[it_iota] = eval_const_expr(c, it);
            } else if (n->type.kind == TYPE_ARRAY) {
                value.as.array.data[it_iota] = eval_const_expr(c, it);
            } else {
                unreachable();
            }
        }

        return value;
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
        static_assert(COUNT_TYPE_CASTS == 4, "");
        switch (call->type_cast) {
        case TYPE_CAST_NOP:
            return value;

        case TYPE_CAST_NORMAL:
            return value;

        case TYPE_CAST_TO_BOOL:
            return const_value_int(value.as.integer != 0);

        case TYPE_CAST_TO_UNION:
            return const_value_to_union(c, n->type, call->type_cast_union_index, value);

        default:
            unreachable();
        }
    } break;

    case NODE_INDEX: {
        Node_Index       *index = (Node_Index *) n;
        const Const_Value lhs = eval_const_expr(c, index->lhs);
        if (index->is_ranged) {
            static_assert(COUNT_CONST_VALUES == 9, "");
            switch (lhs.kind) {
            case CONST_VALUE_ARRAY: {
                Const_Value_Array array = lhs.as.array;

                i64 begin = 0;
                if (index->a) {
                    begin = eval_const_expr(c, index->a).as.integer;
                }

                i64 end = array.count;
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

                if (begin < 0 || end < 0 || (size_t) begin > array.count || (size_t) end > array.count) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Range (%zd..%zd) is out of bounds in array of length %zu\n",
                        Pos_Arg(n->token.pos),
                        begin,
                        end,
                        array.count);
                    exit(1);
                }

                array.data += begin;
                array.count = end - begin;
                array.is_slice = true;
                return const_value_array(array);
            }

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

            static_assert(COUNT_CONST_VALUES == 9, "");
            switch (lhs.kind) {
            case CONST_VALUE_ARRAY: {
                if (at < 0 || (size_t) at >= lhs.as.array.count) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Index %zd is out of bounds in array of length %zu\n",
                        Pos_Arg(n->token.pos),
                        at,
                        lhs.as.array.count);
                    exit(1);
                };

                return lhs.as.array.data[at];
            }

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

static Const_Value eval_const_expr(Compiler *c, Node *n) {
    if (!n) {
        return (Const_Value) {0};
    }

    Type n_type_save;
    if (n->auto_cast_from) {
        n_type_save = n->type;
        n->type = *n->auto_cast_from;
    }

    Const_Value result = eval_const_expr_impl(c, n);
    if (n->auto_cast_from) {
        n->type = n_type_save;

        static_assert(COUNT_AUTO_CASTS == 3, "");
        switch (n->auto_cast_kind) {
        case AUTO_CAST_TO_ANY: {
            Const_Value_Any any = {0};
            any.type = n->auto_cast_from;
            any.value = arena_clone(c->arena, &result, sizeof(result));
            result = const_value_any(any);
        } break;

        case AUTO_CAST_TO_UNION:
            result = const_value_to_union(c, n->type, n->auto_cast_data, result);
            break;

        case AUTO_CAST_ARRAY_TO_SLICE:
            assert(result.kind == CONST_VALUE_ARRAY);
            result.as.array.is_slice = true;
            break;

        default:
            unreachable();
        }
    }

    return result;
}

static void check_switch_expr_and_alloc_preds(Compiler *c, Node_Switch *sw) {
    check_expr(c, sw->expr, REF_NONE, NULL);
    finalize_untyped_type(&sw->expr->type);
    check_that_type_is_known(sw->expr);

    if (!sw->expr->type.ref && type_kind_eq(sw->expr->type, TYPE_ENUM)) {
        sw->enumeration = sw->expr->type.spec.enumm.definition;
    } else if (type_is_union(sw->expr->type)) {
        sw->unionn = sw->expr->type.spec.unionn->definition;
    } else if (sw->expr->type.is_meta) {
        sw->expr->emit_type_info = arena_clone(c->arena, &sw->expr->type, sizeof(sw->expr->type));
        sw->expr->emit_type_info->is_meta = false;
        sw->expr->type = c->type_info_pointer_type;
        sw->is_expr_type_info = true;
    } else if (type_eq(sw->expr->type, c->type_info_pointer_type)) {
        sw->is_expr_type_info = true;
    } else if (type_eq(sw->expr->type, (Type) {.kind = TYPE_ANY})) {
        sw->is_expr_any = true;
    } else if (!type_is_numeric(sw->expr->type) && !type_kind_eq(sw->expr->type, TYPE_CHAR)) {
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
    Const_Value value = {0};
    if (sw->unionn) {
        check_expr(c, pred, REF_NONE, NULL);
        if (node_is_null(pred)) {
            value = const_value_int(0); // TODO: Pointers in constant expressions
        } else {
            type_assert_type(pred);
            value = const_value_int(get_union_type_index(pred, sw->expr->type));
        }
    } else if (sw->is_expr_any) {
        check_expr(c, pred, REF_NONE, NULL);
        if (node_is_null(pred)) {
            value = const_value_int(0); // TODO: Pointers in constant expressions
        } else {
            type_assert_type(pred);
            Type type = pred->type;
            type.is_meta = false;
            value = const_value_type(type);
        }
    } else {
        check_expr(c, pred, REF_NONE, &sw->expr->type);
        type_assert(c, pred, sw->expr->type);
        value = eval_const_expr(c, pred);
    }

    for (size_t i = 0; i < *iota; i++) {
        if (const_value_eq(sw->preds[i].value, value)) {
            fprintf(stderr, Pos_Fmt "ERROR: Duplicate case ", Pos_Arg(pred->token.pos));

            if (sw->unionn) {
                pred->type.is_meta = false;
                fprintf(stderr, "%s", type_to_cstr(pred->type));
                pred->type.is_meta = true;
            } else {
                static_assert(COUNT_CONST_VALUES == 9, "");
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

                default:
                    unreachable();
                }
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
    } else if (sw->unionn) {
        if (sw->preds_count < sw->unionn->variants_count + 1 && !sw->fallback) {
            fprintf(stderr, Pos_Fmt "ERROR: This switch statement is not complete\n", Pos_Arg(sw->node.token.pos));

            fprintf(stderr, "\n");
            fprintf(stderr, "The following union variants are not handled:\n");

            const size_t variants_count = sw->unionn->variants_count + 1;

            bool *handled = temp_alloc(variants_count * sizeof(*handled));
            for (size_t i = 0; i < sw->preds_count; i++) {
                const Const_Value *pred_value = &sw->preds[i].value;
                assert(pred_value->kind == CONST_VALUE_INT);

                const size_t pred_index = pred_value->as.integer;
                assert(pred_index < variants_count);
                handled[pred_index] = true;
            }

            for (size_t i = 0; i < variants_count; i++) {
                if (!handled[i]) {
                    fprintf(
                        stderr,
                        "    - %s\n",
                        i ? type_to_cstr_raw(sw->unionn->node.type.spec.unionn->variants[i - 1].type) : "null");
                }
            }
            fprintf(stderr, "\n");

            fprintf(stderr, Pos_Fmt "NOTE: Union defined here\n", Pos_Arg(sw->unionn->node.token.pos));
            exit(1);
        }
    }
}

static void push_context_replace(Compiler *c, Context_Replace *replace, Node_Atom *from, Type to) {
    replace->from = from;

    // The only definitions which have definitions are ghosts
    while (replace->from->definition) {
        replace->from = replace->from->definition;
    }

    replace->to = arena_clone(c->arena, from, sizeof(*from));
    replace->to->is_ghost = true;
    replace->to->definition = from;
    replace->to->definition_spec =
        arena_clone(c->arena, replace->to->definition_spec, sizeof(*replace->to->definition_spec));

    replace->to->node.type = to;
    replace->to->node.type.is_meta = false;

    if (replace->to->definition_spec->is_const) {
        Const_Value *value = &replace->to->definition_spec->const_value;

        static_assert(COUNT_CONST_VALUES == 9, "");
        switch (value->kind) {
        case CONST_VALUE_UNION: {
            const Const_Value_Union unionn = value->as.unionn;
            if (unionn.index) {
                if (type_eq(unionn.spec->variants[unionn.index - 1].type, replace->to->node.type)) {
                    assert(unionn.real);
                    *value = *unionn.real;
                }

                // Technically the code generated will access invalid memory.
                // However it will be unreachable, so does it even matter?
            }
        } break;

        case CONST_VALUE_ANY: {
            const Const_Value_Any any = value->as.any;
            if (any.type) {
                if (type_eq(*any.type, replace->to->node.type)) {
                    assert(any.value);
                    *value = *any.value;
                }
            }
        } break;

        default:
            unreachable();
        }
    }

    c->context.replace = replace;
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
                        const Context_Fn *fn = c->context.fn;

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

                        context_push_local(&c->context, it);
                    }

                    it->definition_spec->fn_context = c->context.fn;
                } else {
                    if (get_builtin_type_kind(it->node.token.sv, NULL)) {
                        error_redefinition((Node *) it, NULL);
                    }

                    bool is_method = false;
                    if (it->definition_spec->assignment_node && it->definition_spec->assignment_node->kind == NODE_FN) {
                        Node_Fn *fn = (Node_Fn *) it->definition_spec->assignment_node;
                        is_method = fn->is_method;

                        if (is_method) {
                            da_push(&c->methods_list, fn);
                        }
                    }

                    if (!is_method) {
                        Node_Atom *previous = global_scope_find(&it->module->globals, it->node.token.sv);
                        if (previous) {
                            error_redefinition((Node *) it, &previous->node.token.pos);
                        }

                        global_scope_push(&it->module->globals, it);
                    }
                }

                it->definition_spec->replace_context = c->context.replace;
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
            iff->compile_time_real = value.as.integer ? iff->consequence : iff->antecedence;

            if (iff->compile_time_real) {
                iff->context_replace.outer = c->context.replace;

                if (iff->compile_time_real == iff->consequence) {
                    if (iff->condition->kind == NODE_BINARY && iff->condition->token.kind == TOKEN_EQ) {
                        Node_Binary *condition = (Node_Binary *) iff->condition;
                        const Type   any_type = {.kind = TYPE_ANY};
                        if ((type_eq(condition->lhs->type, any_type) || type_is_union(condition->lhs->type)) &&
                            condition->lhs->kind == NODE_ATOM) {
                            if (!node_is_null(condition->rhs)) {
                                push_context_replace(
                                    c,
                                    &iff->context_replace,
                                    ((Node_Atom *) condition->lhs)->definition,
                                    condition->rhs->type);
                            }
                        } else if (
                            (type_eq(condition->rhs->type, any_type) || type_is_union(condition->rhs->type)) &&
                            condition->rhs->kind == NODE_ATOM) {
                            if (!node_is_null(condition->lhs)) {
                                push_context_replace(
                                    c,
                                    &iff->context_replace,
                                    ((Node_Atom *) condition->rhs)->definition,
                                    condition->lhs->type);
                            }
                        }
                    }
                }

                if (iff->compile_time_real->kind == NODE_BLOCK) {
                    Node_Block *block = (Node_Block *) iff->compile_time_real;
                    for (Node *it = block->body.head; it; it = it->next) {
                        define_orderless_nodes(c, it, block_start);
                    }
                } else {
                    define_orderless_nodes(c, iff->compile_time_real, block_start);
                }

                c->context.replace = iff->context_replace.outer;
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
                Node_Case *branch = (Node_Case *) it;
                for (Node *pred = branch->preds.head; pred; pred = pred->next) {
                    const Const_Value pred_value = check_switch_pred(c, sw, pred, &iota);
                    if (sw->is_expr_any) {
                        assert(value.kind == CONST_VALUE_ANY);

                        const Type *pred_type = NULL;
                        if (pred_value.kind == CONST_VALUE_TYPE) {
                            pred_type = &pred_value.as.type;
                        }

                        if (value.as.any.type) {
                            if (pred_type && type_eq(*value.as.any.type, *pred_type)) {
                                sw->compile_time_real = branch;
                            }
                        } else {
                            if (!pred_type) {
                                sw->compile_time_real = branch;
                            }
                        }
                    } else if (sw->unionn) {
                        assert(value.kind == CONST_VALUE_UNION);
                        assert(pred_value.kind == CONST_VALUE_INT);
                        if (value.as.unionn.index == (size_t) pred_value.as.integer) {
                            sw->compile_time_real = branch;
                        }
                    } else if (const_value_eq(pred_value, value)) {
                        sw->compile_time_real = branch;
                    }
                }
            }
            assert(iota == sw->preds_count);

            if (!sw->compile_time_real && sw->fallback) {
                assert(sw->fallback->kind == NODE_CASE);
                sw->compile_time_real = (Node_Case *) sw->fallback;
            }

            Node_Case *branch = sw->compile_time_real;
            if (branch) {
                branch->context_replace.outer = c->context.replace;

                if ((sw->unionn || sw->is_expr_any) && sw->expr->kind == NODE_ATOM && branch->preds_count == 1) {
                    if (!node_is_null(branch->preds.head)) {
                        push_context_replace(
                            c,
                            &branch->context_replace,
                            ((Node_Atom *) sw->expr)->definition,
                            branch->preds.head->type);
                    }
                }

                assert(branch->body->kind == NODE_BLOCK);
                Node_Block *block = (Node_Block *) branch->body;
                for (Node *it = block->body.head; it; it = it->next) {
                    define_orderless_nodes(c, it, block_start);
                }

                c->context.replace = branch->context_replace.outer;
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
                it_expr->type = c->source_code_location_type;
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

                if (it_expr->type.is_meta && !it->definition_spec->is_const) {
                    it_expr->emit_type_info = arena_clone(c->arena, &it_expr->type, sizeof(it_expr->type));
                    it_expr->emit_type_info->is_meta = false;
                    it_expr->type = c->type_info_pointer_type;
                }

                const bool is_it_a_module = type_kind_eq(it_expr->type, TYPE_MODULE) && !it->definition_spec->is_const;
                if (is_it_a_module) {
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
                    type_assert_grouped(c, n, type->type, group_index, &it->node.token.pos);
                } else {
                    it->node.type = it_expr->type.spec.group.data[it->definition_spec->group_index];
                    finalize_untyped_type(&it->node.type);
                }

                type_determined = true;
            }
        }

        if (!type_determined) {
            if (type) {
                type_assert(c, it_expr, type->type);
            } else {
                if (!it->definition_spec->is_const) {
                    finalize_untyped_type(&it_expr->type);
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

static void check_definition_if_needed(Compiler *c, Node_Atom *definition, Ref_Kind ref) {
    switch (definition->definition_spec->check_status) {
    case UNCHECKED: {
        Context_Fn *context_fn_save = c->context.fn;
        c->context.fn = definition->definition_spec->fn_context;

        Context_Replace *context_replace_save = c->context.replace;
        c->context.replace = definition->definition_spec->replace_context;

        // Only orderless definitions can be uninffered, and the assignment of such definitions must be constant
        assert(definition->definition_spec->definition_node->is_value_known_at_compile_time);

        check_definition(
            c,
            definition,
            definition->definition_spec->assignment_node,
            definition->definition_spec->definition_node->type);

        context_restore_fn(&c->context, context_fn_save);
        c->context.replace = context_replace_save;
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
}

static void check_ident(Compiler *c, Node *n, Ref_Kind ref) {
    Node_Atom   *atom = NULL;
    Node_Member *member = NULL;

    Module *module = NULL;
    bool    importing = false;
    if (n->kind == NODE_ATOM) {
        atom = (Node_Atom *) n;
        module = atom->module;
    } else if (n->kind == NODE_MEMBER) {
        member = (Node_Member *) n;
        assert(member->lhs->type.kind == TYPE_MODULE);
        module = member->lhs->type.spec.module;
        importing = true;
    } else {
        unreachable();
    }

    if (sv_match(n->token.sv, "_")) {
        fprintf(stderr, Pos_Fmt "ERROR: Identifier '_' cannot be used as a value\n", Pos_Arg(n->token.pos));
        exit(1);
    }

    Node_Atom *definition = NULL;
    if (atom) {
        definition = context_find_local(&c->context, n->token.sv);
        if (definition && definition->definition_spec->fn_context && c->context.fn) {
            if (definition->definition_spec->fn_context != c->context.fn && !definition->definition_spec->is_const) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot use variable from stack frame of outer function\n",
                    Pos_Arg(n->token.pos));
                fprintf(stderr, Pos_Fmt "NOTE: Here is the variable being used\n", Pos_Arg(definition->node.token.pos));
                exit(1);
            }
        }
    }

    if (!definition) {
        definition = global_scope_find(&module->globals, n->token.sv);
        if (!definition && atom) {
            module = c->builtin_module;
            importing = true;
            definition = global_scope_find(&module->globals, n->token.sv);
        }

        if (definition && definition->definition_spec->is_private && importing) {
            definition = NULL;
        }
    }

    if (definition) {
        for (Context_Replace *it = c->context.replace; it; it = it->outer) {
            if (it->from == definition) {
                definition = it->to;
                break;
            }
        }
    }

    if (atom) {
        atom->definition = definition;
    } else if (member) {
        member->module_access_definition = definition;
    }

    if (definition) {
        check_definition_if_needed(c, definition, ref);

        n->type = definition->node.type;
        n->is_memory = !definition->definition_spec->is_const;
        if (!n->is_memory) {
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

            case REF_ADDR_MEMBER:
                if (!n->type.is_meta && !type_kind_eq(definition->node.type, TYPE_MODULE)) {
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

            case REF_ASSIGN_MEMBER:
                if (!type_kind_eq(definition->node.type, TYPE_MODULE)) {
                    fprintf(
                        stderr, Pos_Fmt "ERROR: Cannot assign to compile time constant value\n", Pos_Arg(n->token.pos));
                    exit(1);
                }
                break;
            }
        }
    } else {
        if (atom) {
            Type_Kind kind;
            if (get_builtin_type_kind(n->token.sv, &kind)) {
                n->type = (Type) {.kind = kind, .is_meta = true};
                return;
            }
        }

        error_undefined(&n->token, "identifier", false);
    }
}

static bool get_method_spec(Compiler *c, Type receiver, SV name, Method_Spec *spec, Module *defining_in_module) {
    if (spec) {
        spec->name = name;
    }

    if (type_kind_eq(receiver, TYPE_ENUM)) {
        if (spec) {
            spec->uid = (uintptr_t) receiver.spec.enumm.definition;
        }

        if (defining_in_module) {
            return defining_in_module == receiver.spec.enumm.definition->module;
        }
        return true;
    } else if (type_kind_eq(receiver, TYPE_UNION)) {
        if (spec) {
            spec->uid = (uintptr_t) receiver.spec.unionn->definition;
        }

        if (defining_in_module) {
            return defining_in_module == receiver.spec.unionn->definition->module;
        }
        return true;
    } else if (type_kind_eq(receiver, TYPE_STRUCT)) {
        if (spec) {
            spec->uid = (uintptr_t) receiver.spec.structt->definition;
        }

        if (defining_in_module) {
            return defining_in_module == receiver.spec.structt->definition->module;
        }
        return true;
    } else if (receiver.distinct) {
        if (spec) {
            spec->uid = (uintptr_t) receiver.distinct;
        }

        if (defining_in_module) {
            return defining_in_module == receiver.distinct->module;
        }
        return true;
    }

    static const Type string_type = {.kind = TYPE_STRING};
    if (type_eq(receiver, string_type)) {
        if (spec) {
            spec->uid = (uintptr_t) &string_type;
        }

        if (defining_in_module) {
            return defining_in_module == c->builtin_module;
        }
        return true;
    }

    return false;
}

static Node_Fn *get_operator_overload(Compiler *c, SV operator, Node *receiver, Pos *pos) {
    Method_Spec spec = {0};
    if (get_method_spec(c, receiver->type, operator, &spec, NULL)) {
        Node_Fn **fn = ht_get(&c->methods_table, spec);
        if (fn) {
            return *fn;
        }
    }

    check_that_type_is_known(receiver);
    fprintf(
        stderr,
        Pos_Fmt "ERROR: Operator '" SV_Fmt "' is not defined for %s\n",
        Pos_Arg(*pos),
        SV_Arg(spec.name),
        type_to_cstr(receiver->type));
    exit(1);
}

static SV operator_name_from_token_kind(Token_Kind kind) {
    static const char *names[COUNT_TOKENS] = {
        [TOKEN_ADD] = "+",
        [TOKEN_SUB] = "-",
        [TOKEN_MUL] = "*",
        [TOKEN_DIV] = "/",
        [TOKEN_MOD] = "%",

        [TOKEN_GT] = ">",
        [TOKEN_GE] = ">=",
        [TOKEN_LT] = "<",
        [TOKEN_LE] = "<=",
        [TOKEN_EQ] = "==",
        [TOKEN_NE] = "!=",

        [TOKEN_ADD_SET] = "+",
        [TOKEN_SUB_SET] = "-",
        [TOKEN_MUL_SET] = "*",
        [TOKEN_DIV_SET] = "/",
        [TOKEN_MOD_SET] = "%",
    };

    assert(kind > TOKEN_EOF && kind < COUNT_TOKENS);
    return sv_from_cstr(names[kind]);
}

static Node_Fn *check_assignment_lhs_for_arithmetics(Compiler *c, Node *n, Token_Kind op) {
    switch (op) {
    case TOKEN_ADD_SET:
    case TOKEN_SUB_SET:
        if (!type_is_numeric(n->type) && !type_is_pointer(n->type)) {
            return get_operator_overload(c, operator_name_from_token_kind(op), n, &n->token.pos);
        }
        break;

    case TOKEN_MUL_SET:
    case TOKEN_DIV_SET:
    case TOKEN_MOD_SET:
        if (!type_is_numeric(n->type)) {
            return get_operator_overload(c, operator_name_from_token_kind(op), n, &n->token.pos);
        }
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

    return NULL;
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
        if (binary->node.token.kind != TOKEN_SET) {
            binary->overloads = arena_alloc(c->arena, lhs_count * sizeof(*binary->overloads));
        }

        assert(is_rhs_group);
        for (size_t i = 0; i < lhs_count; i++) {
            i64   lhs_group_index = -1;
            Node *lhs = get_node_from_group(binary->lhs, i, &lhs_group_index);
            i64   rhs_group_index = -1;
            Node *rhs = get_node_from_group(binary->rhs, i, &rhs_group_index);
            type_assert_grouped(c, rhs, lhs->type, rhs_group_index, &lhs->token.pos);

            if (binary->overloads) {
                binary->overloads[i] = check_assignment_lhs_for_arithmetics(c, lhs, binary->node.token.kind);
            }
        }
    } else {
        type_assert(c, binary->rhs, binary->lhs->type);
        binary->overload = check_assignment_lhs_for_arithmetics(c, binary->lhs, binary->node.token.kind);
    }

    binary->node.type = (Type) {.kind = TYPE_UNIT};
}

static const Type *get_argument_type(const Type_Fn *spec, size_t index) {
    const Type *type = NULL;
    if (index < spec->args_count) {
        type = &spec->args[index].type;
    }

    if (spec->variadics_kind == VARIADICS_TYPED && index >= spec->variadics_index) {
        type = &spec->args[spec->variadics_index].type;
        assert(type->kind == TYPE_SLICE);
        type = type->spec.slice.element;
    }
    return type;
}

static const char *
fn_type_to_cstr_but_excluding_receiver_if_required(const Type_Fn *fn_spec_raw, bool exclude_receiver) {
    Type_Fn spec = *fn_spec_raw;
    if (exclude_receiver) {
        assert(spec.args_count);
        spec.args++;
        spec.args_count--;
        if (spec.args_count_min) spec.args_count_min--;
        if (spec.variadics_index) spec.variadics_index--;
    }

    return type_to_cstr((Type) {.kind = TYPE_FN, .spec.fn = &spec});
}

// If this is a cast, then do not pass 'fn_type_spec'
static void check_call_arguments(Compiler *c, Node_Call *call, const Type_Fn *fn_spec) {
    // TODO: When printing error messages showing the function type also, prettify it for methods
    typedef struct {
        const Node *node;
        const Node *name;
    } Argument;

    Argument *args = NULL;
    size_t    args_count_min = 1;
    size_t    args_count_max = 1;

    bool is_method = false;
    if (fn_spec) {
        args = temp_alloc(fn_spec->args_count * sizeof(*args));
        args_count_min = fn_spec->args_count_min;
        args_count_max = fn_spec->variadics_kind != VARIADICS_NONE ? UINT64_MAX : fn_spec->args_count;

        if (call->fn->kind == NODE_MEMBER) {
            Node_Member *member = (Node_Member *) call->fn;
            if (member->method) {
                assert(member->lhs);

                is_method = true;

                // The reference level has already been checked.
                // Technically the type has also been checked, and right now this is redundant. But later when compile
                // time polymorphism will be implemented, this will be important.
                Type expected = fn_spec->args[call->args_count].type;
                expected.ref = member->lhs->type.ref;
                type_assert(c, member->lhs, expected);

                args[call->args_count++].node = member->lhs;
            }
        }

        if (call->spread) {
            if (fn_spec->variadics_kind != VARIADICS_TYPED) {
                if (call->spread->kind != NODE_INTERPOLATION) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot use %s in a call to a function that does not have typed variadics\n",
                        Pos_Arg(call->spread_pos),
                        token_kind_to_cstr(TOKEN_SPREAD));
                    exit(1);
                }
            }
        }
    }

    ll_foreach(arg, &call->args) {
        Node      *it = arg;
        size_t     it_index = call->args_count;
        const bool it_is_named = it->kind == NODE_BINARY && it->token.kind == TOKEN_SET;
        Node      *it_name = NULL;

        const Type *expected = NULL;
        if (it_is_named) {
            if (!fn_spec) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot use named arguments in a cast expression\n", Pos_Arg(it->token.pos));
                exit(1);
            }

            it_name = ((Node_Binary *) it)->lhs;
            assert(it_name->kind == NODE_ATOM && it_name->token.kind == TOKEN_IDENT);

            for (size_t i = 0; i < fn_spec->args_count; i++) {
                const Type_Fn_Arg *arg = &fn_spec->args[i];
                if (sv_eq(arg->name, it_name->token.sv)) {
                    it_index = i;
                    expected = get_argument_type(fn_spec, i);
                    break;
                }
            }

            if (!expected) {
                error_undefined(&it_name->token, "argument", true);
                fprintf(
                    stderr,
                    Pos_Fmt "NOTE: The %s being called is %s\n",
                    Pos_Arg(call->fn->token.pos),
                    is_method ? "method" : "function",
                    fn_type_to_cstr_but_excluding_receiver_if_required(fn_spec, is_method));
                exit(1);
            }

            if (args[it_index].node) {
                const Node *previous_named = args[it_index].name;
                if (previous_named) {
                    error_redefinition(it_name, &previous_named->token.pos);
                }

                if (fn_spec->variadics_kind != VARIADICS_TYPED || it_index != fn_spec->variadics_index) {
                    error_redefinition(it_name, &args[it_index].node->token.pos);
                }
            } else {
                args[it_index].name = it_name;
            }

            it->token.as.integer = it_index;
            it = ((Node_Binary *) it)->rhs;
        } else if (fn_spec) {
            expected = get_argument_type(fn_spec, call->args_count);
        }

        if (it->kind == NODE_INTERPOLATION && fn_spec->variadics_kind == VARIADICS_TYPED) {
            bool ok = false;
            if (it_index == fn_spec->variadics_index) {
                const Type *type = &fn_spec->args[fn_spec->variadics_index].type;
                assert(type->kind == TYPE_SLICE);

                type = type->spec.slice.element;
                if (type->kind == TYPE_ANY) {
                    ok = true;
                    ((Node_Interpolation *) it)->is_valid = true;
                }
            }

            if (!ok) {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Interpolated string is in the wrong position\n", Pos_Arg(it->token.pos));
                fprintf(
                    stderr,
                    Pos_Fmt "NOTE: The %s being called is %s\n",
                    Pos_Arg(call->fn->token.pos),
                    is_method ? "method" : "function",
                    fn_type_to_cstr_but_excluding_receiver_if_required(fn_spec, is_method));
                exit(1);
            }
        }

        check_expr(c, it, REF_NONE, expected);
        check_that_type_is_known(it);

        const size_t parts = type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
        if (args) {
            bool type_checked = false;
            if (it_is_named) {
                assert(expected);
                if (fn_spec->variadics_kind == VARIADICS_TYPED && it_index == fn_spec->variadics_index) {
                    expected = &fn_spec->args[fn_spec->variadics_index].type;
                    call->do_not_allocate_typed_variadic_array = true;
                }
                type_assert(c, it, *expected);
                type_checked = true;
            } else if (arg == call->spread) {
                assert(fn_spec->variadics_kind == VARIADICS_TYPED);
                if (it_index != fn_spec->variadics_index) {
                    fprintf(stderr, Pos_Fmt "ERROR: Spread is in the wrong position\n", Pos_Arg(call->spread_pos));
                    fprintf(
                        stderr,
                        Pos_Fmt "NOTE: The %s being called is %s\n",
                        Pos_Arg(call->fn->token.pos),
                        is_method ? "method" : "function",
                        fn_type_to_cstr_but_excluding_receiver_if_required(fn_spec, is_method));
                    exit(1);
                }

                expected = &fn_spec->args[fn_spec->variadics_index].type;
                type_assert(c, it, *expected);
                type_checked = true;
            } else if (fn_spec->variadics_kind == VARIADICS_TYPED && it_index >= fn_spec->variadics_index) {
                call->typed_variadics_array_count += parts;
            }

            for (size_t i = 0; i < parts; i++) {
                if (!type_checked && expected) {
                    type_assert_grouped(c, it, *expected, i, NULL);
                }

                const size_t n = it_index + i;
                if (fn_spec->variadics_kind == VARIADICS_TYPED && n >= fn_spec->variadics_index) {
                    Argument *variadic_arg = &args[fn_spec->variadics_index];
                    if (it_is_named) {
                        if (n == fn_spec->variadics_index) {
                            // Provide the variadic argument as a named argument
                            if (variadic_arg->node) {
                                // Variadic arguments was already started as a stack allocated array
                                fprintf(
                                    stderr,
                                    Pos_Fmt "ERROR: Multiple typed variadic sources found\n",
                                    Pos_Arg(call->node.token.pos));

                                if (variadic_arg->node == call->spread) {
                                    fprintf(
                                        stderr,
                                        Pos_Fmt "... This %s provide one source\n",
                                        Pos_Arg(call->spread_pos),
                                        call->spread->kind == NODE_INTERPOLATION ? "interpolated string" : "spread");
                                } else {
                                    bool following = false;
                                    if (variadic_arg->node->next) {
                                        following = true;
                                        Node *next = variadic_arg->node->next;
                                        if (next->kind == NODE_BINARY && next->token.kind == TOKEN_SET) {
                                            following = false;
                                        }
                                    }

                                    fprintf(
                                        stderr,
                                        Pos_Fmt "... This argument%s provides one source\n",
                                        Pos_Arg(variadic_arg->node->token.pos),
                                        following ? " and its following positional arguments" : "");
                                }

                                fprintf(
                                    stderr,
                                    Pos_Fmt "... But this named argument directly passes another variadic source\n",
                                    Pos_Arg(it_name->token.pos));
                                exit(1);
                            }
                        }
                    } else {
                        // Start the variadic arguments as a stack allocated array
                        if (call->spread && call->spread != it) {
                            // There is a spread later, but another variadic allocation starts here
                            fprintf(
                                stderr,
                                Pos_Fmt "ERROR: Multiple typed variadic sources found\n",
                                Pos_Arg(call->node.token.pos));

                            bool following = false;
                            if (it->next) {
                                following = true;
                                Node *next = it->next;
                                if (next->kind == NODE_BINARY && next->token.kind == TOKEN_SET) {
                                    following = false;
                                }

                                if (next == call->spread) {
                                    following = false;
                                }
                            }

                            fprintf(
                                stderr,
                                Pos_Fmt "... This argument%s provides one source\n",
                                Pos_Arg(it->token.pos),
                                following ? " and its following positional arguments" : "");

                            fprintf(
                                stderr,
                                Pos_Fmt "... But this %s provides another\n",
                                Pos_Arg(call->spread_pos),
                                call->spread->kind == NODE_INTERPOLATION ? "interpolated string" : "spread");
                            exit(1);
                        }

                        if (!variadic_arg->node) {
                            variadic_arg->node = it;
                        }
                        continue;
                    }
                }

                if (n < fn_spec->args_count) {
                    args[n].node = it;
                }
            }
        }
        call->args_count += parts;
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
        if (args) {
            size_t not_provided_count = 0;
            SV     not_provided_name = {0};
            for (size_t i = 0; i < fn_spec->args_count; i++) {
                if (fn_spec->variadics_kind == VARIADICS_TYPED && i == fn_spec->variadics_index) {
                    continue;
                }

                const Type_Fn_Arg *it = &fn_spec->args[i];
                if (!args[i].node && !it->default_value && !it->default_value_is_caller_location) {
                    not_provided_count++;
                    if (not_provided_count == 1) {
                        not_provided_name = it->name;
                    } else if (not_provided_count == 2) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: The following arguments are not provided: " SV_Fmt ", " SV_Fmt,
                            Pos_Arg(call->end),
                            SV_Arg(not_provided_name),
                            SV_Arg(it->name));
                    } else {
                        fprintf(stderr, ", " SV_Fmt, SV_Arg(it->name));
                    }
                }
            }

            if (not_provided_count) {
                if (not_provided_count == 1) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Argument '" SV_Fmt "' is not provided\n",
                        Pos_Arg(call->end),
                        SV_Arg(not_provided_name));
                } else {
                    fprintf(stderr, "\n");
                }

                fprintf(
                    stderr,
                    Pos_Fmt "NOTE: The %s being called is %s\n",
                    Pos_Arg(call->fn->token.pos),
                    is_method ? "method" : "function",
                    fn_type_to_cstr_but_excluding_receiver_if_required(fn_spec, is_method));
                exit(1);
            }

            temp_reset(args);
        }

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
        expected - is_method,
        call->args_count - is_method);
    exit(1);
}

static void check_whether_member_access_is_valid(Node_Member *m) {
    if (m->rhs) {
        assert(m->lhs); // A bare '.(Type)' will error out at parse time
        if (!type_is_union(m->lhs->type) && !type_eq(m->lhs->type, (Type) {.kind = TYPE_ANY})) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot access variant of %s\n",
                Pos_Arg(m->node.token.pos),
                type_to_cstr(m->lhs->type));
            exit(1);
        }
    } else {
        if (sv_match(m->node.token.sv, "_")) {
            fprintf(stderr, Pos_Fmt "ERROR: Field '_' cannot be accessed\n", Pos_Arg(m->node.token.pos));
            exit(1);
        }
    }
}

static_assert(COUNT_OPERATORS == 12, "");
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
        static_assert(COUNT_TOKENS == 77, "");
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

        case TOKEN_ISTRING:
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
            break;

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

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        static_assert(COUNT_TOKENS == 77, "");
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
            n->is_memory = true;
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

        case TOKEN_ADD_ADD:
        case TOKEN_SUB_SUB:
            check_expr(c, unary->value, REF_ASSIGN, expected_type);
            n->type = type_assert_numeric(unary->value, true);
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

        case TOKEN_TYPEOF:
            check_expr(c, unary->value, REF_NONE, NULL);
            check_that_type_is_known(unary->value);
            n->type = unary->value->type;

            finalize_untyped_type(&n->type);
            n->type.is_meta = true;
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        static_assert(COUNT_TOKENS == 77, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
            check_expr(c, binary->lhs, REF_NONE, expected_type);
            check_expr(c, binary->rhs, REF_NONE, expected_type);
            type_assert_node(c, binary->rhs, binary->lhs);
            if (!type_is_numeric(binary->lhs->type) && !type_is_pointer(binary->lhs->type)) {
                binary->overload =
                    get_operator_overload(c, operator_name_from_token_kind(n->token.kind), binary->lhs, &n->token.pos);
            }
            n->type = binary->lhs->type;
            break;

        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
            check_expr(c, binary->lhs, REF_NONE, expected_type);
            check_expr(c, binary->rhs, REF_NONE, expected_type);
            type_assert_node(c, binary->rhs, binary->lhs);
            if (!type_is_numeric(binary->lhs->type)) {
                binary->overload =
                    get_operator_overload(c, operator_name_from_token_kind(n->token.kind), binary->lhs, &n->token.pos);
            }
            n->type = binary->lhs->type;
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
            if (!type_is_numeric(binary->lhs->type) && !type_is_pointer(binary->lhs->type)) {
                binary->overload =
                    get_operator_overload(c, operator_name_from_token_kind(n->token.kind), binary->lhs, &n->token.pos);
            }
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_EQ:
        case TOKEN_NE:
            check_expr(c, binary->lhs, REF_NONE, NULL);
            check_expr(c, binary->rhs, REF_NONE, &binary->lhs->type);

            if (type_is_union(binary->lhs->type)) {
                binary->union_check = binary->lhs;
                if (!node_is_null(binary->rhs)) {
                    type_assert_type(binary->rhs);
                    binary->union_check_index = get_union_type_index(binary->rhs, binary->lhs->type);
                }
            } else if (type_is_union(binary->rhs->type)) {
                binary->union_check = binary->rhs;
                if (!node_is_null(binary->lhs)) {
                    type_assert_type(binary->lhs);
                    binary->union_check_index = get_union_type_index(binary->lhs, binary->rhs->type);
                }
            } else if (type_eq(binary->lhs->type, (Type) {.kind = TYPE_ANY})) {
                binary->any_check = binary->lhs;
                if (!node_is_null(binary->rhs)) {
                    type_assert_type(binary->rhs);
                    binary->any_check_type = &binary->rhs->type;
                }
            } else if (type_eq(binary->rhs->type, (Type) {.kind = TYPE_ANY})) {
                binary->any_check = binary->rhs;
                if (!node_is_null(binary->lhs)) {
                    type_assert_type(binary->lhs);
                    binary->any_check_type = &binary->lhs->type;
                }
            } else {
                type_assert_node(c, binary->rhs, binary->lhs);
                check_that_type_is_known(binary->lhs);

                if (try_auto_cast_type_to_rtti(c, binary->lhs, c->type_info_pointer_type)) {
                    assert(try_auto_cast_type_to_rtti(c, binary->rhs, c->type_info_pointer_type));
                } else if (!type_is_scalar(binary->lhs->type)) {
                    binary->overload = get_operator_overload(
                        c, operator_name_from_token_kind(n->token.kind), binary->lhs, &n->token.pos);
                }
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
        if (member->lhs) {
            {
                Ref_Kind ref_member = ref;
                switch (ref_member) {
                case REF_ADDR:
                    ref_member = REF_ADDR_MEMBER;
                    break;

                case REF_ASSIGN:
                    ref_member = REF_ASSIGN_MEMBER;
                    break;

                case REF_NONE:
                case REF_ADDR_MEMBER:
                case REF_ASSIGN_MEMBER:
                    // Pass
                    break;
                }

                check_expr(c, member->lhs, ref_member, NULL);
            }

            check_that_type_is_known(member->lhs);

            is_ref_valid = true; // check_node() has already determined that the reference is valid

            // Method
            {
                Method_Spec spec = {0};
                if (get_method_spec(c, member->lhs->type, n->token.sv, &spec, NULL)) {
                    Node_Fn **method = ht_get(&c->methods_table, spec);
                    if (method) {
                        member->method = *method;
                        if (member->method->node.type.kind != TYPE_FN) {
                            assert(member->method->defined_as);
                            check_definition_if_needed(c, member->method->defined_as, REF_NONE);
                        }

                        n->type = member->method->node.type;
                        assert(n->type.kind == TYPE_FN);

                        const Type_Fn *method_spec = n->type.spec.fn;
                        assert(method_spec->args_count);

                        const Type receiver_type = method_spec->args[0].type;
                        if (receiver_type.ref > member->lhs->type.ref + 1) {
                            fprintf(
                                stderr,
                                Pos_Fmt "ERROR: Too many levels of pointer indirection in method call\n",
                                Pos_Arg(n->token.pos));
                            fprintf(
                                stderr,
                                Pos_Fmt "NOTE: This is of type %s, but the receiver is expected to be %s\n",
                                Pos_Arg(member->lhs->token.pos),
                                type_to_cstr(member->lhs->type),
                                type_to_cstr(receiver_type));
                            exit(1);
                        }

                        if (receiver_type.ref > member->lhs->type.ref && !member->lhs->is_memory) {
                            fprintf(
                                stderr,
                                Pos_Fmt "ERROR: Too many levels of pointer indirection in method call\n",
                                Pos_Arg(n->token.pos));
                            fprintf(
                                stderr,
                                Pos_Fmt "NOTE: This is of type %s, but the receiver is expected to be %s\n",
                                Pos_Arg(member->lhs->token.pos),
                                type_to_cstr(member->lhs->type),
                                type_to_cstr(receiver_type));
                            fprintf(
                                stderr,
                                Pos_Fmt
                                "NOTE: This value does not exist in memory, therefore cannot take reference to it\n",
                                Pos_Arg(member->lhs->token.pos));
                            exit(1);
                        }
                        is_ref_valid = ref == REF_NONE;
                    }
                }
            }

            if (!member->method) {
                n->is_memory = member->lhs->is_memory;
                if (member->lhs->type.is_meta && member->lhs->type.kind == TYPE_ENUM) {
                    check_whether_member_access_is_valid(member);
                    Node_Enum *enumm = member->lhs->type.spec.enumm.definition;
                    member->enum_value = get_enum_value(enumm, n->token.sv, &n->token);
                    member->is_enum = true;
                    n->type = member->lhs->type;
                    n->type.is_meta = false;
                } else if (type_kind_eq(member->lhs->type, TYPE_ANY)) {
                    check_whether_member_access_is_valid(member);
                    if (member->rhs) {
                        check_expr(c, member->rhs, REF_NONE, NULL);
                        type_assert_type(member->rhs);
                        n->type = member->rhs->type;
                        n->type.is_meta = false;
                    } else {
                        if (sv_match(n->token.sv, "case")) {
                            n->type = c->type_info_pointer_type;
                            member->field_index = 0;
                        } else if (sv_match(n->token.sv, "data")) {
                            n->type = (Type) {.kind = TYPE_RAWPTR};
                            member->field_index = 1;
                        } else {
                            error_undefined(&n->token, "field", false);
                        }

                        if (ref != REF_NONE) {
                            fprintf(
                                stderr,
                                Pos_Fmt "ERROR: Cannot %s to restricted fields of %s\n",
                                Pos_Arg(n->token.pos),
                                (ref == REF_ADDR || ref == REF_ADDR_MEMBER) ? "take reference" : "assign",
                                type_to_cstr(member->lhs->type));
                            exit(1);
                        }
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_UNION)) {
                    check_whether_member_access_is_valid(member);
                    if (member->rhs) {
                        check_expr(c, member->rhs, REF_NONE, NULL);
                        type_assert_type(member->rhs);
                        member->union_index = get_union_type_index(member->rhs, member->lhs->type);
                        n->type = member->rhs->type;
                        n->type.is_meta = false;
                    } else {
                        if (sv_match(n->token.sv, "case")) {
                            n->type = (Type) {.kind = TYPE_I64};
                            member->field_index = 0;
                        } else {
                            error_undefined(&n->token, "field or method", false);
                        }
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_STRUCT)) {
                    check_whether_member_access_is_valid(member);
                    Type_Struct_Field *definition = NULL;

                    Type_Struct *spec = member->lhs->type.spec.structt;
                    for (size_t i = 0; i < spec->fields_count; i++) {
                        Type_Struct_Field *it = &spec->fields[i];
                        if (sv_eq(it->name, n->token.sv)) {
                            definition = it;
                            member->field_index = i;
                            break;
                        }
                    }

                    if (!definition) {
                        error_undefined(&n->token, "field or method", true);
                        fprintf(
                            stderr,
                            Pos_Fmt "NOTE: Structure defined here\n",
                            Pos_Arg(spec->definition->node.token.pos));
                        exit(1);
                    }

                    n->type = definition->type;
                } else if (type_kind_eq(member->lhs->type, TYPE_ARRAY)) {
                    check_whether_member_access_is_valid(member);
                    if (sv_match(n->token.sv, "data")) {
                        n->type = *member->lhs->type.spec.array.element;
                        n->type.ref++;
                        member->field_index = 0;
                    } else if (sv_match(n->token.sv, "count")) {
                        n->type = (Type) {.kind = TYPE_I64};
                        member->field_index = 1;
                    } else {
                        error_undefined(&n->token, "field", false);
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_SLICE)) {
                    check_whether_member_access_is_valid(member);
                    if (sv_match(n->token.sv, "data")) {
                        n->type = *member->lhs->type.spec.slice.element;
                        n->type.ref++;
                        member->field_index = 0;
                    } else if (sv_match(n->token.sv, "count")) {
                        n->type = (Type) {.kind = TYPE_I64};
                        member->field_index = 1;
                    } else {
                        error_undefined(&n->token, "field", false);
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_STRING)) {
                    check_whether_member_access_is_valid(member);
                    if (sv_match(n->token.sv, "data")) {
                        n->type = (Type) {.kind = TYPE_CHAR, .ref = 1};
                        member->field_index = 0;
                    } else if (sv_match(n->token.sv, "count")) {
                        n->type = (Type) {.kind = TYPE_I64};
                        member->field_index = 1;
                    } else {
                        error_undefined(&n->token, "field", false);
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_MODULE)) {
                    check_whether_member_access_is_valid(member);
                    check_ident(c, n, ref);
                } else {
                    bool ok = false;
                    if (member->lhs->type.is_meta) {
                        Type receiver = member->lhs->type;
                        receiver.is_meta = false;

                        Method_Spec spec = {0};
                        if (get_method_spec(c, receiver, n->token.sv, &spec, NULL)) {
                            Node_Fn **method = ht_get(&c->methods_table, spec);
                            if (method) {
                                ok = true;
                                member->method = *method;
                                n->type = member->method->node.type;
                            } else {
                                error_undefined(&n->token, "method", false);
                            }
                        } else {
                            fprintf(
                                stderr,
                                Pos_Fmt "ERROR: There are no methods defined on %s\n",
                                Pos_Arg(n->token.pos),
                                type_to_cstr(receiver));
                            exit(1);
                        }
                    }

                    if (!ok) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Cannot access field of %s\n",
                            Pos_Arg(n->token.pos),
                            type_to_cstr(member->lhs->type));
                        exit(1);
                    }
                }
            }
        } else {
            check_whether_member_access_is_valid(member);
            n->type = (Type) {.kind = TYPE_UNKNOWN_ENUM};
            member->is_enum = true;

            if (expected_type && type_kind_eq(*expected_type, TYPE_ENUM)) {
                Node_Enum *enumm = expected_type->spec.enumm.definition;
                member->enum_value = get_enum_value(enumm, n->token.sv, &n->token);
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
                if (parser_import(c->parser, import)) {
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

    case NODE_INTERPOLATION: {
        Node_Interpolation *interp = (Node_Interpolation *) n;
        if (!interp->is_valid) {
            fprintf(
                stderr,
                Pos_Fmt
                "ERROR: Cannot use interpolated string here. It can only be used as a variadic source of type 'any' in a function call\n",
                Pos_Arg(n->token.pos));
            exit(1);
        }

        ll_foreach(it, &interp->children) {
            check_expr(c, it, REF_NONE, NULL);
            type_assert(c, it, (Type) {.kind = TYPE_ANY});
        }

        n->type = c->interpolated_string_type;
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;

        Context_Fn context_fn = {.fn = fn, .outer = c->context.fn};
        context_push_fn(&c->context, &context_fn);

        {
            Type_Fn *fn_spec = arena_alloc(c->arena, sizeof(*fn_spec));
            fn_spec->args = arena_alloc(c->arena, fn->args_count * sizeof(*fn_spec->args));
            fn_spec->args_count_min = fn->args_count_min;
            fn_spec->variadics_kind = fn->variadics_kind;

            for (Node *arg = fn->args.head; arg; arg = arg->next) {
                assert(arg->kind == NODE_DEFINE);
                Node_Define *define = (Node_Define *) arg;

                assert(define->name->kind == NODE_ATOM);
                Node_Atom *it = (Node_Atom *) define->name;
                if (!sv_match(it->node.token.sv, "_")) {
                    for (size_t i = 0; i < fn_spec->args_count; i++) {
                        Type_Fn_Arg previous = fn_spec->args[i];
                        if (sv_eq(previous.name, it->node.token.sv)) {
                            error_redefinition((Node *) it, &previous.pos);
                        }
                    }
                }

                fn_spec->args[fn_spec->args_count].name = it->node.token.sv;
                fn_spec->args[fn_spec->args_count].pos = it->node.token.pos;

                check_stmt(c, arg);
                if (define->has_spread) {
                    fn_spec->variadics_index = fn_spec->args_count;
                    it->node.type.kind = TYPE_SLICE;
                    it->node.type.spec.slice.element = &define->type->type;
                }

                if (define->expr) {
                    if (is_node_caller_location(define->expr)) {
                        fn_spec->args[fn_spec->args_count].default_value_is_caller_location = true;
                    } else {
                        it->definition_spec->const_value = eval_const_expr(c, define->expr);
                        fn_spec->args[fn_spec->args_count].default_value = &it->definition_spec->const_value;
                    }
                }
                fn_spec->args[fn_spec->args_count].type = it->node.type;
                fn_spec->args_count += define->count;
            }

            if (fn->returns.head) {
                fn_spec->returns = arena_alloc(c->arena, fn->returns_count * sizeof(*fn_spec->returns));

                size_t iota = 0;
                ll_foreach(it, &fn->returns) {
                    check_expr(c, it, REF_NONE, NULL);
                    type_assert_type(it);

                    fn_spec->returns[iota] = it->type;
                    fn_spec->returns[iota].is_meta = false;
                    iota++;
                }
            }
            fn_spec->returns_count = fn->returns_count;

            Type return_type = {0};
            if (fn_spec->returns_count == 0) {
                return_type.kind = TYPE_UNIT;
            } else if (fn_spec->returns_count == 1) {
                return_type = *fn_spec->returns;
            } else {
                return_type.kind = TYPE_GROUP;
                return_type.spec.group.data = fn_spec->returns;
                return_type.spec.group.count = fn_spec->returns_count;
            }
            fn_spec->return_type = arena_clone(c->arena, &return_type, sizeof(return_type));

            n->type = (Type) {.kind = TYPE_FN, .spec.fn = fn_spec};

            if (fn->defined_as) {
                // The body of a function is irrelevant for outer expressions
                fn->defined_as->node.type = n->type;
                fn->defined_as->definition_spec->check_status = CHECKED;
            }

            if (fn->operator_kind) {
                static_assert(COUNT_TOKENS == 77, "");
                switch (fn->operator_kind) {
                case OPERATOR_ADD:
                case OPERATOR_SUB:
                case OPERATOR_MUL:
                case OPERATOR_DIV:
                case OPERATOR_MOD: {
                    if (fn_spec->args_count != 2) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Expected 2 arguments, got %zu\n",
                            Pos_Arg(n->token.pos),
                            fn_spec->args_count);
                        exit(1);
                    }

                    const Type lhs_type = fn_spec->args[0].type;
                    const Type rhs_type = fn_spec->args[1].type;
                    if (!type_eq(lhs_type, rhs_type)) {
                        Node *second = fn->args.head->next;
                        assert(second && second->kind == NODE_DEFINE);

                        Node_Define *define = (Node_Define *) second;
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Operand types must be same: Expected %s, got %s\n",
                            Pos_Arg(define->type->token.pos),
                            type_to_cstr(lhs_type),
                            type_to_cstr(rhs_type));
                        exit(1);
                    }

                    if (!type_eq(lhs_type, *fn_spec->return_type)) {
                        fprintf(
                            stderr,
                            Pos_Fmt
                            "ERROR: Operand types and return type must be same: Expected to return %s, got %s\n",
                            Pos_Arg(fn->returns.head ? fn->returns.head->token.pos : fn->body->token.pos),
                            type_to_cstr(lhs_type),
                            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
                        exit(1);
                    }
                } break;

                case OPERATOR_GT:
                case OPERATOR_GE:
                case OPERATOR_LT:
                case OPERATOR_LE:
                case OPERATOR_EQ:
                case OPERATOR_NE: {
                    if (fn_spec->args_count != 2) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Expected 2 arguments, got %zu\n",
                            Pos_Arg(n->token.pos),
                            fn_spec->args_count);
                        exit(1);
                    }

                    const Type lhs_type = fn_spec->args[0].type;
                    const Type rhs_type = fn_spec->args[1].type;
                    if (!type_eq(lhs_type, rhs_type)) {
                        Node *second = fn->args.head->next;
                        assert(second && second->kind == NODE_DEFINE);

                        Node_Define *define = (Node_Define *) second;
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Operand types must be same: Expected %s, got %s\n",
                            Pos_Arg(define->type->token.pos),
                            type_to_cstr(lhs_type),
                            type_to_cstr(rhs_type));
                        exit(1);
                    }

                    const Type bool_type = {.kind = TYPE_BOOL};
                    if (!type_eq(bool_type, *fn_spec->return_type)) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Expected to return %s, got %s\n",
                            Pos_Arg(fn->returns.head ? fn->returns.head->token.pos : fn->body->token.pos),
                            type_to_cstr(bool_type),
                            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
                        exit(1);
                    }
                } break;

                default:
                    unreachable();
                    break;
                }
            }

            if (fn->is_type) {
                n->type.is_meta = true;
                is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
            } else if (fn->body) {
                check_stmt(c, fn->body);
                if (fn_spec->returns_count && !always_returns(fn->body)) {
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

        n->type = (Type) {.kind = TYPE_ENUM, .is_meta = true, .spec.enumm = spec};
    } break;

    case NODE_UNION: {
        Node_Union *unionn = (Node_Union *) n;

        Type_Union *spec = arena_alloc(c->arena, sizeof(*spec));
        spec->definition = unionn;

        n->type = (Type) {
            .kind = TYPE_UNION,
            .is_meta = true,
            .spec.unionn = spec,
        };

        if (unionn->defined_as) {
            unionn->defined_as->node.type = n->type;
        }

        spec->variants = arena_alloc(c->arena, unionn->variants_count * sizeof(*spec->variants));
        spec->variants_count = unionn->variants_count;

        size_t iota = 0;
        ll_foreach(it, &unionn->variants) {
            check_expr(c, it, REF_NONE, NULL);
            type_assert_type(it);

            Type_Union_Variant *variant = &spec->variants[iota];
            variant->pos = it->token.pos;
            variant->type = it->type;
            variant->type.is_meta = false;

            for (size_t i = 0; i < iota; i++) {
                const Type_Union_Variant *prev = &spec->variants[i];
                if (type_eq(prev->type, variant->type)) {
                    error_redefinition(it, &prev->pos);
                }
            }

            iota++;
        }

        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
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
        ll_foreach(field, &structt->fields) {
            if (field->kind == NODE_DEFINE) {
                Node_Define *define = (Node_Define *) field;

                Node_Atom *it = NULL;
                while ((it = (Node_Atom *) node_iter((Node *) it, define->name))) {
                    if (!sv_match(it->node.token.sv, "_")) {
                        for (size_t i = fields_start; i < c->struct_fields.count; i++) {
                            Type_Struct_Field previous = c->struct_fields.data[i];
                            if (sv_eq(previous.name, it->node.token.sv)) {
                                error_redefinition((Node *) it, &previous.pos);
                            }
                        }
                    }

                    it->definition_spec->is_local = false;
                    check_definition(c, it, define->expr, define->type);

                    const Type_Struct_Field it_field = {
                        .name = it->node.token.sv,
                        .pos = it->node.token.pos,
                        .type = it->node.type,
                    };
                    da_push(&c->struct_fields, it_field);
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
                        for (size_t i = fields_start; i < c->struct_fields.count; i++) {
                            Type_Struct_Field previous = c->struct_fields.data[i];
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
                }
            } else {
                unreachable();
            }
        }

        const size_t fields_count = c->struct_fields.count - fields_start;
        if (fields_count) {
            spec->fields = arena_clone(
                c->arena, &c->struct_fields.data[fields_start], fields_count * sizeof(*c->struct_fields.data));
            spec->fields_count = fields_count;
        }

        c->struct_fields.count = fields_start;
        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
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
            if (expected_type) {
                if (type_kind_eq(*expected_type, TYPE_STRUCT) || type_kind_eq(*expected_type, TYPE_ARRAY) ||
                    type_kind_eq(*expected_type, TYPE_SLICE)) {
                    n->type = *expected_type;
                }
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

        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
        n->is_memory = true;
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
            Type *from_type = &call->args.head->type;
            Type *to_type = &n->type;

            bool same = false;
            bool to_union = false;
            if (type_eq_without_distinct(*to_type, *from_type)) {
                same = true;
            } else if (type_is_union(*to_type)) {
                to_union = true;
            } else if (type_is_scalar(*to_type)) {
                // Pass
            } else {
                fprintf(
                    stderr, Pos_Fmt "ERROR: Cannot cast to %s\n", Pos_Arg(call->fn->token.pos), type_to_cstr(*to_type));
                exit(1);
            }

            if (!same) {
                if (to_union) {
                    finalize_untyped_type(from_type);
                    call->type_cast_union_index = get_union_type_index(call->args.head, *to_type);
                } else if (type_is_scalar(*to_type)) {
                    type_assert_scalar(call->args.head);

                    bool ok = true;
                    if (type_kind_eq(*from_type, TYPE_FN) && !from_type->ref) {
                        // fn -> rawptr
                        ok = type_eq(*to_type, (Type) {.kind = TYPE_RAWPTR});
                    } else if (type_kind_eq(*to_type, TYPE_FN) && !to_type->ref) {
                        // rawptr -> fn
                        ok = type_eq(*from_type, (Type) {.kind = TYPE_RAWPTR});
                    } else if (!type_is_pointer(*from_type) && type_is_pointer(*to_type)) {
                        // i64/u64 -> ptr
                        if (!type_kind_eq(*from_type, TYPE_I64) && !type_kind_eq(*from_type, TYPE_U64) &&
                            !type_kind_eq(*from_type, TYPE_INT)) {
                            ok = false;
                        }
                    } else if (type_is_pointer(*from_type) && !type_is_pointer(*to_type)) {
                        // ptr -> i64/u64
                        if (!type_kind_eq(*to_type, TYPE_I64) && !type_kind_eq(*to_type, TYPE_U64) &&
                            !type_kind_eq(*to_type, TYPE_INT)) {
                            ok = false;
                        }
                    }

                    if (!ok) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Cannot cast %s to %s\n",
                            Pos_Arg(call->fn->token.pos),
                            type_to_cstr(*from_type),
                            type_to_cstr(*to_type));
                        exit(1);
                    }
                } else {
                    unreachable();
                }
            }

            if (same) {
                call->type_cast = TYPE_CAST_NOP;
            } else if (type_eq(*to_type, (Type) {.kind = TYPE_BOOL})) {
                call->type_cast = TYPE_CAST_TO_BOOL;
            } else if (to_union) {
                call->type_cast = TYPE_CAST_TO_UNION;
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
            const Type_Fn *fn_type_spec = fn_type.spec.fn;

            check_call_arguments(c, call, fn_type_spec);

            n->type = *fn_type_spec->return_type;
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

            is_ref_valid = ref == REF_NONE;
        } else {
            n->is_memory = index->lhs->is_memory;
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

            const u64 max_count = INT64_MAX;
            if (array_count > max_count) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Number '%zd' is invalid for array capacity, which must be in range [0, %zu]\n",
                    Pos_Arg(indexable->count->token.pos),
                    array_count,
                    max_count);
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

        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
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
        case REF_ADDR_MEMBER:
            if (!n->type.is_meta) {
                fprintf(stderr, Pos_Fmt "ERROR: Cannot take reference to value not in memory\n", Pos_Arg(n->token.pos));
                exit(1);
            }
            break;

        case REF_ASSIGN:
        case REF_ASSIGN_MEMBER:
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

        const size_t context_end_save = c->context.fn->end;
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
            if (iff->compile_time_real) {
                Context_Replace *context_replace_save = c->context.replace;
                c->context.replace = &iff->context_replace;

                if (iff->compile_time_real->kind == NODE_BLOCK) {
                    Node_Block *block = (Node_Block *) iff->compile_time_real;
                    for (Node *it = block->body.head; it; it = it->next) {
                        check_stmt(c, it);
                    }
                } else {
                    check_stmt(c, iff->compile_time_real);
                }

                c->context.replace = context_replace_save;
            }
        } else {
            check_expr(c, iff->condition, REF_NONE, NULL);
            type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});

            iff->context_replace.outer = c->context.replace;
            if (iff->condition->kind == NODE_BINARY && iff->condition->token.kind == TOKEN_EQ) {
                Node_Binary *condition = (Node_Binary *) iff->condition;
                const Type   any_type = {.kind = TYPE_ANY};
                if ((type_eq(condition->lhs->type, any_type) || type_is_union(condition->lhs->type)) &&
                    condition->lhs->kind == NODE_ATOM) {
                    if (!node_is_null(condition->rhs)) {
                        push_context_replace(
                            c, &iff->context_replace, ((Node_Atom *) condition->lhs)->definition, condition->rhs->type);
                    }
                } else if (
                    (type_eq(condition->rhs->type, any_type) || type_is_union(condition->rhs->type)) &&
                    condition->rhs->kind == NODE_ATOM) {
                    if (!node_is_null(condition->lhs)) {
                        push_context_replace(
                            c, &iff->context_replace, ((Node_Atom *) condition->rhs)->definition, condition->lhs->type);
                    }
                }
            }

            check_stmt(c, iff->consequence);
            c->context.replace = iff->context_replace.outer;

            check_stmt(c, iff->antecedence);
        }
    } break;

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;

        const size_t context_end_save = c->context.fn->end;
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
            if (sw->compile_time_real) {
                Context_Replace *context_replace_save = c->context.replace;

                Node_Case *branch = sw->compile_time_real;
                c->context.replace = &branch->context_replace;

                assert(branch->body->kind == NODE_BLOCK);
                Node_Block *block = (Node_Block *) branch->body;
                for (Node *it = block->body.head; it; it = it->next) {
                    check_stmt(c, it);
                }

                c->context.replace = context_replace_save;
            }
        } else {
            size_t iota = 0;
            for (Node *it = sw->cases.head; it; it = it->next) {
                Node_Case *branch = (Node_Case *) it;
                for (Node *pred = branch->preds.head; pred; pred = pred->next) {
                    check_switch_pred(c, sw, pred, &iota);
                }

                branch->context_replace.outer = c->context.replace;
                if ((sw->unionn || sw->is_expr_any) && sw->expr->kind == NODE_ATOM && branch->preds_count == 1) {
                    if (!node_is_null(branch->preds.head)) {
                        push_context_replace(
                            c,
                            &branch->context_replace,
                            ((Node_Atom *) sw->expr)->definition,
                            branch->preds.head->type);
                    }
                }

                check_stmt(c, branch->body);
                c->context.replace = branch->context_replace.outer;
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
        Node_Return   *returnn = (Node_Return *) n;
        const Type_Fn *fn_type = c->context.fn->fn->node.type.spec.fn;
        if (returnn->value) {
            check_expr(c, returnn->value, REF_NONE, fn_type->return_type);

            const bool   is_group = type_kind_eq(returnn->value->type, TYPE_GROUP);
            const size_t actual_count = is_group ? returnn->value->type.spec.group.count : 1;

            if (actual_count != fn_type->returns_count) {
                error_number_of_return_values_mismatch(n->token.pos, fn_type->returns_count, actual_count);
            }

            assert(actual_count == fn_type->returns_count);
            for (size_t i = 0; i < fn_type->returns_count; i++) {
                i64   group_index = -1;
                Node *n = get_node_from_group(returnn->value, i, &group_index);
                type_assert_grouped(c, n, fn_type->returns[i], group_index, NULL);
            }

            // The inference of the individual group items might not have reflected here
            returnn->value->type = *fn_type->return_type;
        } else {
            if (fn_type->returns_count) {
                error_number_of_return_values_mismatch(n->token.pos, fn_type->returns_count, 0);
            }
        }

        n->type = *fn_type->return_type;
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
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

#ifdef PLATFORM_X86_64_LINUX
    return const_value_int(CONTRACT_PLATFORM_LINUX);
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    return const_value_int(CONTRACT_PLATFORM_MACOS);
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
    return const_value_int(CONTRACT_PLATFORM_WINDOWS);
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

static uint64_t ht_hasheq_method_spec(const void *va, const void *vb, size_t n) {
    unused(n);

    const Method_Spec a = *(const Method_Spec *) va;
    if (vb) {
        const Method_Spec b = *(const Method_Spec *) vb;
        return a.uid == b.uid && sv_eq(a.name, b.name);
    }

    const uint64_t receiver_hash = ht_hasheq_bytes(&a.uid, NULL, sizeof(a.uid));
    const uint64_t name_hash = ht_hasheq_bytes(a.name.data, NULL, a.name.count);
    return ht_hash_combine(receiver_hash, name_hash);
}

void check_nodes(Compiler *c) {
    assert(c->parser);
    assert(c->modules);
    assert(c->main_module);
    assert(c->builtin_module);

    c->methods_table.hasheq = ht_hasheq_method_spec;

    {
        Type_Fn *fn_spec = arena_alloc(c->arena, sizeof(*fn_spec));

        const Type unit = {.kind = TYPE_UNIT};
        fn_spec->return_type = arena_clone(c->arena, &unit, sizeof(unit));

        c->main_fn_type = (Type) {
            .kind = TYPE_FN,
            .spec.fn = fn_spec,
        };
    }

    {
        const Type any = {.kind = TYPE_ANY};
        c->interpolated_string_type = (Type) {
            .kind = TYPE_SLICE,
            .spec.slice.element = arena_clone(c->arena, &any, sizeof(any)),
        };
    }

    for (Module *m = c->modules->head; m; m = m->next) {
        for (Node *it = m->nodes.head; it; it = it->next) {
            define_orderless_nodes(c, it, 0);
        }
    }

    // Type info
    {
        const Const_Value value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Type_Info"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->type_info_type = value.as.type;
        c->type_info_type.is_meta = false;

        c->type_info_pointer_type = c->type_info_type;
        c->type_info_pointer_type.ref++;

        assert(c->type_info_pointer_type.kind == TYPE_STRUCT);
        const Type_Struct *type_info_structure = c->type_info_pointer_type.spec.structt;

        assert(type_info_structure->fields_count == 4);
        const Type *type_info_variant = &type_info_structure->fields[2].type;

        assert(type_info_variant->kind == TYPE_UNION);
        c->type_info_variants_union = type_info_variant->spec.unionn;
        assert(c->type_info_variants_union->variants_count == 12);

        static_assert(COUNT_TYPES == 25, "");
        c->type_info_variants[TYPE_BOOL] = CONTRACT_TYPE_INFO_BOOLEAN;
        c->type_info_variants[TYPE_CHAR] = CONTRACT_TYPE_INFO_CHARACTER;

        c->type_info_variants[TYPE_I8] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_I16] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_I32] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_I64] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_INT] = CONTRACT_TYPE_INFO_INTEGER;

        c->type_info_variants[TYPE_U8] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U16] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U32] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U64] = CONTRACT_TYPE_INFO_INTEGER;

        c->type_info_variants[TYPE_RAWPTR] = CONTRACT_TYPE_INFO_POINTER;

        c->type_info_variants[TYPE_FN] = CONTRACT_TYPE_INFO_FUNCTION;
        c->type_info_variants[TYPE_ENUM] = CONTRACT_TYPE_INFO_ENUMERATION;
        c->type_info_variants[TYPE_UNION] = CONTRACT_TYPE_INFO_UNION;
        c->type_info_variants[TYPE_STRUCT] = CONTRACT_TYPE_INFO_STRUCTURE;

        c->type_info_variants[TYPE_ARRAY] = CONTRACT_TYPE_INFO_ARRAY;
        c->type_info_variants[TYPE_SLICE] = CONTRACT_TYPE_INFO_SLICE;
        c->type_info_variants[TYPE_STRING] = CONTRACT_TYPE_INFO_STRING;
        c->type_info_variants[TYPE_ANY] = CONTRACT_TYPE_INFO_ANY;
    }

    // Source code location
    {
        const Const_Value value =
            get_const_definition_value(c, c->builtin_module, sv_from_cstr("Source_Code_Location"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);

        c->source_code_location_type = value.as.type;
        c->source_code_location_type.is_meta = false;
    }

    // Define the methods
    {
        for (size_t i = 0; i < c->methods_list.count; i++) {
            Node_Fn *fn = c->methods_list.data[i];
            assert(fn->args.head && fn->args.head->kind == NODE_DEFINE); // Guaranteed by the parser

            Node_Define *define = (Node_Define *) fn->args.head;
            assert(define->name->kind == NODE_ATOM && define->type); // Guaranteed by the parser

            if (!fn->defined_as) {
                fprintf(stderr, Pos_Fmt "ERROR: Anonymous function cannot be a method\n", Pos_Arg(fn->node.token.pos));
                fprintf(
                    stderr,
                    Pos_Fmt "NOTE: This argument is taken to be the receiver\n",
                    Pos_Arg(define->name->token.pos));
                exit(1);
            }
            const SV name = fn->defined_as->node.token.sv;

            check_expr(c, define->type, REF_NONE, NULL);
            type_assert_type(define->type);
            define->type->type.is_meta = false;
            const Type receiver_type = define->type->type;

            Method_Spec spec = {0};
            if (get_method_spec(c, receiver_type, name, &spec, fn->module)) {
                if (type_kind_eq(receiver_type, TYPE_ENUM)) {
                    ll_foreach(it, &receiver_type.spec.enumm.definition->values) {
                        if (sv_eq(it->token.sv, name)) {
                            error_redefinition((Node *) fn->defined_as, &it->token.pos);
                        }
                    }
                } else if (type_kind_eq(receiver_type, TYPE_STRUCT)) {
                    for (size_t i = 0; i < receiver_type.spec.structt->fields_count; i++) {
                        const Type_Struct_Field it = receiver_type.spec.structt->fields[i];
                        if (sv_eq(it.name, name)) {
                            error_redefinition((Node *) fn->defined_as, &it.pos);
                        }
                    }
                }

                Node_Fn **previous = ht_get(&c->methods_table, spec);
                if (previous) {
                    error_redefinition((Node *) fn->defined_as, &(*previous)->defined_as->node.token.pos);
                }
                ht_set(&c->methods_table, spec, fn);
            } else {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Can only define methods on types defined in the same module\n",
                    Pos_Arg(fn->node.token.pos));
                fprintf(
                    stderr,
                    Pos_Fmt "NOTE: This argument is taken to be the receiver\n",
                    Pos_Arg(define->name->token.pos));
                exit(1);
            }
        }
    }

    for (Module *m = c->modules->head; m; m = m->next) {
        for (Node *it = m->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
    }

    get_main(c);
}

// TODO: Sometimes non-cyclic definitions are falsely flagged as cyclic
// TODO: Private methods
