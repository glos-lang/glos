#include "formatter.h"
#include "node.h"

typedef struct {
    size_t depth;
    SB    *sb;
} Formatter;

static void format_indent(Formatter *f) {
    for (size_t i = 0; i < f->depth; i++) {
        sb_push(f->sb, '\t');
    }
}

static void format_expr(Formatter *f, Node *n);

static_assert(COUNT_NODES == 22, "");
static void format_type(Formatter *f, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;
        if (atom->scope.sv.data) {
            sb_sprintf(f->sb, SVFmt "::", SVArg(atom->scope.sv));
        }
        sb_sprintf(f->sb, SVFmt, SVArg(n->token.sv));
    } break;

    case NODE_UNARY: {
        sb_push(f->sb, '&');
        format_type(f, ((NodeUnary *) n)->operand);
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        sb_push(f->sb, '[');
        format_type(f, index->base);
        if (index->from) {
            sb_sprintf(f->sb, "; ");
            format_expr(f, index->from);
        }
        sb_push(f->sb, ']');
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        sb_sprintf(f->sb, "fn (");
        if (fn->fmt_multiline) {
            f->depth++;
        }

        for (Node *it = fn->args.head; it; it = it->next) {
            if (fn->fmt_multiline) {
                sb_push(f->sb, '\n');
                format_indent(f);
            }

            format_type(f, ((NodeVar *) it)->type);
            if (it->next) {
                sb_sprintf(f->sb, ", ");
            }
        }

        if (fn->fmt_multiline) {
            f->depth--;
            sb_push(f->sb, '\n');
            format_indent(f);
        }
        sb_push(f->sb, ')');

        if (fn->ret) {
            sb_push(f->sb, ' ');
            format_type(f, fn->ret);
        }
    } break;

    default:
        unreachable();
    }
}

static void format_expr_with_parens_maybe(Formatter *f, Node *n, Power mbp) {
    if (n->kind == NODE_BINARY && token_kind_to_power(n->token.kind) < mbp) {
        da_push(f->sb, '(');
        format_expr(f, n);
        da_push(f->sb, ')');
    } else {
        format_expr(f, n);
    }
}

static void format_expr(Formatter *f, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;
        if (atom->scope.sv.data) {
            sb_sprintf(f->sb, SVFmt "::", SVArg(atom->scope.sv));
        }
        sb_sprintf(f->sb, SVFmt, SVArg(n->token.sv));
    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        format_expr(f, call->fn);

        sb_push(f->sb, '(');
        if (call->fmt_multiline) {
            f->depth++;
        }

        for (Node *it = call->args.head; it; it = it->next) {
            if (call->fmt_multiline) {
                sb_push(f->sb, '\n');
                format_indent(f);
            }

            format_expr(f, it);
            if (it->next) {
                sb_sprintf(f->sb, ", ");
            }
        }

        if (call->fmt_multiline) {
            f->depth--;
            sb_push(f->sb, '\n');
            format_indent(f);
        }
        sb_push(f->sb, ')');
    } break;

    case NODE_CAST: {
        NodeCast *cast = (NodeCast *) n;
        sb_push(f->sb, '<');
        format_type(f, cast->to);
        sb_sprintf(f->sb, "> ");
        format_expr_with_parens_maybe(f, cast->from, POWER_PRE);
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        sb_sprintf(f->sb, SVFmt, SVArg(n->token.sv));
        format_expr_with_parens_maybe(f, unary->operand, POWER_PRE);
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        format_expr_with_parens_maybe(f, index->base, POWER_DOT);
        sb_push(f->sb, '[');
        format_expr(f, index->from);
        if (index->ranged) {
            sb_sprintf(f->sb, "..");
        }
        format_expr(f, index->to);
        sb_push(f->sb, ']');
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;
        const Power mbp = token_kind_to_power(n->token.kind);
        format_expr_with_parens_maybe(f, binary->lhs, mbp);
        sb_sprintf(f->sb, " " SVFmt " ", SVArg(n->token.sv));
        format_expr_with_parens_maybe(f, binary->rhs, mbp);
    } break;

    case NODE_MEMBER: {
        NodeMember *member = (NodeMember *) n;
        format_expr(f, member->lhs);
        sb_sprintf(f->sb, "." SVFmt, SVArg(n->token.sv));
    } break;

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        sb_sprintf(f->sb, "sizeof");
        if (sizeoff->expr) {
            sb_push(f->sb, '(');
            format_expr(f, sizeoff->expr);
            sb_push(f->sb, ')');
        } else {
            sb_push(f->sb, '<');
            format_expr(f, sizeoff->type);
            sb_push(f->sb, '>');
        }
    } break;

    case NODE_COMPOUND: {
        NodeCompound *compound = (NodeCompound *) n;
        format_type(f, compound->type);
        sb_sprintf(f->sb, " {");
        if (compound->fmt_multiline) {
            f->depth++;
        }

        for (Node *it = compound->nodes.head; it; it = it->next) {
            if (compound->fmt_multiline) {
                sb_push(f->sb, '\n');
                format_indent(f);
            }

            if (it->kind == NODE_BINARY && it->token.kind == TOKEN_COLON) {
                NodeBinary *assign = (NodeBinary *) it;
                format_expr(f, assign->lhs);
                sb_sprintf(f->sb, ": ");
                format_expr(f, assign->rhs);
            } else {
                format_expr(f, it);
            }

            if (it->next) {
                sb_sprintf(f->sb, ", ");
            }
        }

        if (compound->fmt_multiline) {
            f->depth--;
            sb_push(f->sb, '\n');
            format_indent(f);
        }
        sb_push(f->sb, '}');
    } break;

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_NODES == 22, "");
static void format_stmt(Formatter *f, Node *n, bool no_indent) {
    if (!n) {
        return;
    }

    if (!no_indent) {
        format_indent(f);
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        NodeAssert *assertt = (NodeAssert *) n;
        if (assertt->is_static) {
            sb_sprintf(f->sb, "static ");
        }
        sb_sprintf(f->sb, "assert ");
        format_expr(f, assertt->expr);

        if (assertt->message) {
            sb_sprintf(f->sb, ", ");
            format_expr(f, assertt->message);
        }
    } break;

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        sb_sprintf(f->sb, "if ");
        format_expr(f, iff->condition);
        sb_push(f->sb, ' ');
        format_stmt(f, iff->consequence, true);

        if (iff->antecedence) {
            sb_sprintf(f->sb, " else ");
            format_stmt(f, iff->antecedence, true);
        }
    } break;

    case NODE_FOR: {
        NodeFor *forr = (NodeFor *) n;
        sb_sprintf(f->sb, "for");
        if (forr->init) {
            sb_push(f->sb, ' ');
            format_stmt(f, forr->init, true);
            sb_push(f->sb, ';');
        }

        if (forr->condition) {
            sb_push(f->sb, ' ');
            format_expr(f, forr->condition);
        }

        if (forr->update) {
            sb_sprintf(f->sb, "; ");
            format_stmt(f, forr->update, true);
        }

        sb_push(f->sb, ' ');
        format_stmt(f, forr->body, true);
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        sb_push(f->sb, '{');

        f->depth++;
        for (Node *it = block->body.head; it; it = it->next) {
            sb_push(f->sb, '\n');
            format_stmt(f, it, false);
        }
        f->depth--;

        if (block->body.head) {
            sb_push(f->sb, '\n');
            format_indent(f);
        }
        sb_push(f->sb, '}');
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        sb_sprintf(f->sb, "return");

        if (ret->value) {
            sb_push(f->sb, ' ');
            format_expr(f, ret->value);
        }
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        if (fn->link) {
            sb_sprintf(f->sb, "#link ");
            format_expr(f, fn->link);
            sb_push(f->sb, '\n');
            format_indent(f);
        }

        sb_sprintf(f->sb, "fn ");
        if (n->token.kind == TOKEN_IDENT) {
            sb_sprintf(f->sb, SVFmt, SVArg(n->token.sv));
        }

        sb_push(f->sb, '(');
        if (fn->fmt_multiline) {
            f->depth++;
        }

        for (Node *arg = fn->args.head; arg; arg = arg->next) {
            if (fn->fmt_multiline) {
                sb_push(f->sb, '\n');
                format_indent(f);
            }

            sb_sprintf(f->sb, SVFmt, SVArg(arg->token.sv));
            sb_push(f->sb, ' ');
            format_type(f, ((NodeVar *) arg)->type);

            if (arg->next) {
                sb_sprintf(f->sb, ", ");
            }
        }

        if (fn->fmt_multiline) {
            f->depth--;
            sb_push(f->sb, '\n');
            format_indent(f);
        }
        sb_sprintf(f->sb, ") ");

        if (fn->ret) {
            format_type(f, fn->ret);
            sb_push(f->sb, ' ');
        }

        format_stmt(f, fn->body, true);
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (var->link) {
            sb_sprintf(f->sb, "#link ");
            format_expr(f, var->link);
            sb_push(f->sb, '\n');
            format_indent(f);
        }

        if (var->is_static) {
            sb_sprintf(f->sb, "static ");
        }

        sb_sprintf(f->sb, "var " SVFmt, SVArg(n->token.sv));
        if (var->type) {
            sb_push(f->sb, ' ');
            format_type(f, var->type);
        }

        if (var->expr) {
            sb_sprintf(f->sb, " = ");
            format_expr(f, var->expr);
        }
    } break;

    case NODE_TYPE:
        sb_sprintf(f->sb, "type " SVFmt " ", SVArg(n->token.sv));
        format_type(f, ((NodeType *) n)->definition);
        break;

    case NODE_CONST: {
        NodeConst *constt = (NodeConst *) n;
        sb_sprintf(f->sb, "const " SVFmt, SVArg(n->token.sv));
        if (constt->type) {
            sb_push(f->sb, ' ');
            format_type(f, constt->type);
        }

        sb_sprintf(f->sb, " = ");
        format_expr(f, constt->expr);
    } break;

    case NODE_FIELD:
        unreachable();
        break;

    case NODE_STRUCT: {
        NodeStruct *structt = (NodeStruct *) n;

        size_t max_field_length = 0;
        for (Node *it = structt->fields.head; it; it = it->next) {
            max_field_length = max(max_field_length, it->token.sv.count);
        }

        sb_sprintf(f->sb, "struct " SVFmt " {\n", SVArg(n->token.sv));

        f->depth++;
        for (Node *it = structt->fields.head; it; it = it->next) {
            format_indent(f);
            sb_sprintf(f->sb, SVFmt "%*s", SVArg(it->token.sv), (int) (max_field_length - it->token.sv.count + 1), "");

            format_type(f, ((NodeField *) it)->type);
            sb_push(f->sb, '\n');
        }
        f->depth--;

        format_indent(f);
        sb_sprintf(f->sb, "}\n");
    } break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        sb_sprintf(f->sb, "extern ");

        for (Node *it = externn->libraries.head; it; it = it->next) {
            format_expr(f, it);
            if (it->next) {
                sb_push(f->sb, ',');
            }
            sb_push(f->sb, ' ');
        }

        sb_sprintf(f->sb, "{");
        f->depth++;
        for (Node *it = externn->definitions.head; it; it = it->next) {
            sb_push(f->sb, '\n');
            format_stmt(f, it, false);
        }
        f->depth--;

        if (externn->definitions.head) {
            sb_push(f->sb, '\n');
            format_indent(f);
        }
        sb_push(f->sb, '}');
    } break;

    case NODE_PRINT: {
        sb_sprintf(f->sb, "print ");
        format_expr(f, ((NodePrint *) n)->operand);
    } break;

    default:
        format_expr(f, n);
        break;
    }
}

static bool fn_needs_newline(NodeFn *fn) {
    assert(fn->body->kind == NODE_BLOCK);
    return ((NodeBlock *) fn->body)->body.head || fn->fmt_multiline;
}

static Import *sort_imports(Import *head) {
    if (!head) {
        return NULL;
    }

    bool     swapped = false;
    Import **p = NULL;

    do {
        swapped = false;
        p = &head;

        while ((*p) && (*p)->next) {
            Import *a = *p;
            Import *b = a->next;
            if (sv_cmp(a->token.sv, b->token.sv) > 0) {
                a->next = b->next;
                b->next = a;
                *p = b;
                swapped = true;
            }

            p = &((*p)->next);
        }
    } while (swapped);

    return head;
}

bool format_file(const char *path, SV package, Import *imports, Node *nodes, SB *sb) {
    const size_t start = sb->count;

    sb_sprintf(sb, "package " SVFmt "\n", SVArg(package));
    if (imports) {
        sb_sprintf(sb, "\nimport ");
        if (imports->next) {
            imports = sort_imports(imports);

            sb_sprintf(sb, "(\n");
            for (Import *it = imports; it; it = it->next) {
                sb_push(sb, '\t');
                if (it->aliased) {
                    sb_sprintf(sb, SVFmt " ", SVArg(it->as));
                }
                sb_sprintf(sb, SVFmt "\n", SVArg(it->token.sv));
            }
            sb_sprintf(sb, ")\n");
        } else {
            if (imports->aliased) {
                sb_sprintf(sb, SVFmt " ", SVArg(imports->as));
            }
            sb_sprintf(sb, SVFmt "\n", SVArg(imports->token.sv));
        }
    }

    if (nodes) {
        sb_push(sb, '\n');
    }

    Formatter f = {.sb = sb};
    for (Node *it = nodes; it; it = it->next) {
        format_stmt(&f, it, false);
        sb_push(f.sb, '\n');

        if (!it->next) {
            break;
        }

        if (it->kind == NODE_FN) {
            if (fn_needs_newline((NodeFn *) it) || it->next->kind != NODE_FN || fn_needs_newline((NodeFn *) it->next)) {
                sb_push(f.sb, '\n');
            }
        } else if (it->kind != NODE_STRUCT && it->kind != NODE_ASSERT && it->next->kind != it->kind) {
            sb_push(f.sb, '\n');
        }
    }

    const SV result = sb_to_sv(*sb, start);
    sb->count = 0;

    FILE *out = fopen(path, "w");
    if (!out) {
        return false;
    }

    fwrite(result.data, result.count, 1, out);
    fclose(out);
    return true;
}

// TODO: Newlines
// TODO: Comments
