#include "../error.h"
#include "checker.h"

static_assert(COUNT_TOKENS == 94, "");
SV token_kind_to_operator_method_name(Token_Kind kind) {
    switch (kind) {
    case TOKEN_ADD:
    case TOKEN_ADD_SET:
        return OPERATOR_ADD;

    case TOKEN_SUB:
    case TOKEN_SUB_SET:
        return OPERATOR_SUB;

    case TOKEN_MUL:
    case TOKEN_MUL_SET:
        return OPERATOR_MUL;

    case TOKEN_DIV:
    case TOKEN_DIV_SET:
        return OPERATOR_DIV;

    case TOKEN_MOD:
    case TOKEN_MOD_SET:
        return OPERATOR_MOD;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return OPERATOR_CMP;

    case TOKEN_OPERATOR_CMP:
        return OPERATOR_CMP;

    case TOKEN_OPERATOR_INDEX:
        return OPERATOR_INDEX;

    case TOKEN_OPERATOR_SLICE:
        return OPERATOR_SLICE;

    default:
        unreachable();
    }
}

void check_that_methods_can_be_accessed(Compiler *c, Node *receiver) {
    if (c->methods_to_check.count && !type_kind_eq(receiver->type, TYPE_MODULE)) {
        error_node(EK_ERROR, receiver, "Cannot access methods at this stage of compilation yet");
        if (c->current_comptime_conditional_stmt) {
            error_node(EK_NOTE, c->current_comptime_conditional_stmt, "Evaluating this conditional statement");
            afprintf(
                stderr,
                ANSI_COLOR_YELLOW | ANSI_BOLD,
                "    The '#if' statements are evaluated immediately instead of waiting for all the definitions\n"
                "    to be registered. Therefore at this point of time, the methods might not be defined yet.\n"
                "    Thus, to prevent inconsistent behaviour, any operations involving them are disallowed.\n\n");
        }
        exit(c, 1);
    }
}

bool get_method_spec(
    Compiler *c, Node *receiver_node, Type receiver_type, SV name, Method_Spec *spec, Method_Defining *defining) //
{
    if (spec) {
        spec->name = name;
    }

    if (type_kind_eq(receiver_type, TYPE_ENUM)) {
        Node_Enum *definition = receiver_type.spec.enumm.definition;
        if (spec) {
            spec->uid = (uintptr_t) definition;
        }

        if (defining) {
            defining->defined_as = definition->defined_as;
            defining->block = definition->defined_in_block;
            defining->module = definition->node.module;
            defining->is_named = definition->defined_as != NULL;
        } else {
            check_that_methods_can_be_accessed(c, receiver_node);
        }

        return true;
    } else if (type_kind_eq(receiver_type, TYPE_TRAIT)) {
        Node_Trait *definition = receiver_type.spec.trait->definition;
        if (spec) {
            spec->uid = (uintptr_t) definition;
        }

        if (defining) {
            defining->defined_as = definition->defined_as;
            defining->is_named = definition->defined_as != NULL;
            defining->block = definition->defined_in_block;
            defining->module = definition->node.module;
        } else {
            check_that_methods_can_be_accessed(c, receiver_node);
        }

        return true;
    } else if (type_kind_eq(receiver_type, TYPE_UNION)) {
        Node_Union *definition = receiver_type.spec.unionn->definition;
        if (spec) {
            spec->uid = (uintptr_t) definition;
        }

        if (defining) {
            defining->defined_as = definition->defined_as;
            defining->is_named = definition->defined_as != NULL;
            defining->block = definition->defined_in_block;
            defining->module = definition->node.module;
        } else {
            check_that_methods_can_be_accessed(c, receiver_node);
        }

        return true;
    } else if (type_kind_eq(receiver_type, TYPE_STRUCT)) {
        Node_Struct *definition = receiver_type.spec.structt->original_definition;
        if (spec) {
            spec->uid = (uintptr_t) definition;
        }

        if (defining) {
            defining->defined_as = definition->defined_as;
            defining->is_named = definition->defined_as != NULL;
            defining->block = definition->defined_in_block;
            defining->module = definition->node.module;
        } else {
            check_that_methods_can_be_accessed(c, receiver_node);
        }

        return true;
    } else if (receiver_type.distinct) {
        if (spec) {
            spec->uid = (uintptr_t) receiver_type.distinct;
        }

        if (defining) {
            defining->defined_as = receiver_type.distinct;
            defining->is_named = true;
            defining->block = receiver_type.distinct->definition_spec->defined_in_block;
            defining->module = receiver_type.distinct->node.module;
        } else {
            check_that_methods_can_be_accessed(c, receiver_node);
        }

        return true;
    }

    static const uint8_t builtin_type_kinds[COUNT_TYPES];
    for (Type_Kind kind = 0; kind < COUNT_TYPES; kind++) {
        if (type_kind_eq(receiver_type, kind)) {
            if (spec) {
                spec->uid = (uintptr_t) &builtin_type_kinds[kind];
            }

            if (defining) {
                defining->is_named = true;
                defining->block = NULL;
                defining->module = c->builtin_module;
            } else {
                check_that_methods_can_be_accessed(c, receiver_node);
            }

            return true;
        }
    }

    return false;
}

Node_Fn *get_method(Compiler *c, Method_Spec spec, Module *module) {
    Node_Fn **fn = ht_get(&c->methods_table, spec);
    if (!fn) {
        return NULL;
    }

    Node_Fn *method = *fn;
    assert(method->defined_as);

    if (method->node.module != module && method->defined_as->definition_spec->is_private) {
        return NULL;
    }

    if (method->node.type.kind != TYPE_FN) {
        check_definition_if_needed(c, method->defined_as, NULL, REF_NONE);
    }

    return method;
}

typedef enum {
    OMS_ARITH = 1,
    OMS_CMP,
    OMS_INDEX,
    OMS_SLICE,
    OMS_RANGE,
} OMS;

static void pretty_print_oms(SV name, OMS oms, const Type *receiver, bool partial_comparison_acceptable) {
    ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
    fprintf(stderr, "        %s" SV_Fmt " :: ", oms == OMS_RANGE ? "" : "operator ", SV_Arg(name));

    const char *T = NULL;
    switch (oms) {
    case OMS_ARITH:
        T = type_to_cstr_raw(type_without_ref(*receiver));
        fprintf(stderr, "(this: %s, that: %s) -> %s", T, T, T);
        break;

    case OMS_CMP:
        T = type_to_cstr_raw(type_without_ref(*receiver));
        if (partial_comparison_acceptable) {
            fprintf(stderr, "(this: %s, that: %s) -> Ordering | Equivalence", T, T);
        } else {
            fprintf(stderr, "(this: %s, that: %s) -> Ordering", T, T);
        }
        break;

    case OMS_INDEX:
        T = type_to_cstr_raw(*receiver);
        fprintf(stderr, "(this: %s, key: K, assign: bool) -> &V", T);
        break;

    case OMS_SLICE:
        T = type_to_cstr_raw(*receiver);
        fprintf(stderr, "(this: %s, begin: A, end: A) -> V", T);
        break;

    case OMS_RANGE:
        T = type_to_cstr_raw(*receiver);
        fprintf(stderr, "(this: %s, state: &A) -> &V1, &V2, ..., bool", T);
        break;
    }
    fprintf(stderr, " {}\n\n");

    ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
    if (oms == OMS_CMP && partial_comparison_acceptable) {
        fprintf(
            stderr,
            "    Return 'Ordering' if you want this method to implement both equality checking as well as ordered comparisons.\n"
            "    Otherwise return 'Equivalence' to implement just equality checking. Do NOT return 'Ordering | Equivalence' literally.\n\n");
    }

    if (oms == OMS_RANGE) {
        fprintf(
            stderr,
            "    Iteration can be by reference or by value. By default, when you implement an iterator, it only works by value.\n"
            "    However you can implement both semantics using the '#reference' directive.\n"

            "\n");

        ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
        fprintf(
            stderr,
            "        range :: (this: &Iterable, state: &Iterator) -> A, #reference B, bool {}\n"
            "\n"
            "        usage :: () {\n"
            "            iterable: Iterable\n"
            "            for a, b := range iterable {\n"
            "                // Here both 'a' and 'b' are by value.\n"
            "            }\n"
            "\n"
            "            // To iterate by reference, take a reference to the iterable value.\n"
            "            for a, b := range &iterable {\n"
            "                // Here 'a' is by value and 'b' is by reference.\n"
            "            }\n"
            "        }\n"
            "\n");

        ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
        fprintf(stderr, "    Notice that in the above example, the receiver of the iterator method is a pointer.\n\n");
    }

    fprintf(
        stderr,
        "    It may have other optional arguments at the end, but this is the bare minimum that must be implemented.\n"
        "\n");

    ansi_reset(stderr);
}

Node_Fn *get_operator_overload(Compiler *c, SV operator, Node *receiver, Node *op, Module *module) {
    return get_operator_overload_ex(c, operator, receiver->type, op, module, true, true, receiver, -1);
}

Node_Fn *get_operator_overload_ex(
    Compiler *c,
    SV        operator,
    Type      receiver,
    Node     *op,
    Module   *module,
    bool      monomorphize_if_needed,
    bool      partial_comparison_acceptable,
    Node     *n,
    i64       group_index) //
{
    unused(partial_comparison_acceptable);
    Method_Spec spec = {0};
    if (get_method_spec(c, n, receiver, operator, &spec, NULL)) {
        if (sv_eq(operator, OPERATOR_MUL) || sv_eq(operator, OPERATOR_DIV) || sv_eq(operator, OPERATOR_MOD)) {
            if (type_is_pointer(receiver)) {
                error_node(EK_ERROR, n, "The operator '" SV_Fmt "' is not valid for pointers", SV_Arg(operator));
                if (group_index == -1) {
                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    The value is of type %s\n\n",
                        type_to_cstr(receiver));
                } else {
                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    The %zu%s value of this expression has type %s. The type of this entire expression is %s\n\n",
                        group_index + 1,
                        order_postfix(group_index + 1),
                        type_to_cstr(receiver),
                        type_to_cstr(n->type));
                }
                exit(c, 1);
            }
        }

        Node_Fn *method = get_method(c, spec, module);
        if (method) {
            if (method->polymorphs.count && monomorphize_if_needed) {
                Call_Checker cc = {0};
                cc.expr = op;
                cc.fn_source = op;
                cc.fn = (Node *) method;
                cc.end = op->token;

                cc.is_method = true;
                cc.receiver = n;
                cc.is_polymorph = true;

                check_call_arguments(c, &cc, false);
                assert(cc.fn->kind == NODE_FN);

                method = (Node_Fn *) cc.fn;
            }

            if (sv_eq(spec.name, OPERATOR_CMP)) {
                if (!partial_comparison_acceptable && !method->is_compare_operator_complete) {
                    assert(method->returns.head);
                    error_node(EK_ERROR, n, "Type %s does not implement ordered comparisons", type_to_cstr(receiver));
                    error_node(
                        EK_NOTE,
                        method->returns.head,
                        "The method '" SV_Fmt "' only implements equality checking since its return type is %s, not %s",
                        SV_Arg(method->defined_as->node.token.sv),
                        type_to_cstr(c->equivalence_type),
                        type_to_cstr(c->ordering_type));
                    exit(c, 1);
                }
            }
            return method;
        }
    }

    check_that_type_is_known(c, n);
    error_node_begin(EK_ERROR, op);

    OMS oms = 0;
    if (sv_eq(spec.name, OPERATOR_ADD)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_SUB)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_MUL)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_DIV)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_MOD)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_CMP)) {
        oms = OMS_CMP;
    } else if (sv_eq(spec.name, OPERATOR_INDEX)) {
        oms = OMS_INDEX;
    } else if (sv_eq(spec.name, OPERATOR_SLICE)) {
        oms = OMS_SLICE;
    } else if (sv_eq(spec.name, OPERATOR_RANGE)) {
        oms = OMS_RANGE;
    } else {
        unreachable();
    }

    if (oms == OMS_RANGE) {
        fprintf(stderr, "Iteration is not defined for %s", type_to_cstr(receiver));
    } else {
        fprintf(stderr, "Operator '" SV_Fmt "' is not defined for %s", SV_Arg(spec.name), type_to_cstr(receiver));
    }

    if (group_index != -1) {
        const char *postfix = order_postfix(group_index + 1);
        fprintf(
            stderr,
            ", which is the %zd%s value of this expression. The type of the entire expression is %s.",
            group_index + 1,
            postfix,
            type_to_cstr(n->type));
    }

    error_finalize();
    ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
    fprintf(stderr, "    Implement this:\n\n");
    pretty_print_oms(spec.name, oms, &receiver, partial_comparison_acceptable);
    exit(c, 1);
}

static void error_operator_method_wrong_signature(Token name, OMS oms, const Type *receiver) {
    error_token(
        EK_ERROR,
        name,
        "The method '" SV_Fmt "' is special because it implements an %s overload",
        SV_Arg(name.sv),
        oms == OMS_RANGE ? "iterator" : "operator");

    afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "    It should have this signature:\n\n");
    pretty_print_oms(name.sv, oms, receiver, true);
}

static void check_operator_method_signature_args_count_and_that_receiver_is_not_scalar(
    Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec, const size_t args_count, OMS oms) //
{
    const Type *receiver = &fn_spec->args[0].type;
    if (oms == OMS_RANGE) {
        assert(!receiver->is_meta);
        switch (receiver->kind) {
        case TYPE_ARRAY:
        case TYPE_DYNAMIC_ARRAY:
        case TYPE_SLICE:
        case TYPE_STRING:
            fn->body = NULL;
            error_node(EK_ERROR, (Node *) fn, "Cannot define iterator for type %s", type_to_cstr(*receiver));
            if (receiver->distinct) {
                afprintf(
                    stderr,
                    ANSI_COLOR_YELLOW | ANSI_BOLD,
                    "    The type %s is a distinct alias to %s\n\n",
                    type_to_cstr(*receiver),
                    type_to_cstr(type_without_distinct(*receiver)));
            }
            exit(c, 1);
            break;

        default:
            break;
        }
    }

    if (type_is_scalar(type_without_ref(*receiver))) {
        fn->body = NULL;
        error_node(
            EK_ERROR,
            (Node *) fn,
            "Cannot define %s for scalar type %s",
            oms == OMS_RANGE ? "iterator" : "operator overload",
            type_to_cstr(*receiver));
        if (receiver->distinct) {
            afprintf(
                stderr,
                ANSI_COLOR_YELLOW | ANSI_BOLD,
                "    The type %s is a distinct alias to %s\n\n",
                type_to_cstr(*receiver),
                type_to_cstr(type_without_distinct(*receiver)));
        }
        exit(c, 1);
    }

    if (fn_spec->args_count < args_count) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE, fn->args_end_token, "Expected at least %zu arguments, got %zu", args_count, fn_spec->args_count);
        exit(c, 1);
    }

    for (size_t i = 0; i < fn_spec->args_count; i++) {
        const Type_Fn_Arg *it = &fn_spec->args[i];
        if (!it->has_default_value && i >= args_count) {
            error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
            error_parts(
                EK_NOTE,
                fn_spec->args[i].name,
                fn_spec->args[i].pos,
                "All arguments after the %zu%s argument must have a default value",
                (size_t) args_count,
                order_postfix(args_count));
            exit(c, 1);
        }
    }
}

void check_signature_of_arithmetic_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_ARITH;
    check_operator_method_signature_args_count_and_that_receiver_is_not_scalar(c, fn, fn_spec, 2, oms);

    const Type lhs_type = fn_spec->args[0].type;
    if (lhs_type.ref) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[0].name,
            fn_spec->args[0].pos,
            "Operand cannot be a pointer. (Provided type is %s)",
            type_to_cstr(lhs_type));
        exit(c, 1);
    }

    const Type rhs_type = fn_spec->args[1].type;
    if (!type_eq(rhs_type, lhs_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[1].name,
            fn_spec->args[1].pos,
            "Operand types must be same: Expected %s, got %s",
            type_to_cstr(lhs_type),
            type_to_cstr(rhs_type));
        exit(c, 1);
    }

    if (!type_eq(*fn_spec->return_type, lhs_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE,
            fn->returns.head ? fn->returns.head->token : fn->body->token,
            "Operand types and return type must be same: Expected to return %s, got %s",
            type_to_cstr(lhs_type),
            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
        exit(c, 1);
    }
}

void check_signature_of_binary_comparison_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_CMP;
    check_operator_method_signature_args_count_and_that_receiver_is_not_scalar(c, fn, fn_spec, 2, oms);

    const Type lhs_type = fn_spec->args[0].type;
    if (lhs_type.ref) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[0].name,
            fn_spec->args[0].pos,
            "Operand cannot be a pointer. (Provided type is %s)",
            type_to_cstr(lhs_type));
        exit(c, 1);
    }

    const Type rhs_type = fn_spec->args[1].type;
    if (!type_eq(rhs_type, lhs_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[1].name,
            fn_spec->args[1].pos,
            "Operand types must be same: Expected %s, got %s",
            type_to_cstr(lhs_type),
            type_to_cstr(rhs_type));
        exit(c, 1);
    }

    if (!type_eq(*fn_spec->return_type, c->equivalence_type) && !type_eq(*fn_spec->return_type, c->ordering_type)) //
    {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE,
            fn->returns.head ? fn->returns.head->token : fn->body->token,
            "Expected to return %s or %s, got %s",
            type_to_cstr(c->equivalence_type),
            type_to_cstr(c->ordering_type),
            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
        exit(c, 1);
    }
}

void check_signature_of_index_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_INDEX;
    check_operator_method_signature_args_count_and_that_receiver_is_not_scalar(c, fn, fn_spec, 3, oms);

    const Type assign_type = fn_spec->args[2].type;
    if (!type_eq(assign_type, (Type) {.kind = TYPE_BOOL})) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[2].name,
            fn_spec->args[2].pos,
            "Expected the third argument to be %s, got %s",
            type_to_cstr((Type) {.kind = TYPE_BOOL}),
            type_to_cstr(assign_type));
        exit(c, 1);
    }

    if (!type_is_pointer(*fn_spec->return_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE,
            fn->returns.head ? fn->returns.head->token : fn->body->token,
            "Expected to return a pointer, got %s",
            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
        exit(c, 1);
    }
}

void check_signature_of_slice_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_SLICE;
    check_operator_method_signature_args_count_and_that_receiver_is_not_scalar(c, fn, fn_spec, 3, oms);

    const Type begin_type = fn_spec->args[1].type;
    const Type end_type = fn_spec->args[2].type;
    if (!type_eq(end_type, begin_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[2].name,
            fn_spec->args[2].pos,
            "Types of slice beginning and end must be same: Expected %s, got %s",
            type_to_cstr(begin_type),
            type_to_cstr(end_type));
        exit(c, 1);
    }

    if (fn_spec->returns_count != 1) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        if (fn->returns.head) {
            error_node_range(
                EK_NOTE,
                fn->returns.head,
                fn->returns.tail,
                "The slice operator cannot return %zu values",
                fn_spec->returns_count);
        } else {
            error_token(
                EK_NOTE, fn->body->token, "The slice operator cannot return %zu values", fn_spec->returns_count);
        }
        exit(c, 1);
    }
}

void check_signature_of_range_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_RANGE;
    check_operator_method_signature_args_count_and_that_receiver_is_not_scalar(c, fn, fn_spec, 2, oms);

    const Type state_type = fn_spec->args[1].type;
    if (state_type.is_meta || !state_type.ref) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[1].name,
            fn_spec->args[1].pos,
            "Expected the state argument to be a typed pointer, got %s",
            type_to_cstr(state_type));
        exit(c, 1);
    }

    if (fn_spec->returns_count < 2) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(EK_NOTE, fn->body->token, "The iterator must return atleast two values");
        exit(c, 1);
    }

    if (!type_eq(fn_spec->returns[fn_spec->returns_count - 1], (Type) {.kind = TYPE_BOOL})) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_node(
            EK_NOTE,
            fn->returns.tail,
            "Expected the last return value to be %s, got %s",
            type_to_cstr((Type) {.kind = TYPE_BOOL}),
            type_to_cstr(fn_spec->returns[fn_spec->returns_count - 1]));
        exit(c, 1);
    }
}

static void show_explanation_about_custom_formatter(const Node_Fn *fn, Type receiver) {
    error_token(
        EK_ERROR,
        fn->defined_as->node.token,
        "The method '" SV_Fmt "' is special because it implements a custom formatter",
        SV_Arg(fn->defined_as->node.token.sv));

    afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "    It should have this signature:\n\n");

    receiver.ref = (receiver.distinct ? receiver.distinct->node.type.ref : 0) + 1;
    afprintf(
        stderr,
        ANSI_COLOR_MAGENTA | ANSI_BOLD,
        "        format :: (this: %s, w: Writer, nested: bool) {}\n\n",
        type_to_cstr_raw(receiver));
}

static void show_explanation_about_the_not_formatter_directive(const Node_Fn *fn) {
    if (fn->body) {
        assert(fn->body->kind == NODE_BLOCK);
        error_token_begin(EK_NOTE, ((Node_Block *) fn->body)->end);
    } else if (fn->returns.tail) {
        error_node_begin(EK_NOTE, fn->returns.tail);
    } else {
        error_token_begin(EK_NOTE, fn->args_end_token);
    }

    fprintf(
        stderr,
        "If this method is not meant to be a formatter, then add the %s directive after this",
        token_kind_to_cstr(TOKEN_DIRECTIVE_NOT_FORMATTER));
    error_finalize();
}

void check_signature_of_custom_formatter(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    Type receiver = fn_spec->args[0].type;
    if (receiver.distinct) {
        receiver.ref -= receiver.distinct->node.type.ref;
    }

    if (receiver.ref != 1) {
        goto error;
    }

    if (fn_spec->args_count != 3) {
        goto error;
    }

    assert(type_kind_eq(c->type_info_type, TYPE_STRUCT));
    assert(type_kind_eq(c->type_info_type.spec.structt->fields[4].type, TYPE_FN));
    const Type_Fn *expected_spec = c->type_info_type.spec.structt->fields[4].type.spec.fn;

    assert(fn_spec->args_count == expected_spec->args_count);
    for (size_t i = 1; i < fn_spec->args_count; i++) {
        if (!type_eq(fn_spec->args[i].type, expected_spec->args[i].type)) {
            goto error;
        }
    }

    if (!type_eq(*fn_spec->return_type, *expected_spec->return_type)) {
        goto error;
    }

    if (!c->type_info_cache.hasheq) {
        c->type_info_cache.hasheq = ht_hasheq_type;
    }

    receiver.ref--;
    ht_set(&c->type_info_cache, receiver, (Type_Info) {.format = fn});
    return;

error:
    show_explanation_about_custom_formatter(fn, receiver);
    show_explanation_about_the_not_formatter_directive(fn);
    exit(c, 1);
}

void define_orderless_methods(Compiler *c) {
    for (size_t i = 0; i < c->methods_to_check.count; i++) {
        Node_Fn *fn = c->methods_to_check.data[i];
        assert(fn->args.head && fn->args.head->kind == NODE_DEFINE); // Guaranteed by the parser

        // Define the polymorphic parameters
        check_fn(c, fn, REF_NONE, NULL, true, true);

        Node_Define *define = (Node_Define *) fn->args.head;
        assert(define->name->kind == NODE_ATOM && define->type); // Guaranteed by the parser

        if (!fn->defined_as) {
            error_node(EK_ERROR, (Node *) fn, "Anonymous function cannot be a method");
            error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
            exit(c, 1);
        }

        const SV name = fn->defined_as->node.token.sv;
        if (fn->is_not_formatter && !sv_eq(name, SV_Lit("format"))) {
            error_token(
                EK_ERROR,
                fn->not_formatter_token,
                "The %s directive can only be applied to a method named 'format'",
                token_kind_to_cstr(fn->not_formatter_token.kind));
            exit(c, 1);
        }

        check_expr(c, define->type, REF_NONE);
        type_assert_type(c, define->type);
        define->type->type.is_meta = false;
        const Type receiver_type = define->type->type;

        if (fn->reference_directives.head && !receiver_type.ref) {
            error_token(
                EK_ERROR,
                fn->reference_directives.head->token,
                "This iterator overload has reference semantics, yet the receiver is not a typed pointer");

            assert(fn->args.head);
            error_node(
                EK_NOTE,
                fn->args.head,
                "This argument is taken to be the receiver. Its type is %s",
                type_to_cstr(receiver_type));

            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "    This must be a pointer.\n\n");
            exit(c, 1);
        }

        Method_Spec     spec = {0};
        Method_Defining defining = {0};

        bool can_define = get_method_spec(c, define->type, receiver_type, name, &spec, &defining);
        if (can_define) {
            if (!defining.is_named) {
                error_node(EK_ERROR, define->type, "The receiver of a method cannot have an anonymous type");
                error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                exit(c, 1);
            }

            if (defining.block != fn->outer_block || defining.module != fn->node.module) {
                can_define = false;
            }
        }

        if (!can_define) {
            error_node(
                EK_ERROR,
                define->type,
                "Can only define methods on types that are defined in the same %s",
                defining.module == fn->node.module ? "block" : "module");

            error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
            if (defining.defined_as) {
                error_node(EK_NOTE, (Node *) defining.defined_as, "Here is the definition of the receiver type");
            }
            exit(c, 1);
        }

        if (type_kind_eq(receiver_type, TYPE_ENUM)) {
            ll_foreach(it, &receiver_type.spec.enumm.definition->values) {
                if (sv_eq(it->token.sv, name)) {
                    error_redefinition(c, (Node *) fn->defined_as, &it->token.pos);
                }
            }
        } else if (type_kind_eq(receiver_type, TYPE_TRAIT)) {
            Type_Trait *trait = receiver_type.spec.trait;
            for (size_t i = 0; i < trait->methods_count; i++) {
                Type_Trait_Method *it = &trait->methods[i];
                if (sv_eq(it->name, name)) {
                    it->fallback = fn;
                    assert(type_kind_eq(it->signature->node.type, TYPE_FN));
                    fn->default_trait_method = it;
                    break;
                }
            }

            if (!fn->default_trait_method) {
                error_undefined_in(c, &fn->defined_as->node.token, &receiver_type, "method");
            }

            if (receiver_type.ref) {
                error_node(EK_ERROR, define->type, "The receiver of a default trait method cannot be a pointer");
                error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                exit(c, 1);
            }

            if (fn->polymorphs.head) {
                error_node(
                    EK_ERROR,
                    (Node *) fn->polymorphs.head,
                    "A default trait method cannot have polymorphic parameters");
                exit(c, 1);
            }
        } else if (type_kind_eq(receiver_type, TYPE_STRUCT)) {
            for (size_t i = 0; i < receiver_type.spec.structt->fields_count; i++) {
                const Type_Struct_Field it = receiver_type.spec.structt->fields[i];
                if (sv_eq(it.name, name)) {
                    error_redefinition(c, (Node *) fn->defined_as, &it.pos);
                }
            }
        }

        Node_Fn **previous = ht_get(&c->methods_table, spec);
        if (previous) {
            error_redefinition(c, (Node *) fn->defined_as, &(*previous)->defined_as->node.token.pos);
        }

        if (sv_eq(name, SV_Lit("format")) && !fn->is_not_formatter) {
            ll_foreach(it, &fn->polymorphs) {
                if (it->arg_index) {
                    show_explanation_about_custom_formatter(fn, receiver_type);
                    error_node(EK_NOTE, (Node *) it, "Cannot have polymorphic parameters after the first argument");
                    show_explanation_about_the_not_formatter_directive(fn);
                    exit(c, 1);
                }
            }
        }

        ht_set(&c->methods_table, spec, fn);
    }

    c->methods_to_check.count = 0;
}
