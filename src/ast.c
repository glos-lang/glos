#include "ast.h"
#include "basic.h"

void ast_nodes_push(AST_Nodes *ns, AST_Node *n) {
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

static_assert(COUNT_AST_TYPES == 14, "");
static const char *ast_type_to_cstr_impl(AST_Type type) {
    assert(!type.is_type);

    const char *s = temp_alloc(0);
    for (size_t i = 0; i < type.ref; i++) {
        temp_sprintf("&");
        temp_remove_null();
    }

    switch (type.kind) {
    case AST_TYPE_UNIT:
        temp_sprintf("void");
        break;

    case AST_TYPE_BOOL:
        temp_sprintf("bool");
        break;

    case AST_TYPE_I8:
        temp_sprintf("i8");
        break;

    case AST_TYPE_I16:
        temp_sprintf("i16");
        break;

    case AST_TYPE_I32:
        temp_sprintf("i32");
        break;

    case AST_TYPE_I64:
        temp_sprintf("i64");
        break;

    case AST_TYPE_U8:
        temp_sprintf("u8");
        break;

    case AST_TYPE_U16:
        temp_sprintf("u16");
        break;

    case AST_TYPE_U32:
        temp_sprintf("u32");
        break;

    case AST_TYPE_U64:
        temp_sprintf("u64");
        break;

    case AST_TYPE_INT:
        temp_sprintf("i64");
        break;

    case AST_TYPE_RAWPTR:
        temp_sprintf("rawptr");
        break;

    case AST_TYPE_FN:
        temp_sprintf("(");

        for (size_t i = 0; i < type.spec.fn.args_count; i++) {
            AST_Node_Atom *it = type.spec.fn.args[i];
            if (i) {
                temp_remove_null();
                temp_sprintf(", ");
            }

            temp_remove_null();
            temp_sprintf(SV_Fmt ": ", SV_Arg(it->node.token.sv));

            temp_remove_null();
            ast_type_to_cstr_impl(it->node.type);
        }

        temp_remove_null();
        temp_sprintf(")");

        if (type.spec.fn.returnn->kind != AST_TYPE_UNIT) {
            temp_remove_null();
            temp_sprintf(" -> ");

            temp_remove_null();
            ast_type_to_cstr_impl(*type.spec.fn.returnn);
        }
        break;

    case AST_TYPE_STRUCT: {
        assert(type.spec.structt.definition);
        const AST_Node_Atom *defined_as = type.spec.structt.definition->defined_as;
        if (defined_as) {
            temp_sv_to_cstr(defined_as->node.token.sv);
        } else {
            temp_sprintf("struct {");

            for (size_t i = 0; i < type.spec.structt.fields_count; i++) {
                AST_Node_Atom *it = type.spec.structt.fields[i];
                if (i) {
                    temp_remove_null();
                    temp_sprintf(", ");
                }

                temp_remove_null();
                temp_sprintf(SV_Fmt ": ", SV_Arg(it->node.token.sv));

                temp_remove_null();
                ast_type_to_cstr_impl(it->node.type);
            }

            temp_remove_null();
            temp_sprintf("}");
        }
    } break;

    default:
        unreachable();
    }

    return s;
}

const char *ast_type_to_cstr(AST_Type type) {
    if (type.is_type) {
        return temp_sprintf("a type");
    }

    const char *s = temp_sprintf("'");
    temp_remove_null();
    ast_type_to_cstr_impl(type);
    temp_remove_null();
    temp_sprintf("'");
    return s;
}

static_assert(COUNT_AST_TYPES == 14, "");
bool ast_type_eq(AST_Type a, AST_Type b) {
    if (a.kind != b.kind || a.ref != b.ref) {
        return false;
    }

    if (a.is_type) {
        return b.is_type;
    }

    if (b.is_type) {
        return a.is_type;
    }

    switch (a.kind) {
    case AST_TYPE_FN: {
        if (a.spec.fn.args_count != b.spec.fn.args_count) {
            return false;
        }

        if (a.spec.fn.args == b.spec.fn.args && a.spec.fn.returnn == b.spec.fn.returnn) {
            return true;
        }

        for (size_t i = 0; i < a.spec.fn.args_count; i++) {
            if (!ast_type_eq(a.spec.fn.args[i]->node.type, b.spec.fn.args[i]->node.type)) {
                return false;
            }
        }

        return ast_type_eq(*a.spec.fn.returnn, *b.spec.fn.returnn);
    }

    case AST_TYPE_STRUCT: {
        if (a.spec.structt.fields_count != b.spec.structt.fields_count) {
            return false;
        }

        if (a.spec.structt.fields == b.spec.structt.fields) {
            return true;
        }

        for (size_t i = 0; i < a.spec.structt.fields_count; i++) {
            if (!ast_type_eq(a.spec.structt.fields[i]->node.type, b.spec.structt.fields[i]->node.type)) {
                return false;
            }
        }

        return true;
    }

    default:
        return true;
    }
}

static_assert(COUNT_AST_TYPES == 14, "");
bool ast_type_kind_eq(AST_Type type, AST_Type_Kind kind) {
    if (type.is_type) {
        return false;
    }

    return type.kind == kind;
}

bool ast_type_is_numeric(AST_Type type) {
    return ast_type_is_integer(type);
}

static_assert(COUNT_AST_TYPES == 14, "");
bool ast_type_is_integer(AST_Type type) {
    if (type.ref || type.is_type) {
        return false;
    }

    switch (type.kind) {
    case AST_TYPE_I8:
    case AST_TYPE_I16:
    case AST_TYPE_I32:
    case AST_TYPE_I64:

    case AST_TYPE_U8:
    case AST_TYPE_U16:
    case AST_TYPE_U32:
    case AST_TYPE_U64:

    case AST_TYPE_INT:
        return true;

    default:
        return false;
    }
}

bool ast_type_is_pointer(AST_Type type) {
    if (type.is_type) {
        return false;
    }
    return type.ref != 0 || type.kind == AST_TYPE_RAWPTR;
}

bool ast_type_is_scalar(AST_Type type) {
    if (type.is_type) {
        return false;
    }

    if (ast_type_is_numeric(type) || ast_type_is_pointer(type)) {
        return true;
    }

    if (type.kind == AST_TYPE_BOOL || type.kind == AST_TYPE_FN) {
        return true;
    }

    return false;
}

static_assert(COUNT_AST_TYPES == 14, "");
bool ast_type_is_signed(AST_Type type) {
    if (type.ref || type.is_type) {
        return false;
    }

    switch (type.kind) {
    case AST_TYPE_I8:
    case AST_TYPE_I16:
    case AST_TYPE_I32:
    case AST_TYPE_I64:
    case AST_TYPE_INT:
        return true;

    default:
        return false;
    }
}

#define Indent_Fmt    "%*s"
#define Indent_Arg(d) (d) * 4, ""

static void ast_node_debug_impl(FILE *f, AST_Node *n, int depth, const char *label);

static void ast_nodes_debug_impl(FILE *f, AST_Nodes ns, int depth, const char *label) {
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
    for (AST_Node *it = ns.head; it; it = it->next) {
        ast_node_debug_impl(f, it, child_depth, NULL);
    }

    if (label) {
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    }
}

static_assert(COUNT_AST_NODES == 16, "");
static void ast_node_debug_impl(FILE *f, AST_Node *n, int depth, const char *label) {
    if (!n) {
        return;
    }

    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    if (label) {
        fprintf(f, "%s = ", label);
    }

    switch (n->kind) {
    case AST_NODE_ATOM:
        fprintf(f, "Atom " SV_Fmt "\n", SV_Arg(n->token.sv));
        break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        fprintf(f, "Unary '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        ast_node_debug_impl(f, unary->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        fprintf(f, "Binary '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        ast_node_debug_impl(f, binary->lhs, depth + 1, "Lhs");
        ast_node_debug_impl(f, binary->rhs, depth + 1, "Rhs");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_MEMBER: {
        AST_Node_Member *member = (AST_Node_Member *) n;
        fprintf(f, "Member '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        ast_node_debug_impl(f, member->lhs, depth + 1, "Lhs");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_FN: {
        AST_Node_Fn *fn = (AST_Node_Fn *) n;
        fprintf(f, "Function {\n");
        ast_nodes_debug_impl(f, fn->args, depth + 1, "Args");
        ast_node_debug_impl(f, fn->returnn, depth + 1, "Return");
        ast_node_debug_impl(f, fn->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_STRUCT: {
        AST_Node_Struct *structt = (AST_Node_Struct *) n;
        fprintf(f, "Structure {\n");
        ast_nodes_debug_impl(f, structt->fields, depth + 1, "Fields");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_COMPOUND: {
        AST_Node_Compound *compound = (AST_Node_Compound *) n;
        fprintf(f, "Compound {\n");
        ast_node_debug_impl(f, compound->lhs, depth + 1, "Lhs");
        ast_nodes_debug_impl(f, compound->children, depth + 1, "Children");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_CALL: {
        AST_Node_Call *call = (AST_Node_Call *) n;
        fprintf(f, "Call {\n");
        ast_node_debug_impl(f, call->fn, depth + 1, "Fn");
        ast_nodes_debug_impl(f, call->args, depth + 1, "Args");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        fprintf(f, "Define %s {\n", define->is_const ? "constant" : "variable");
        ast_node_debug_impl(f, define->name, depth + 1, "Name");
        ast_node_debug_impl(f, define->type, depth + 1, "Type");
        ast_node_debug_impl(f, define->expr, depth + 1, "Expr");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;
        fprintf(f, "Block {\n");
        for (AST_Node *it = block->body.head; it; it = it->next) {
            ast_node_debug_impl(f, it, depth + 1, NULL);
        }
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_IF: {
        AST_Node_If *iff = (AST_Node_If *) n;
        fprintf(f, "If {\n");
        ast_node_debug_impl(f, iff->condition, depth + 1, "Condition");
        ast_node_debug_impl(f, iff->consequence, depth + 1, "Consequence");
        ast_node_debug_impl(f, iff->antecedence, depth + 1, "Antecedence");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_FOR: {
        AST_Node_For *forr = (AST_Node_For *) n;
        fprintf(f, "For {\n");
        ast_node_debug_impl(f, forr->init, depth + 1, "Init");
        ast_node_debug_impl(f, forr->condition, depth + 1, "Condition");
        ast_node_debug_impl(f, forr->update, depth + 1, "Update");
        ast_node_debug_impl(f, forr->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_JUMP:
        if (n->token.kind == TOKEN_BREAK) {
            fprintf(f, "Break\n");
        } else if (n->token.kind == TOKEN_CONTINUE) {
            fprintf(f, "Continue\n");
        } else {
            unreachable();
        }
        break;

    case AST_NODE_RETURN: {
        AST_Node_Return *returnn = (AST_Node_Return *) n;
        fprintf(f, "Return {\n");
        ast_node_debug_impl(f, returnn->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case AST_NODE_EXTERN: {
        AST_Node_Extern *externn = (AST_Node_Extern *) n;
        if (externn->nodes.head) {
            fprintf(f, "Extern {\n");
            for (AST_Node *it = externn->nodes.head; it; it = it->next) {
                ast_node_debug_impl(f, it, depth + 1, NULL);
            }
            fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
        } else {
            fprintf(f, "Extern {}\n");
        }
    } break;

    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        fprintf(f, "Print {\n");
        ast_node_debug_impl(f, print->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    default:
        unreachable();
    }
}

void ast_node_debug(FILE *f, AST_Node *n) {
    ast_node_debug_impl(f, n, 0, NULL);
}

void ast_nodes_debug(FILE *f, AST_Nodes ns) {
    ast_nodes_debug_impl(f, ns, 0, NULL);
}
