#include "node.h"
#include "basic.h"

#include "thirdparty/stb_ds.h"

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

void modules_free(Modules *m) {
    shfree(m->table);
}

static_assert(COUNT_TYPES == 18, "");
const char *type_to_cstr_raw(Type type) {
    assert(!type.is_meta);

    const char *s = temp_alloc(0);
    for (size_t i = 0; i < type.ref; i++) {
        temp_sprintf("&");
        temp_remove_null();
    }

    switch (type.kind) {
    case TYPE_UNIT:
        temp_sprintf("void");
        break;

    case TYPE_BOOL:
        temp_sprintf("bool");
        break;

    case TYPE_CHAR:
        temp_sprintf("char");
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

    case TYPE_INT:
        temp_sprintf("i64");
        break;

    case TYPE_RAWPTR:
        temp_sprintf("rawptr");
        break;

    case TYPE_FN:
        temp_sprintf("(");

        for (size_t i = 0; i < type.spec.fn.args_count; i++) {
            Type_Fn_Arg it = type.spec.fn.args[i];
            if (i) {
                temp_remove_null();
                temp_sprintf(", ");
            }

            temp_remove_null();
            temp_sprintf(SV_Fmt ": ", SV_Arg(it.name));

            temp_remove_null();
            type_to_cstr_raw(it.type);
        }

        if (type.spec.fn.is_variadic) {
            temp_remove_null();
            temp_sprintf(", ...");
        }

        temp_remove_null();
        temp_sprintf(")");

        if (type.spec.fn.returnn->kind != TYPE_UNIT) {
            temp_remove_null();
            temp_sprintf(" -> ");

            temp_remove_null();
            type_to_cstr_raw(*type.spec.fn.returnn);
        }
        break;

    case TYPE_STRUCT: {
        assert(type.spec.structt);
        assert(type.spec.structt->definition);
        const Node_Atom *defined_as = type.spec.structt->definition->defined_as;
        if (defined_as) {
            temp_sv_to_cstr(defined_as->node.token.sv);
        } else {
            temp_sprintf("struct {");

            for (size_t i = 0; i < type.spec.structt->fields_count; i++) {
                Type_Struct_Field it = type.spec.structt->fields[i];
                if (i) {
                    temp_remove_null();
                    temp_sprintf(", ");
                }

                temp_remove_null();
                temp_sprintf(SV_Fmt ": ", SV_Arg(it.name));

                temp_remove_null();
                type_to_cstr_raw(it.type);
            }

            temp_remove_null();
            temp_sprintf("}");
        }
    } break;

    case TYPE_SLICE:
        temp_sprintf("[]");
        temp_remove_null();
        type_to_cstr_raw(*type.spec.slice.element);
        break;

    case TYPE_STRING:
        temp_sprintf("string");
        break;

    case TYPE_MODULE:
        unreachable();

    default:
        unreachable();
    }

    return s;
}

const char *type_to_cstr(Type type) {
    if (type.is_meta) {
        return temp_sprintf("a type");
    }

    if (type.kind == TYPE_MODULE) {
        return temp_sprintf("a module");
    }

    const char *s = temp_sprintf("'");
    temp_remove_null();
    type_to_cstr_raw(type);
    temp_remove_null();
    temp_sprintf("'");
    return s;
}

static bool type_struct_eq(Type_Struct *a, Type_Struct *b) {
    if (a->fields_count != b->fields_count) {
        return false;
    }

    if (a->fields == b->fields) {
        return true;
    }

    for (size_t i = 0; i < a->fields_count; i++) {
        if (!type_eq(a->fields[i].type, b->fields[i].type)) {
            return false;
        }
    }

    return true;
}

static_assert(COUNT_TYPES == 18, "");
bool type_eq(Type a, Type b) {
    if (a.kind != b.kind || a.ref != b.ref) {
        return false;
    }

    if (a.is_meta) {
        return b.is_meta;
    }

    if (b.is_meta) {
        return a.is_meta;
    }

    switch (a.kind) {
    case TYPE_FN: {
        if (a.spec.fn.args_count != b.spec.fn.args_count) {
            return false;
        }

        if (a.spec.fn.args == b.spec.fn.args && a.spec.fn.returnn == b.spec.fn.returnn) {
            return true;
        }

        for (size_t i = 0; i < a.spec.fn.args_count; i++) {
            if (!type_eq(a.spec.fn.args[i].type, b.spec.fn.args[i].type)) {
                return false;
            }
        }

        return type_eq(*a.spec.fn.returnn, *b.spec.fn.returnn);
    }

    case TYPE_STRUCT:
        return type_struct_eq(a.spec.structt, b.spec.structt);

    case TYPE_SLICE:
        return type_eq(*a.spec.slice.element, *b.spec.slice.element);

    default:
        return true;
    }
}

static_assert(COUNT_TYPES == 18, "");
bool type_kind_eq(Type type, Type_Kind kind) {
    if (type.is_meta) {
        return false;
    }

    return type.kind == kind;
}

bool type_is_numeric(Type type) {
    return type_is_integer(type);
}

static_assert(COUNT_TYPES == 18, "");
bool type_is_integer(Type type) {
    if (type.ref || type.is_meta) {
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
    if (type.is_meta) {
        return false;
    }
    return type.ref != 0 || type.kind == TYPE_RAWPTR;
}

bool type_is_scalar(Type type) {
    if (type.is_meta) {
        return false;
    }

    if (type_is_numeric(type) || type_is_pointer(type)) {
        return true;
    }

    if (type.kind == TYPE_BOOL || type.kind == TYPE_CHAR || type.kind == TYPE_FN) {
        return true;
    }

    return false;
}

static_assert(COUNT_TYPES == 18, "");
bool type_is_signed(Type type) {
    if (type.ref || type.is_meta) {
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

static_assert(COUNT_CONST_VALUES == 6, "");
bool const_value_eq(Const_Value a, Const_Value b) {
    if (a.kind != b.kind) {
        return false;
    }

    switch (a.kind) {
    case CONST_VALUE_INT:
        return a.as.integer == b.as.integer;

    case CONST_VALUE_FN:
        return a.as.fn == b.as.fn;

    case CONST_VALUE_TYPE:
        return type_eq(a.as.type, b.as.type);

    case CONST_VALUE_STRUCT: {
        if (!type_struct_eq(a.as.structt.spec, b.as.structt.spec)) {
            return false;
        }

        for (size_t i = 0; i < a.as.structt.spec->fields_count; i++) {
            if (!const_value_eq(a.as.structt.fields[i], b.as.structt.fields[i])) {
                return false;
            }
        }

        return true;
    }

    case CONST_VALUE_STRING:
        return sv_eq(a.as.string, b.as.string);

    case CONST_VALUE_MODULE:
        unreachable();

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 24, "");
Node *node_alloc(Arena *arena, Node_Kind kind, Token token) {
    static const size_t sizes[COUNT_NODES] = {
        [NODE_ATOM] = sizeof(Node_Atom), // This comment is here to prevent clang-format from messing this up
        [NODE_GHOST] = sizeof(Node_Ghost),
        [NODE_UNARY] = sizeof(Node_Unary),
        [NODE_BINARY] = sizeof(Node_Binary),
        [NODE_MEMBER] = sizeof(Node_Member),
        [NODE_ASSERT] = sizeof(Node_Assert),
        [NODE_IMPORT] = sizeof(Node_Import),

        [NODE_FN] = sizeof(Node_Fn),
        [NODE_STRUCT] = sizeof(Node_Struct),
        [NODE_COMPOUND] = sizeof(Node_Compound),

        [NODE_CALL] = sizeof(Node_Call),

        [NODE_SLICE] = sizeof(Node_Slice),
        [NODE_INDEX] = sizeof(Node_Index),

        [NODE_DEFINE] = sizeof(Node_Define),
        [NODE_BLOCK] = sizeof(Node_Block),
        [NODE_IF] = sizeof(Node_If),
        [NODE_FOR] = sizeof(Node_For),

        [NODE_CASE] = sizeof(Node_Case),
        [NODE_SWITCH] = sizeof(Node_Switch),

        [NODE_JUMP] = sizeof(Node_Jump),
        [NODE_DEFER] = sizeof(Node_Defer),
        [NODE_RETURN] = sizeof(Node_Return),

        [NODE_EXTERN] = sizeof(Node_Extern),

        [NODE_PRINT] = sizeof(Node_Print),
    };

    assert(kind >= NODE_ATOM && kind < COUNT_NODES);
    Node *node = arena_alloc(arena, sizes[kind]);
    node->kind = kind;
    node->token = token;
    return node;
}

#define Indent_Fmt    "%*s"
#define Indent_Arg(d) (d) * 4, ""

static void node_debug_impl(FILE *f, Node *n, int depth, const char *label);

static void nodes_debug_impl(FILE *f, Nodes ns, int depth, const char *label) {
    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    if (label) {
        fprintf(f, "%s = {", label);
        if (ns.head) {
            fprintf(f, "\n");
        } else {
            fprintf(f, "}\n");
            return;
        }
    }

    const size_t child_depth = depth + (label != NULL);
    for (Node *it = ns.head; it; it = it->next) {
        node_debug_impl(f, it, child_depth, NULL);
    }

    if (label) {
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    }
}

static_assert(COUNT_NODES == 24, "");
static void node_debug_impl(FILE *f, Node *n, int depth, const char *label) {
    if (!n) {
        return;
    }

    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    if (label) {
        fprintf(f, "%s = ", label);
    }

    switch (n->kind) {
    case NODE_ATOM:
        fprintf(f, "Atom " SV_Fmt "\n", SV_Arg(n->token.sv));
        break;

    case NODE_GHOST:
        fprintf(f, "Ghost\n");
        break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        fprintf(f, "Unary '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        node_debug_impl(f, unary->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        fprintf(f, "Binary '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        node_debug_impl(f, binary->lhs, depth + 1, "Lhs");
        node_debug_impl(f, binary->rhs, depth + 1, "Rhs");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        fprintf(f, "Member '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        node_debug_impl(f, member->lhs, depth + 1, "Lhs");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        if (assertt->is_compile_time) {
            fprintf(f, "#");
        }

        fprintf(f, "Assert {\n");
        node_debug_impl(f, assertt->expr, depth + 1, "Expr");
        node_debug_impl(f, assertt->message, depth + 1, "Message");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_IMPORT: {
        Node_Import *import = (Node_Import *) n;
        fprintf(f, "Import '%s'\n ", import->module->relative_path);
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;
        fprintf(f, "Function {\n");
        nodes_debug_impl(f, fn->args, depth + 1, "Args");
        node_debug_impl(f, fn->returnn, depth + 1, "Return");
        node_debug_impl(f, fn->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_STRUCT: {
        Node_Struct *structt = (Node_Struct *) n;
        fprintf(f, "Structure {\n");
        nodes_debug_impl(f, structt->fields, depth + 1, "Fields");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        fprintf(f, "Compound {\n");
        node_debug_impl(f, compound->lhs, depth + 1, "Lhs");
        nodes_debug_impl(f, compound->children, depth + 1, "Children");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        fprintf(f, "Call {\n");
        node_debug_impl(f, call->fn, depth + 1, "Fn");
        nodes_debug_impl(f, call->args, depth + 1, "Args");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_SLICE: {
        Node_Slice *slice = (Node_Slice *) n;
        fprintf(f, "Slice {\n");
        node_debug_impl(f, slice->element, depth + 1, "Element");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;
        fprintf(f, "Index {\n");
        node_debug_impl(f, index->lhs, depth + 1, "Lhs");
        if (index->is_ranged) {
            node_debug_impl(f, index->a, depth + 1, "Index");
        } else {
            node_debug_impl(f, index->a, depth + 1, "From");
            node_debug_impl(f, index->b, depth + 1, "To");
        }
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        fprintf(f, "Define %s {\n", define->is_const ? "constant" : "variable");
        node_debug_impl(f, define->name, depth + 1, "Name");
        node_debug_impl(f, define->type, depth + 1, "Type");
        node_debug_impl(f, define->expr, depth + 1, "Expr");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        fprintf(f, "Block {\n");
        for (Node *it = block->body.head; it; it = it->next) {
            node_debug_impl(f, it, depth + 1, NULL);
        }
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        fprintf(f, "If {\n");
        node_debug_impl(f, iff->condition, depth + 1, "Condition");
        node_debug_impl(f, iff->consequence, depth + 1, "Consequence");
        node_debug_impl(f, iff->antecedence, depth + 1, "Antecedence");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;
        fprintf(f, "For {\n");
        node_debug_impl(f, forr->init, depth + 1, "Init");
        node_debug_impl(f, forr->condition, depth + 1, "Condition");
        node_debug_impl(f, forr->update, depth + 1, "Update");
        node_debug_impl(f, forr->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        fprintf(f, "Case");
        if (!case_->preds.head) {
            fprintf(f, " (fallback)");
        }
        fprintf(f, " {\n");
        if (case_->preds.head) {
            nodes_debug_impl(f, case_->preds, depth + 1, "Predicates");
        }
        node_debug_impl(f, case_->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        fprintf(f, "Switch {\n");
        node_debug_impl(f, sw->expr, depth + 1, "Expr");
        nodes_debug_impl(f, sw->cases, depth + 1, "Cases");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_JUMP:
        if (n->token.kind == TOKEN_BREAK) {
            fprintf(f, "Break\n");
        } else if (n->token.kind == TOKEN_CONTINUE) {
            fprintf(f, "Continue\n");
        } else {
            unreachable();
        }
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        fprintf(f, "Defer {\n");
        node_debug_impl(f, defer->stmt, depth + 1, "Stmt");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_RETURN: {
        Node_Return *returnn = (Node_Return *) n;
        fprintf(f, "Return {\n");
        node_debug_impl(f, returnn->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        if (externn->nodes.head) {
            fprintf(f, "Extern {\n");
            for (Node *it = externn->nodes.head; it; it = it->next) {
                node_debug_impl(f, it, depth + 1, NULL);
            }
            fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
        } else {
            fprintf(f, "Extern {}\n");
        }
    } break;

    case NODE_PRINT: {
        Node_Print *print = (Node_Print *) n;
        fprintf(f, "Print {\n");
        node_debug_impl(f, print->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    default:
        unreachable();
    }
}

void node_debug(FILE *f, Node *n) {
    node_debug_impl(f, n, 0, NULL);
}

void nodes_debug(FILE *f, Nodes ns) {
    nodes_debug_impl(f, ns, 0, NULL);
}
