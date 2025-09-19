#include <ctype.h>

#include "formatter.h"

static void format_indent(Formatter *f) {
    for (size_t i = 0; i < f->depth; i++) {
        sb_push(&f->sb, '\t');
    }
}

typedef struct {
    size_t indices[2];
    size_t count;
} NewlinesBefore;

static NewlinesBefore format_get_newlines_before(Formatter *f) {
    NewlinesBefore nlb = {0};
    for (size_t i = f->sb.count; i; i--) {
        const char it = f->sb.data[i - 1];
        if (it == '\n') {
            nlb.indices[nlb.count++] = i - 1;
            if (nlb.count == 2) {
                break;
            }
        }

        if (!isspace(it)) {
            break;
        }
    }
    return nlb;
}

static void format_ensure_blank_line(Formatter *f) {
    NewlinesBefore nlb = format_get_newlines_before(f);
    if (nlb.count < 2) {
        sb_push(&f->sb, '\n');
    }

    if (nlb.count < 1) {
        sb_push(&f->sb, '\n');
    }

    if (nlb.count < 2) {
        format_indent(f);
    }
}

static bool format_sync_comments(Formatter *f, Pos *till, bool emit_newline_after_last) {
    bool emitted = false;
    while (f->comments_synced < f->comments.count) {
        const Comment it = f->comments.data[f->comments_synced];
        if (till) {
            if (it.pos.row > till->row) {
                break;
            }

            if (it.pos.row == till->row && it.pos.col > till->col) {
                break;
            }
        }

        f->comments_synced++;

        if (emitted) {
            sb_push(&f->sb, '\n');
            format_indent(f);
        }

        NewlinesBefore nlb = format_get_newlines_before(f);
        switch (it.ws) {
        case CWS_INLINE:
            if (nlb.count) {
                f->sb.count = nlb.indices[nlb.count - 1];
            }

            if (!f->sb.count || f->sb.data[f->sb.count - 1] != ' ') {
                sb_push(&f->sb, ' ');
            }
            break;

        case CWS_NEWLINE:
            if (nlb.count < 1) {
                sb_push(&f->sb, '\n');
                format_indent(f);
            }
            break;

        case CWS_BLANKLINE:
            if (nlb.count < 2) {
                sb_push(&f->sb, '\n');
            }

            if (nlb.count < 1) {
                sb_push(&f->sb, '\n');
            }

            if (nlb.count < 2) {
                format_indent(f);
            }
            break;
        }

        sb_sprintf(&f->sb, "//");
        if (it.sv.count && !isspace(*it.sv.data)) {
            sb_push(&f->sb, ' ');
        }

        sb_push_many(&f->sb, it.sv.data, it.sv.count);
        emitted = true;
    }

    if (emitted && emit_newline_after_last) {
        sb_push(&f->sb, '\n');
        format_indent(f);
    }

    return emitted;
}

static void format_expr(Formatter *f, Node *n, bool sync_comments_before);

static_assert(COUNT_NODES == 22, "");
static void format_type(Formatter *f, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;
        if (atom->scope.sv.data) {
            sb_sprintf(&f->sb, SVFmt "::", SVArg(atom->scope.sv));
        }
        sb_sprintf(&f->sb, SVFmt, SVArg(n->token.sv));
    } break;

    case NODE_UNARY: {
        sb_push(&f->sb, '&');
        format_type(f, ((NodeUnary *) n)->operand);
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        sb_push(&f->sb, '[');
        format_expr(f, index->from, true);
        sb_push(&f->sb, ']');
        format_type(f, index->base);
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        sb_sprintf(&f->sb, "fn (");
        if (fn->fmt_multiline) {
            f->depth++;
        }

        for (Node *it = fn->args.head; it; it = it->next) {
            if (it->fmt_newline) {
                sb_push(&f->sb, '\n');
            }

            if (fn->fmt_multiline) {
                sb_push(&f->sb, '\n');
                format_indent(f);
            }

            format_type(f, ((NodeVar *) it)->type);
            if (it->next) {
                sb_push(&f->sb, ',');
                if (!fn->fmt_multiline) {
                    sb_push(&f->sb, ' ');
                }
            }
        }

        if (fn->fmt_multiline) {
            f->depth--;
            sb_push(&f->sb, '\n');
            format_indent(f);
        }
        sb_push(&f->sb, ')');

        if (fn->ret) {
            sb_push(&f->sb, ' ');
            format_type(f, fn->ret);
        }
    } break;

    default:
        unreachable();
    }
}

static void format_expr_with_parens_maybe(Formatter *f, Node *n, Power mbp) {
    bool parens = false;
    if (n->kind == NODE_BINARY && token_kind_to_power(n->token.kind) < mbp) {
        parens = true;
    } else if (n->kind == NODE_UNARY && POWER_PRE < mbp) {
        parens = true;
    } else if (n->kind == NODE_CAST && POWER_PRE < mbp) {
        parens = true;
    }

    if (parens) {
        sb_push(&f->sb, '(');
        format_expr(f, n, true);
        sb_push(&f->sb, ')');
    } else {
        format_expr(f, n, true);
    }
}

static void format_stmt(Formatter *f, Node *n, bool no_indent);

static void format_fn(Formatter *f, NodeFn *fn) {
    if (fn->link) {
        sb_sprintf(&f->sb, "#link ");
        format_expr(f, fn->link, true);
        sb_push(&f->sb, '\n');
        format_indent(f);
    }

    if (fn->is_public) {
        sb_sprintf(&f->sb, "pub ");
    }
    sb_sprintf(&f->sb, "fn ");
    if (fn->node.token.kind == TOKEN_IDENT) {
        sb_sprintf(&f->sb, SVFmt, SVArg(fn->node.token.sv));
    }

    if (fn->generics.head) {
        sb_push(&f->sb, '<');
        for (Node *type = fn->generics.head; type; type = type->next) {
            sb_sprintf(&f->sb, SVFmt, SVArg(type->token.sv));
            if (type->next) {
                sb_sprintf(&f->sb, ", ");
            }
        }
        sb_push(&f->sb, '>');
    }

    sb_push(&f->sb, '(');
    if (fn->fmt_multiline) {
        f->depth++;
    }

    for (Node *arg = fn->args.head; arg; arg = arg->next) {
        if (arg->fmt_newline) {
            sb_push(&f->sb, '\n');
        }

        if (fn->fmt_multiline) {
            sb_push(&f->sb, '\n');
            format_indent(f);
        }

        sb_sprintf(&f->sb, SVFmt, SVArg(arg->token.sv));
        sb_push(&f->sb, ' ');
        format_type(f, ((NodeVar *) arg)->type);

        if (arg->next) {
            sb_push(&f->sb, ',');
            if (!fn->fmt_multiline) {
                sb_push(&f->sb, ' ');
            }
        }
    }

    if (fn->fmt_multiline) {
        f->depth--;
        sb_push(&f->sb, '\n');
        format_indent(f);
    }
    sb_sprintf(&f->sb, ") ");

    if (fn->ret) {
        format_type(f, fn->ret);
        sb_push(&f->sb, ' ');
    }

    format_stmt(f, fn->body, true);
}

static void format_expr(Formatter *f, Node *n, bool sync_comments_before) {
    if (!n) {
        return;
    }

    if (sync_comments_before) {
        format_sync_comments(f, &n->token.pos, true);
        if (n->fmt_newline) {
            format_ensure_blank_line(f);
        }
    }

    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;
        if (atom->scope.sv.data) {
            sb_sprintf(&f->sb, SVFmt "::", SVArg(atom->scope.sv));
        }
        sb_sprintf(&f->sb, SVFmt, SVArg(n->token.sv));

        if (atom->generics.head) {
            sb_sprintf(&f->sb, "::<");
            for (Node *type = atom->generics.head; type; type = type->next) {
                sb_sprintf(&f->sb, SVFmt, SVArg(type->token.sv));
                if (type->next) {
                    sb_sprintf(&f->sb, ", ");
                }
            }
            sb_push(&f->sb, '>');
        }

    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        if (call->fn->kind == NODE_FN) {
            sb_push(&f->sb, '(');
            format_expr(f, call->fn, true);
            sb_push(&f->sb, ')');
        } else {
            format_expr_with_parens_maybe(f, call->fn, POWER_DOT);
        }

        sb_push(&f->sb, '(');
        if (call->fmt_multiline) {
            f->depth++;
        }

        for (Node *it = call->args.head; it; it = it->next) {
            if (it->fmt_newline) {
                sb_push(&f->sb, '\n');
            }

            if (call->fmt_multiline) {
                sb_push(&f->sb, '\n');
                format_indent(f);
            }

            format_expr(f, it, true);
            if (it->next) {
                sb_push(&f->sb, ',');
                if (!call->fmt_multiline) {
                    sb_push(&f->sb, ' ');
                }
            }
        }

        format_sync_comments(f, &call->rparen_pos, false);
        if (call->fmt_multiline) {
            f->depth--;
            sb_push(&f->sb, '\n');
            format_indent(f);
        }
        sb_push(&f->sb, ')');
    } break;

    case NODE_CAST: {
        NodeCast *cast = (NodeCast *) n;
        sb_push(&f->sb, '<');
        format_type(f, cast->to);
        sb_sprintf(&f->sb, "> ");
        format_expr_with_parens_maybe(f, cast->from, POWER_PRE);
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        sb_sprintf(&f->sb, SVFmt, SVArg(n->token.sv));
        if (n->token.kind == TOKEN_LEN) {
            da_push(&f->sb, '(');
            format_expr(f, unary->operand, true);
            da_push(&f->sb, ')');
        } else {
            format_expr_with_parens_maybe(f, unary->operand, POWER_PRE);
        }
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        format_expr_with_parens_maybe(f, index->base, POWER_DOT);
        sb_push(&f->sb, '[');
        format_expr(f, index->from, true);
        if (index->ranged) {
            sb_sprintf(&f->sb, "..");
        }
        format_expr(f, index->to, true);
        sb_push(&f->sb, ']');
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;
        const Power mbp = token_kind_to_power(n->token.kind);
        format_expr_with_parens_maybe(f, binary->lhs, mbp);
        sb_sprintf(&f->sb, " " SVFmt " ", SVArg(n->token.sv));
        format_expr_with_parens_maybe(f, binary->rhs, mbp);
    } break;

    case NODE_MEMBER: {
        NodeMember *member = (NodeMember *) n;
        format_expr(f, member->lhs, true);
        sb_sprintf(&f->sb, "." SVFmt, SVArg(n->token.sv));
    } break;

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        sb_sprintf(&f->sb, "sizeof");
        if (sizeoff->expr) {
            sb_push(&f->sb, '(');
            format_expr(f, sizeoff->expr, true);
            sb_push(&f->sb, ')');
        } else {
            sb_push(&f->sb, '<');
            format_expr(f, sizeoff->type, true);
            sb_push(&f->sb, '>');
        }
    } break;

    case NODE_COMPOUND: {
        NodeCompound *compound = (NodeCompound *) n;
        format_type(f, compound->type);
        sb_sprintf(&f->sb, " {");
        if (compound->fmt_multiline) {
            f->depth++;
        }

        typedef struct {
            size_t start;
            size_t count;
            bool   newline;
        } Designator;

        Designator *designators = NULL;
        if (compound->designators && compound->fmt_multiline) {
            designators = temp_alloc(compound->designators * sizeof(*designators));
        }
        Designator *dp = designators;

        for (Node *it = compound->nodes.head; it; it = it->next) {
            if (it->fmt_newline) {
                sb_push(&f->sb, '\n');
            }

            if (compound->fmt_multiline) {
                sb_push(&f->sb, '\n');
                format_indent(f);
            }

            if (it->kind == NODE_BINARY && it->token.kind == TOKEN_COLON) {
                NodeBinary *assign = (NodeBinary *) it;
                if (dp) {
                    dp->start = f->sb.count;
                    dp->newline = it->fmt_newline;
                }

                format_expr(f, assign->lhs, true);
                if (dp) {
                    dp->count = f->sb.count - dp->start;
                }

                sb_sprintf(&f->sb, ": ");
                if (dp) {
                    dp->start = f->sb.count;
                    dp++;
                }

                format_expr(f, assign->rhs, true);
            } else {
                format_expr(f, it, true);
            }

            if (it->next) {
                sb_push(&f->sb, ',');
                if (!compound->fmt_multiline) {
                    sb_push(&f->sb, ' ');
                }
            }
        }

        if (designators) {
            size_t offset = 0;
            size_t max_spaces = 0;
            for (size_t i = 0, j = 0; i < compound->designators; i++) {
                const Designator it = designators[i];
                if (!it.newline) {
                    max_spaces = max(max_spaces, it.count);
                }

                if (it.newline || i + 1 == compound->designators) {
                    while (j < i) {
                        const Designator it = designators[j++];
                        const size_t     n = max_spaces - it.count;
                        sb_insert(&f->sb, ' ', it.start + offset, n);
                        offset += n;
                    }

                    max_spaces = it.count;
                }
            }

            temp_reset(designators);
        }

        format_sync_comments(f, &compound->rbrace_pos, false);
        if (compound->fmt_multiline) {
            f->depth--;
            sb_push(&f->sb, '\n');
            format_indent(f);
        }
        sb_push(&f->sb, '}');
    } break;

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        sb_sprintf(&f->sb, "if ");
        format_expr(f, iff->condition, true);
        sb_sprintf(&f->sb, " then ");
        format_expr(f, iff->consequence, true);
        sb_sprintf(&f->sb, " else ");
        format_expr(f, iff->antecedence, true);
    } break;

    case NODE_FN:
        format_fn(f, (NodeFn *) n);
        break;

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

    if (n->fmt_toplevel_newline) {
        sb_push(&f->sb, '\n');
    }

    if (!no_indent) {
        format_indent(f);
    }

    if (!format_sync_comments(f, &n->token.pos, true) && n->fmt_newline) {
        format_ensure_blank_line(f);
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        NodeAssert *assertt = (NodeAssert *) n;
        if (assertt->is_static) {
            sb_sprintf(&f->sb, "static ");
        }
        sb_sprintf(&f->sb, "assert ");
        format_expr(f, assertt->expr, true);

        if (assertt->message) {
            sb_sprintf(&f->sb, ", ");
            format_expr(f, assertt->message, true);
        }
    } break;

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        if (iff->expr) {
            sb_push(&f->sb, '(');
            format_expr(f, n, false);
            sb_push(&f->sb, ')');
        } else {
            sb_sprintf(&f->sb, "if ");
            format_expr(f, iff->condition, true);
            sb_push(&f->sb, ' ');
            format_stmt(f, iff->consequence, true);

            if (iff->antecedence) {
                sb_sprintf(&f->sb, " else ");
                format_stmt(f, iff->antecedence, true);
            }
        }
    } break;

    case NODE_FOR: {
        NodeFor *forr = (NodeFor *) n;
        sb_sprintf(&f->sb, "for");
        if (forr->init) {
            sb_push(&f->sb, ' ');
            format_stmt(f, forr->init, true);
            sb_push(&f->sb, ';');
        }

        if (forr->condition) {
            sb_push(&f->sb, ' ');
            format_expr(f, forr->condition, true);
        }

        if (forr->update) {
            sb_sprintf(&f->sb, "; ");
            format_stmt(f, forr->update, true);
        }

        sb_push(&f->sb, ' ');
        format_stmt(f, forr->body, true);
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        sb_push(&f->sb, '{');

        f->depth++;
        for (Node *it = block->body.head; it; it = it->next) {
            sb_push(&f->sb, '\n');
            if (it->fmt_newline) {
                sb_push(&f->sb, '\n');
            }
            format_stmt(f, it, false);
        }

        const bool comments = format_sync_comments(f, &block->rbrace_pos, false);
        f->depth--;

        if (block->body.head || comments) {
            sb_push(&f->sb, '\n');
            format_indent(f);
        }

        sb_push(&f->sb, '}');
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        sb_sprintf(&f->sb, "return");

        if (ret->value) {
            sb_push(&f->sb, ' ');
            format_expr(f, ret->value, true);
        }
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        if (fn->node.token.kind != TOKEN_IDENT) {
            sb_push(&f->sb, '(');
            format_fn(f, fn);
            sb_push(&f->sb, ')');
        } else {
            format_fn(f, fn);
        }
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (var->link) {
            sb_sprintf(&f->sb, "#link ");
            format_expr(f, var->link, true);
            sb_push(&f->sb, '\n');
            format_indent(f);
        }

        if (var->is_public) {
            sb_sprintf(&f->sb, "pub ");
        }

        if (var->is_static) {
            sb_sprintf(&f->sb, "static ");
        }

        sb_sprintf(&f->sb, "var " SVFmt, SVArg(n->token.sv));
        if (var->type) {
            sb_push(&f->sb, ' ');
            format_type(f, var->type);
        }

        if (var->expr) {
            sb_sprintf(&f->sb, " = ");
            format_expr(f, var->expr, true);
        }
    } break;

    case NODE_TYPE: {
        NodeType *type = (NodeType *) n;
        if (type->is_public) {
            sb_sprintf(&f->sb, "pub ");
        }

        sb_sprintf(&f->sb, "type " SVFmt " ", SVArg(n->token.sv));
        format_type(f, type->definition);
    } break;

    case NODE_CONST: {
        NodeConst *constt = (NodeConst *) n;
        if (constt->is_public) {
            sb_sprintf(&f->sb, "pub ");
        }

        sb_sprintf(&f->sb, "const " SVFmt, SVArg(n->token.sv));
        if (constt->type) {
            sb_push(&f->sb, ' ');
            format_type(f, constt->type);
        }

        sb_sprintf(&f->sb, " = ");
        format_expr(f, constt->expr, true);
    } break;

    case NODE_FIELD:
        unreachable();
        break;

    case NODE_STRUCT: {
        NodeStruct *structt = (NodeStruct *) n;
        if (structt->is_public) {
            sb_sprintf(&f->sb, "pub ");
        }

        size_t max_field_length = 0;
        for (Node *it = structt->fields.head; it && !it->fmt_newline; it = it->next) {
            max_field_length = max(max_field_length, it->token.sv.count);
        }

        sb_sprintf(&f->sb, "struct " SVFmt " {\n", SVArg(n->token.sv));

        f->depth++;
        for (Node *it = structt->fields.head; it; it = it->next) {
            if (it->fmt_newline) {
                sb_push(&f->sb, '\n');

                max_field_length = it->token.sv.count;
                for (Node *it2 = it->next; it2 && !it2->fmt_newline; it2 = it2->next) {
                    max_field_length = max(max_field_length, it2->token.sv.count);
                }
            }

            format_indent(f);
            sb_sprintf(&f->sb, SVFmt "%*s", SVArg(it->token.sv), (int) (max_field_length - it->token.sv.count + 1), "");

            format_type(f, ((NodeField *) it)->type);
            sb_push(&f->sb, '\n');
        }
        f->depth--;

        format_indent(f);
        sb_sprintf(&f->sb, "}");
    } break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;

        bool all_public = externn->definitions.head != NULL;
        for (Node *it = externn->definitions.head; it; it = it->next) {
            if (it->kind == NODE_FN) {
                if (!((NodeFn *) it)->is_public) {
                    all_public = false;
                    break;
                }
            } else if (it->kind == NODE_VAR) {
                if (!((NodeVar *) it)->is_public) {
                    all_public = false;
                    break;
                }
            } else {
                unreachable();
            }
        }

        if (all_public) {
            for (Node *it = externn->definitions.head; it; it = it->next) {
                if (it->kind == NODE_FN) {
                    ((NodeFn *) it)->is_public = false;
                } else if (it->kind == NODE_VAR) {
                    ((NodeVar *) it)->is_public = false;
                } else {
                    unreachable();
                }
            }

            sb_sprintf(&f->sb, "pub ");
        }

        sb_sprintf(&f->sb, "extern ");

        for (Node *it = externn->libraries.head; it; it = it->next) {
            format_expr(f, it, true);
            if (it->next) {
                sb_push(&f->sb, ',');
            }
            sb_push(&f->sb, ' ');
        }

        sb_sprintf(&f->sb, "{");
        f->depth++;
        for (Node *it = externn->definitions.head; it; it = it->next) {
            sb_push(&f->sb, '\n');
            format_stmt(f, it, false);
        }
        f->depth--;

        if (externn->definitions.head) {
            sb_push(&f->sb, '\n');
            format_indent(f);
        }
        sb_push(&f->sb, '}');
    } break;

    case NODE_PRINT: {
        sb_sprintf(&f->sb, "print ");
        format_expr(f, ((NodePrint *) n)->operand, true);
    } break;

    default:
        format_expr(f, n, false);
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

void formatter_free(Formatter *f) {
    sb_free(&f->sb);
    da_free(&f->comments);
}

bool format_file(Formatter *f, const char *path, SV package, Import *imports, Node *nodes) {
    const size_t start = f->sb.count;

    sb_sprintf(&f->sb, "package " SVFmt "\n", SVArg(package));
    if (imports) {
        sb_sprintf(&f->sb, "\nimport ");
        if (imports->next) {
            imports = sort_imports(imports);

            sb_sprintf(&f->sb, "(\n");
            for (Import *it = imports; it; it = it->next) {
                sb_push(&f->sb, '\t');
                if (it->aliased) {
                    sb_sprintf(&f->sb, SVFmt " ", SVArg(it->as));
                }
                sb_sprintf(&f->sb, SVFmt "\n", SVArg(it->token.sv));
            }
            sb_sprintf(&f->sb, ")\n");
        } else {
            if (imports->aliased) {
                sb_sprintf(&f->sb, SVFmt " ", SVArg(imports->as));
            }
            sb_sprintf(&f->sb, SVFmt "\n", SVArg(imports->token.sv));
        }
    }

    if (nodes && !nodes->fmt_toplevel_newline) {
        sb_push(&f->sb, '\n');
    }

    for (Node *it = nodes; it; it = it->next) {
        format_stmt(f, it, false);
        sb_push(&f->sb, '\n');

        if (!it->next || it->next->fmt_toplevel_newline) {
            continue;
        }

        if (it->kind == NODE_FN) {
            if (fn_needs_newline((NodeFn *) it) || it->next->kind != NODE_FN || fn_needs_newline((NodeFn *) it->next)) {
                sb_push(&f->sb, '\n');
            }
        } else if (it->kind == NODE_STRUCT) {
            sb_push(&f->sb, '\n');
        } else if (it->kind != NODE_ASSERT && it->next->kind != it->kind) {
            sb_push(&f->sb, '\n');
        }
    }

    format_sync_comments(f, NULL, true);

    const SV result = sb_to_sv(*&f->sb, start);
    f->sb.count = 0;
    f->comments.count = 0;
    f->comments_synced = 0;

    FILE *out = fopen(path, "w");
    if (!out) {
        return false;
    }

    fwrite(result.data, result.count, 1, out);
    fclose(out);
    return true;
}
