#include "node.h"

static_assert(COUNT_TYPES == 19, "");
const char *type_to_cstr(Type type) {
    const char *s = temp_alloc(0);

    for (size_t i = 0; i < type.ref; i++) {
        temp_sprintf("&");
        temp_remove_null();
    }

    switch (type.kind) {
    case TYPE_UNIT:
        temp_sprintf("()");
        break;

    case TYPE_BOOL:
        temp_sprintf("bool");
        break;

    case TYPE_I8:
        temp_sprintf("i8");
        break;

    case TYPE_I16:
        temp_sprintf("i16");
        break;

    case TYPE_I32:
        temp_sprintf("i32");
        break;

    case TYPE_I64:
    case TYPE_INT:
        temp_sprintf("i64");
        break;

    case TYPE_U8:
        temp_sprintf("u8");
        break;

    case TYPE_U16:
        temp_sprintf("u16");
        break;

    case TYPE_U32:
        temp_sprintf("u32");
        break;

    case TYPE_U64:
        temp_sprintf("u64");
        break;

    case TYPE_RAWPTR:
        temp_sprintf("rawptr");
        break;

    case TYPE_FN: {
        const NodeFn *spec = (const NodeFn *) type.spec_node;

        temp_sprintf("fn");
        if (spec->generics.head) {
            temp_remove_null();
            temp_sprintf("<");

            for (Node *it = spec->generics.head; it; it = it->next) {
                temp_remove_null();
                type_to_cstr(it->type);

                if (it->next) {
                    temp_remove_null();
                    temp_sprintf(", ");
                }
            }

            temp_remove_null();
            temp_sprintf(">");
        }

        temp_remove_null();
        temp_sprintf("(");

        for (const Node *it = spec->args.head; it; it = it->next) {
            temp_remove_null();
            type_to_cstr(it->type);

            if (it->next) {
                temp_remove_null();
                temp_sprintf(", ");
            }
        }

        temp_remove_null();
        temp_sprintf(")");

        if (spec->ret) {
            temp_remove_null();
            temp_sprintf(" ");
            temp_remove_null();
            type_to_cstr(spec->ret->type);
        }
    } break;

    case TYPE_ARRAY:
        temp_sprintf("[%zu]", type.spec_count);
        temp_remove_null();
        type_to_cstr(*type.spec_type);
        break;

    case TYPE_SLICE:
        temp_sprintf("[]");
        temp_remove_null();
        type_to_cstr(*type.spec_type);
        break;

    case TYPE_DSLICE:
        temp_sprintf("[..]");
        temp_remove_null();
        type_to_cstr(*type.spec_type);
        break;

    case TYPE_TRAIT:
        temp_sv_to_cstr(type.spec_node->token.sv);
        break;

    case TYPE_STRUCT:
        temp_sv_to_cstr(type.spec_node->token.sv);
        if (type.spec_struct_instance) {
            temp_remove_null();
            temp_sprintf("<");

            for (Node *it = type.spec_struct_instance->generics; it; it = it->next) {
                temp_remove_null();
                type_to_cstr(it->type);

                if (it->next) {
                    temp_remove_null();
                    temp_sprintf(", ");
                }
            }

            temp_remove_null();
            temp_sprintf(">");
        }
        break;

    case TYPE_GENERIC:
        temp_sv_to_cstr(type.spec_node->token.sv);
        break;

    default:
        unreachable();
    }

    return s;
}

typedef enum {
    TYPE_MATCH_STRICT,
    TYPE_MATCH_BESTFIT_UNI,
    TYPE_MATCH_BESTFIT_BI,
} TypeMatchLevel;

static_assert(COUNT_TYPES == 19, "");
static bool type_matches(Type a, Type b, TypeMatchLevel level) {
    if (level != TYPE_MATCH_STRICT && a.kind == TYPE_GENERIC) {
        return b.ref >= a.ref;
    }

    if (level == TYPE_MATCH_BESTFIT_BI && b.kind == TYPE_GENERIC) {
        return a.ref >= b.ref;
    }

    if (a.kind != b.kind || a.ref != b.ref) {
        return false;
    }

    switch (a.kind) {
    case TYPE_FN: {
        const NodeFn *a_spec = (const NodeFn *) a.spec_node;
        const NodeFn *b_spec = (const NodeFn *) b.spec_node;

        if (a_spec->arity != b_spec->arity) {
            return false;
        }

        for (const Node *a = a_spec->args.head, *b = b_spec->args.head; a; a = a->next, b = b->next) {
            if (!type_matches(a->type, b->type, level)) {
                return false;
            }
        }

        return type_matches(node_fn_return_type(a_spec), node_fn_return_type(b_spec), level);
    } break;

    case TYPE_SLICE:
    case TYPE_DSLICE:
        assert(a.spec_type);
        assert(b.spec_type);
        return type_matches(*a.spec_type, *b.spec_type, level);

    case TYPE_ARRAY:
        assert(a.spec_type);
        assert(b.spec_type);
        return type_matches(*a.spec_type, *b.spec_type, level) && a.spec_count == b.spec_count;

    case TYPE_TRAIT:
        return a.spec_node == b.spec_node;

    case TYPE_STRUCT:
        if (a.spec_struct_instance) {
            if (!b.spec_struct_instance) {
                return false;
            }

            const StructInstanace *a_spec = a.spec_struct_instance;
            const StructInstanace *b_spec = b.spec_struct_instance;
            if (a_spec->definition != b_spec->definition) {
                return false;
            }

            Node *a_it = a_spec->generics;
            Node *b_it = b_spec->generics;
            while (a_it && b_it) {
                assert(a_it);
                assert(b_it);
                if (!type_matches(a_it->type, b_it->type, level)) {
                    return false;
                }

                a_it = a_it->next;
                b_it = b_it->next;
            }

            return true;
        }

        return a.spec_node == b.spec_node;

    case TYPE_GENERIC:
        return a.spec_node == b.spec_node;

    default:
        return true;
    }
}

static_assert(COUNT_TYPES == 19, "");
bool type_eq(Type a, Type b) {
    return type_matches(a, b, TYPE_MATCH_STRICT);
}

static_assert(COUNT_TYPES == 19, "");
bool type_is_signed(Type type) {
    if (type.ref != 0) {
        return false;
    }

    switch (type.kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_INT:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_TYPES == 19, "");
bool type_is_integer(Type type) {
    if (type.ref) {
        return false;
    }

    switch (type.kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:

    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:

    case TYPE_INT:
        return true;

    default:
        return false;
    }
}

bool type_is_pointer(Type type) {
    return type.ref != 0 || type.kind == TYPE_RAWPTR;
}

Type type_remove_ref(Type type) {
    type.ref = 0;
    return type;
}

void instantiations_push(Instantiations *is, Instantiation *i) {
    if (!i) {
        return;
    }

    if (is->tail) {
        is->tail->next = i;
    } else {
        is->head = i;
    }

    is->tail = i;
}

Instantiation *instantiations_find(Instantiations is, Type *types, size_t count) {
    for (Instantiation *it = is.head; it; it = it->next) {
        if (it->count == count) {
            bool ok = true;
            for (size_t i = 0; i < count; i++) {
                if (!type_eq(types[i], it->types[i])) {
                    ok = false;
                    break;
                }
            }

            if (ok) {
                return it;
            }
        }
    }

    return NULL;
}

Instantiation *instantiations_get(Instantiations *instantiations, Node *generics, size_t generics_count, Arena *a) {
    Type *types = temp_alloc(generics_count * sizeof(Type));
    {
        Node *generic = generics;
        for (size_t i = 0; i < generics_count; i++) {
            assert(generic);

            Type type = generic->type;
            while (type.kind == TYPE_GENERIC && type.spec_node->type.spec_type) {
                type = *type.spec_node->type.spec_type;
            }
            types[i] = type;

            generic = generic->next;
        }
    }

    Instantiation *instantiation = instantiations_find(*instantiations, types, generics_count);
    if (instantiation) {
        temp_reset(types);
        return instantiation;
    }

    instantiation = arena_alloc(a, sizeof(Instantiation));
    instantiation->count = generics_count;
    instantiation->types = arena_clone(a, types, generics_count * sizeof(Type));

    instantiations_push(instantiations, instantiation);
    temp_reset(types);

    return instantiation;
}

void nodes_push(Nodes *ns, Node *n) {
    if (!n) {
        return;
    }

    if (ns->tail) {
        ns->tail->next = n;
    } else {
        ns->head = n;
    }

    ns->tail = n;
}

Type node_fn_return_type(const NodeFn *fn) {
    if (fn->ret) {
        return fn->ret->type;
    }

    return (Type) {.kind = TYPE_UNIT};
}

struct TypeMethods {
    Type type;

    NodeFn *head;
    NodeFn *tail;

    TypeMethods *next;
};

static TypeMethods *type_methods_find(Methods *list, Type type, TypeMatchLevel level) {
    type = type_remove_ref(type);
    for (TypeMethods *it = list->head; it; it = it->next) {
        if (type_matches(type_remove_ref(it->type), type, level)) {
            return it;
        }
    }
    return NULL;
}

NodeFn *methods_find(Methods *list, Type type, SV name) {
    TypeMethods *methods = type_methods_find(list, type, TYPE_MATCH_BESTFIT_UNI);
    if (!methods) {
        return NULL;
    }

    for (NodeFn *it = methods->head; it; it = it->next_method) {
        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

NodeFn *methods_push(Methods *list, Type type, NodeFn *fn, Arena *a) {
    if (!fn) {
        return NULL;
    }

    TypeMethods *methods = type_methods_find(list, type, TYPE_MATCH_BESTFIT_BI);
    if (methods) {
        for (NodeFn *it = methods->head; it; it = it->next_method) {
            if (sv_eq(it->node.token.sv, fn->node.token.sv)) {
                return it;
            }
        }
    }

    if (!methods || !type_matches(type_remove_ref(methods->type), type_remove_ref(type), TYPE_MATCH_BESTFIT_UNI)) {
        methods = type_methods_find(list, type, TYPE_MATCH_BESTFIT_UNI);
    }

    if (!methods) {
        methods = arena_alloc(a, sizeof(TypeMethods));
        methods->type = type;

        if (list->tail) {
            list->tail->next = methods;
        } else {
            list->head = methods;
        }

        list->tail = methods;
    }

    if (methods->tail) {
        methods->tail->next_method = fn;
    } else {
        methods->head = fn;
    }

    methods->tail = fn;
    return NULL;
}
