#include "parser.h"
#include "message.h"

static void nodes_append(Nodes *dst, Nodes *src) {
    nodes_push(dst, src->head);
    dst->tail = src->tail;
}

static Import *imports_find(Imports is, SV name) {
    for (Import *it = is.head; it; it = it->next) {
        if (sv_eq(it->as, name)) {
            return it;
        }
    }

    return NULL;
}

void parser_free(Parser *p) {
    da_free(&p->paths);
}

static_assert(COUNT_NODES == 23, "");
static void *node_alloc(Parser *p, NodeKind kind, Token token) {
    static const size_t sizes[COUNT_NODES] = {
        [NODE_ATOM] = sizeof(NodeAtom), // Prevent clang-format from messing this up
        [NODE_CALL] = sizeof(NodeCall),
        [NODE_CAST] = sizeof(NodeCast),
        [NODE_UNARY] = sizeof(NodeUnary),
        [NODE_INDEX] = sizeof(NodeIndex),
        [NODE_BINARY] = sizeof(NodeBinary),
        [NODE_MEMBER] = sizeof(NodeMember),
        [NODE_SIZEOF] = sizeof(NodeSizeof),
        [NODE_ASSERT] = sizeof(NodeAssert),
        [NODE_COMPOUND] = sizeof(NodeCompound), // Prevent clang-format from messing this up

        [NODE_IF] = sizeof(NodeIf),
        [NODE_FOR] = sizeof(NodeFor),
        [NODE_BLOCK] = sizeof(NodeBlock),

        [NODE_JUMP] = sizeof(Node),
        [NODE_RETURN] = sizeof(NodeReturn),

        [NODE_FN] = sizeof(NodeFn),
        [NODE_VAR] = sizeof(NodeVar),
        [NODE_TYPE] = sizeof(NodeType),
        [NODE_CONST] = sizeof(NodeConst),
        [NODE_FIELD] = sizeof(NodeField),
        [NODE_STRUCT] = sizeof(NodeStruct),
        [NODE_EXTERN] = sizeof(NodeExtern),

        [NODE_PRINT] = sizeof(NodePrint),
    };

    assert(kind >= NODE_ATOM && kind < COUNT_NODES);
    const size_t size = sizes[kind];

    Node *node = arena_alloc(p->arena, size);
    node->kind = kind;
    node->token = token;
    return node;
}

static void error_unexpected(Token token) {
    error_full(ERROR, token.pos, "Unexpected %s", token_kind_to_cstr(token.kind));
    exit(1);
}

static_assert(COUNT_TOKENS == 69, "");
static bool token_kind_is_start_of_type(TokenKind k) {
    switch (k) {
    case TOKEN_IDENT:
    case TOKEN_LBRACKET:
    case TOKEN_BAND:
    case TOKEN_LAND:
    case TOKEN_FN:
        return true;

    default:
        return false;
    }
}

typedef enum {
    PF_COMPOUND_ALLOWED = 1 << 0,
    PF_CONSTANT_EXPR = 1 << 1
} ParseFlags;

static Node *parse_type(Parser *p);
static Node *parse_expr(Parser *p, Power mbp, ParseFlags flags);

static void parse_generics(Parser *p, Nodes *generics, size_t *generics_count) {
    Token token = {0};
    do {
        nodes_push(generics, parse_type(p));
        (*generics_count)++;

        token = lexer_peek(&p->lexer);
        switch (token.kind) {
        case TOKEN_GE:
        case TOKEN_SHR:
        case TOKEN_SHR_SET:
            lexer_unbuffer(&p->lexer);
            token = lexer_split_token(&p->lexer, p->lexer.buffer);
            break;

        default:
            token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_GT);
            break;
        }
    } while (token.kind != TOKEN_GT);
}

static_assert(COUNT_TOKENS == 69, "");
static Node *parse_type(Parser *p) {
    Node *node = NULL;
    Token token = lexer_next(&p->lexer);

    switch (token.kind) {
    case TOKEN_IDENT: {
        NodeAtom *atom = node_alloc(p, NODE_ATOM, token);
        atom->package = p->packages->current;

        if (lexer_read(&p->lexer, TOKEN_SCOPE)) {
            token = lexer_expect(&p->lexer, TOKEN_IDENT, TOKEN_LT);
            if (token.kind == TOKEN_IDENT) {
                atom->scope = atom->node.token;
                atom->node.token = token;
            } else {
                lexer_buffer(&p->lexer, token);
            }
        }

        if (lexer_read(&p->lexer, TOKEN_SCOPE)) {
            token = lexer_expect(&p->lexer, TOKEN_LT);
            lexer_buffer(&p->lexer, token);
        } else {
            token = lexer_peek(&p->lexer);
        }

        if (token.kind == TOKEN_LT && !token.newlines) {
            lexer_unbuffer(&p->lexer);
            parse_generics(p, &atom->generics, &atom->generics_count);
        }

        node = (Node *) atom;
    } break;

    case TOKEN_LBRACKET: {
        NodeIndex *index = node_alloc(p, NODE_INDEX, token);

        token = lexer_peek(&p->lexer);
        if (token.kind == TOKEN_RBRACKET) {
            lexer_unbuffer(&p->lexer);
        } else if (token.kind == TOKEN_RANGE) {
            index->ranged = true;
            lexer_unbuffer(&p->lexer);
            lexer_expect(&p->lexer, TOKEN_RBRACKET);
        } else {
            index->from = parse_expr(p, POWER_SET, PF_CONSTANT_EXPR);
            lexer_expect(&p->lexer, TOKEN_RBRACKET);
        }

        index->base = parse_type(p);
        node = (Node *) index;
    } break;

    case TOKEN_BAND: {
        NodeUnary *unary = node_alloc(p, NODE_UNARY, token);
        unary->operand = parse_type(p);
        node = (Node *) unary;
    } break;

    case TOKEN_LAND: {
        NodeUnary *unary = node_alloc(p, NODE_UNARY, lexer_split_token(&p->lexer, token));
        unary->operand = parse_type(p);
        node = (Node *) unary;
    } break;

    case TOKEN_FN: {
        const size_t starting_row = lexer_expect(&p->lexer, TOKEN_LPAREN).pos.row;

        NodeFn *fn = node_alloc(p, NODE_FN, token);
        while (!lexer_read(&p->lexer, TOKEN_RPAREN)) {
            const bool fmt_newline = fn->args.head && lexer_peek(&p->lexer).newlines > 1;

            NodeVar *arg = node_alloc(p, NODE_VAR, fn->node.token);
            arg->type = parse_type(p);

            nodes_push(&fn->args, (Node *) arg);
            fn->args.tail->fmt_newline = fmt_newline;
            fn->arity++;

            token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN);
            if (token.kind != TOKEN_COMMA) {
                break;
            }
        }

        if (p->lexer.pos.row != starting_row) {
            fn->fmt_multiline = true;
        }

        token = lexer_peek(&p->lexer);
        if (!token.newlines && token_kind_is_start_of_type(token.kind)) {
            fn->ret = parse_type(p);
        }

        node = (Node *) fn;
    } break;

    default:
        error_unexpected(token);
        break;
    }

    return node;
}

static bool node_is_compound_literal_type(Node *n) {
    if (n->kind == NODE_ATOM && n->token.kind == TOKEN_IDENT) {
        return true;
    }

    return false;
}

static Node *parse_fn(Parser *p, Token name);

static_assert(COUNT_NODES == 23, "");
static void ensure_const_expr(Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_CALL:
        error_full(ERROR, n->token.pos, "Unexpected call in constant expression");
        exit(1);
        break;

    case NODE_CAST: {
        NodeCast *cast = (NodeCast *) n;
        ensure_const_expr(cast->from);
    } break;

    case NODE_UNARY: {
        if (n->token.kind == TOKEN_MUL) {
            error_full(ERROR, n->token.pos, "Unexpected dereference in constant expression");
            exit(1);
        }

        if (n->token.kind == TOKEN_BAND) {
            error_full(ERROR, n->token.pos, "Unexpected reference in constant expression");
            exit(1);
        }

        NodeUnary *unary = (NodeUnary *) n;
        ensure_const_expr(unary->operand);
    } break;

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;
        ensure_const_expr(index->base);
        ensure_const_expr(index->from);
        ensure_const_expr(index->to);
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;
        ensure_const_expr(binary->lhs);
        ensure_const_expr(binary->rhs);
    } break;

    case NODE_MEMBER:
        error_full(ERROR, n->token.pos, "Unexpected member access in constant expression");
        exit(1);
        break;

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;
        ensure_const_expr(sizeoff->expr);
    } break;

    case NODE_COMPOUND:
        error_full(ERROR, n->token.pos, "Unexpected compound literal in constant expression");
        exit(1);
        break;

    case NODE_FN:
        error_full(ERROR, n->token.pos, "Unexpected function in constant expression");
        exit(1);
        break;

    default:
        break;
    }
}

static NodeCompound *parse_compound(Parser *p, Node *node, Token token, ParseFlags flags) {
    const size_t starting_row = token.pos.row;

    NodeCompound *compound = node_alloc(p, NODE_COMPOUND, token);
    compound->type = node;

    enum {
        COMPOUND_UNKNOWN,
        COMPOUND_ORDERED,
        COMPOUND_DESIGNATED,
    } kind = COMPOUND_UNKNOWN;

    while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
        const bool fmt_newline = compound->nodes.head && lexer_peek(&p->lexer).newlines > 1;

        Node *expr = parse_expr(p, POWER_SET, flags | PF_COMPOUND_ALLOWED);
        token = lexer_peek(&p->lexer);
        if (token.kind == TOKEN_COLON) {
            lexer_unbuffer(&p->lexer);

            if (kind == COMPOUND_ORDERED) {
                error_full(ERROR, token.pos, "Cannot mix ordered and designated initializers");
                exit(1);
            }
            kind = COMPOUND_DESIGNATED;

            ensure_const_expr(expr);

            NodeBinary *assign = node_alloc(p, NODE_BINARY, token);
            assign->lhs = expr;
            assign->rhs = parse_expr(p, POWER_SET, flags | PF_COMPOUND_ALLOWED);
            nodes_push(&compound->nodes, (Node *) assign);
            compound->designators++;
        } else {
            if (kind == COMPOUND_DESIGNATED) {
                error_full(ERROR, expr->token.pos, "Cannot mix ordered and designated initializers");
                exit(1);
            }
            kind = COMPOUND_ORDERED;

            nodes_push(&compound->nodes, expr);
        }

        compound->nodes.tail->fmt_newline = fmt_newline;

        token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_RBRACE);
        if (token.kind != TOKEN_COMMA) {
            break;
        }
    }

    compound->rbrace_pos = p->lexer.pos;
    compound->rbrace_pos.col--; // The lexer has already consumed the '}'

    if (p->lexer.pos.row != starting_row) {
        compound->fmt_multiline = true;
    }

    return compound;
}

static_assert(COUNT_TOKENS == 69, "");
static Node *parse_expr(Parser *p, Power mbp, ParseFlags flags) {
    Node *node = NULL;
    Token token = lexer_next(&p->lexer);

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_STR:
    case TOKEN_BOOL:
    case TOKEN_CHAR:
        node = node_alloc(p, NODE_ATOM, token);
        break;

    case TOKEN_IDENT: {
        NodeAtom *atom = node_alloc(p, NODE_ATOM, token);
        atom->package = p->packages->current;

        if (lexer_read(&p->lexer, TOKEN_SCOPE)) {
            token = lexer_expect(&p->lexer, TOKEN_IDENT, TOKEN_LT);
            if (token.kind == TOKEN_IDENT) {
                atom->scope = atom->node.token;
                atom->node.token = token;
                if (lexer_read(&p->lexer, TOKEN_SCOPE)) {
                    token = lexer_expect(&p->lexer, TOKEN_LT);
                }
            }

            if (token.kind == TOKEN_LT) {
                if (flags & PF_CONSTANT_EXPR) {
                    error_full(ERROR, token.pos, "Unexpected generic instantiation in constant expression");
                    exit(1);
                }

                parse_generics(p, &atom->generics, &atom->generics_count);
            }
        }

        node = (Node *) atom;
    } break;

    case TOKEN_MUL: {
        if (flags & PF_CONSTANT_EXPR) {
            error_full(ERROR, token.pos, "Unexpected dereference in constant expression");
            exit(1);
        }

        NodeUnary *unary = node_alloc(p, NODE_UNARY, token);
        unary->operand = parse_expr(p, POWER_PRE, flags);
        node = (Node *) unary;
    } break;

    case TOKEN_BAND: {
        if (flags & PF_CONSTANT_EXPR) {
            error_full(ERROR, token.pos, "Unexpected reference in constant expression");
            exit(1);
        }

        NodeUnary *unary = node_alloc(p, NODE_UNARY, token);
        unary->operand = parse_expr(p, POWER_PRE, flags);
        node = (Node *) unary;
    } break;

    case TOKEN_SUB:
    case TOKEN_BNOT:
    case TOKEN_LNOT: {
        NodeUnary *unary = node_alloc(p, NODE_UNARY, token);
        unary->operand = parse_expr(p, POWER_PRE, flags);
        node = (Node *) unary;
    } break;

    case TOKEN_LEN: {
        NodeUnary *unary = node_alloc(p, NODE_UNARY, token);
        lexer_expect(&p->lexer, TOKEN_LPAREN);
        unary->operand = parse_expr(p, POWER_SET, flags);
        lexer_expect(&p->lexer, TOKEN_RPAREN);
        node = (Node *) unary;
    } break;

    case TOKEN_SIZEOF: {
        NodeSizeof *sizeoff = node_alloc(p, NODE_SIZEOF, token);

        token = lexer_expect(&p->lexer, TOKEN_LPAREN, TOKEN_LT);
        if (token.kind == TOKEN_LPAREN) {
            sizeoff->expr = parse_expr(p, POWER_SET, flags | PF_COMPOUND_ALLOWED);
            lexer_expect(&p->lexer, TOKEN_RPAREN);
        } else if (token.kind == TOKEN_LT) {
            sizeoff->type = parse_type(p);
            lexer_expect(&p->lexer, TOKEN_GT);
        } else {
            unreachable();
        }

        node = (Node *) sizeoff;
    } break;

    case TOKEN_LT: {
        NodeCast *cast = node_alloc(p, NODE_CAST, token);
        cast->to = parse_type(p);

        lexer_expect(&p->lexer, TOKEN_GT);
        cast->from = parse_expr(p, POWER_PRE, flags);

        node = (Node *) cast;
    } break;

    case TOKEN_LPAREN:
        node = parse_expr(p, POWER_SET, flags | PF_COMPOUND_ALLOWED);
        lexer_expect(&p->lexer, TOKEN_RPAREN);
        break;

    case TOKEN_LBRACKET:
        if (flags & PF_COMPOUND_ALLOWED) {
            if (flags & PF_CONSTANT_EXPR) {
                error_full(ERROR, token.pos, "Unexpected compound literal in constant expression");
                exit(1);
            }

            lexer_buffer(&p->lexer, token);

            node = parse_type(p);
            if (((NodeIndex *) node)->ranged) {
                error_full(ERROR, node->token.pos, "Cannot construct compound literal with dynamic slice type");
                exit(1);
            }

            token = lexer_expect(&p->lexer, TOKEN_LBRACE);
            if (token.newlines) {
                error_full(ERROR, token.pos, "Expected '{' on same line as type");
                exit(1);
            }

            node = (Node *) parse_compound(p, node, token, flags);
        } else {
            error_unexpected(token);
        }
        break;

    case TOKEN_IF: {
        NodeIf *iff = node_alloc(p, NODE_IF, token);
        iff->condition = parse_expr(p, POWER_SET, flags);

        lexer_expect(&p->lexer, TOKEN_THEN);
        iff->consequence = parse_expr(p, POWER_SET, flags);

        lexer_expect(&p->lexer, TOKEN_ELSE);
        iff->antecedence = parse_expr(p, POWER_SET, flags);

        iff->expr = true;
        node = (Node *) iff;
    } break;

    case TOKEN_FN:
        if (flags & PF_CONSTANT_EXPR) {
            error_full(ERROR, token.pos, "Unexpected function in constant expression");
            exit(1);
        }

        node = parse_fn(p, token);
        break;

    default:
        error_unexpected(token);
    }

    while (true) {
        token = lexer_peek(&p->lexer);
        if (token.newlines) {
            break;
        }

        const Power lbp = token_kind_to_power(token.kind);
        if (lbp <= mbp) {
            break;
        }
        lexer_unbuffer(&p->lexer);

        switch (token.kind) {
        case TOKEN_DOT: {
            if (flags & PF_CONSTANT_EXPR) {
                error_full(ERROR, token.pos, "Unexpected member access in constant expression");
                exit(1);
            }

            NodeMember *member = node_alloc(p, NODE_MEMBER, lexer_expect(&p->lexer, TOKEN_IDENT));
            member->lhs = node;
            member->package = p->packages->current;

            if (lexer_read(&p->lexer, TOKEN_SCOPE)) {
                lexer_expect(&p->lexer, TOKEN_LT);
                parse_generics(p, &member->generics, &member->generics_count);
            }

            node = (Node *) member;
        } break;

        case TOKEN_LPAREN: {
            if (flags & PF_CONSTANT_EXPR) {
                error_full(ERROR, token.pos, "Unexpected call in constant expression");
                exit(1);
            }

            const size_t starting_row = token.pos.row;

            NodeCall *call = node_alloc(p, NODE_CALL, token);
            call->fn = node;
            if (call->fn->kind == NODE_ATOM) {
                ((NodeAtom *) call->fn)->will_be_called = true;
            } else if (call->fn->kind == NODE_MEMBER) {
                ((NodeMember *) call->fn)->will_be_called = true;
            }

            while (!lexer_read(&p->lexer, TOKEN_RPAREN)) {
                const bool fmt_newline = call->args.head && lexer_peek(&p->lexer).newlines > 1;

                nodes_push(&call->args, parse_expr(p, POWER_SET, PF_COMPOUND_ALLOWED));
                call->args.tail->fmt_newline = fmt_newline;

                call->arity++;

                token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN);
                if (token.kind != TOKEN_COMMA) {
                    break;
                }
            }

            call->rparen_pos = p->lexer.pos;
            call->rparen_pos.col--; // The lexer has already consumed the ')'

            if (p->lexer.pos.row != starting_row) {
                call->fmt_multiline = true;
            }

            node = (Node *) call;
        } break;

        case TOKEN_LBRACE:
            if (!(flags & PF_COMPOUND_ALLOWED) || !node_is_compound_literal_type(node)) {
                lexer_buffer(&p->lexer, token);
                return node;
            }

            if (flags & PF_CONSTANT_EXPR) {
                error_full(ERROR, token.pos, "Unexpected compound literal in constant expression");
                exit(1);
            }

            node = (Node *) parse_compound(p, node, token, flags);
            break;

        case TOKEN_LBRACKET: {
            NodeIndex *index = node_alloc(p, NODE_INDEX, token);
            index->base = node;

            if (lexer_peek(&p->lexer).kind != TOKEN_RANGE) {
                index->from = parse_expr(p, POWER_SET, flags | PF_COMPOUND_ALLOWED);
            }

            if (lexer_read(&p->lexer, TOKEN_RANGE)) {
                index->ranged = true;
                if (lexer_peek(&p->lexer).kind != TOKEN_RBRACKET) {
                    index->to = parse_expr(p, POWER_SET, flags | PF_COMPOUND_ALLOWED);
                }
            }

            lexer_expect(&p->lexer, TOKEN_RBRACKET);
            node = (Node *) index;
        } break;

        default: {
            NodeBinary *binary = node_alloc(p, NODE_BINARY, token);
            binary->lhs = node;
            binary->rhs = parse_expr(p, lbp, flags);
            node = (Node *) binary;

            if (lbp == POWER_SET) {
                return node;
            }
        } break;
        }
    }

    return node;
}

static void consume(Parser *p, TokenKind kind) {
    while (lexer_read(&p->lexer, kind));
}

static void local_assert(Parser *p, Token token, bool local) {
    if (p->local != local) {
        error_full(
            ERROR,
            token.pos,
            "Unexpected %s in %s scope",
            token_kind_to_cstr(token.kind),
            p->local ? "local" : "global");

        exit(1);
    }
}

static NodeAssert *parse_assert(Parser *p, Token token) {
    NodeAssert *assertt = node_alloc(p, NODE_ASSERT, token);
    assertt->expr = parse_expr(p, POWER_SET, PF_COMPOUND_ALLOWED);

    if (lexer_read(&p->lexer, TOKEN_COMMA)) {
        assertt->message = node_alloc(p, NODE_ATOM, lexer_expect(&p->lexer, TOKEN_STR));
    }
    return assertt;
}

static void do_import(Parser *p, Token token, SV as, ParseDirStd pds) {
    char *path_start = temp_alloc(0);
    if (p->root) {
        path_start = temp_sprintf("%s/", p->root);
        temp_remove_null();
    }

    char *path_buffer = temp_alloc(token.as.integer + 1);
    SV    path_sv = {.data = token.sv.data + 1, .count = token.sv.count - 2};

    resolve_escape_chars(path_buffer, &path_sv);
    path_buffer[token.as.integer] = '\0';

    const char *path = get_relative_path(p->cwd, path_start, p->arena);
    temp_reset(path_start);

    if (!p->formatter && !strcmp(path, ".")) {
        error_full(ERROR, token.pos, "Cannot import package 'main'");
        exit(1);
    }
    path_sv = sv_from_cstr(path);

    Package *previous = packages_find_by_path(*p->packages, path_sv);
    if (previous) {
        Import *previous_import = imports_find(p->packages->current->imports, previous->name.sv);
        if (previous_import && !p->formatter) {
            error_full(ERROR, token.pos, "Duplicate import of package '" SVFmt "'", SVArg(previous_import->as));
            fprintf(stderr, "\n");
            error_full(NOTE, previous_import->token.pos, "Imported here");
            exit(1);
        }

        Import *import = arena_alloc(p->arena, sizeof(*import));
        if (as.count) {
            import->as = as;
            import->aliased = true;
        } else {
            import->as = previous->name.sv;
        }

        import->token = token;
        import->package = previous;
        imports_push(&p->packages->current->imports, import);
        return;
    }

    Package *package = arena_alloc(p->arena, sizeof(*p->packages->current));
    package->path = path_sv;

    Package *packages_current_save = p->packages->current;
    packages_push(p->packages, package);

    if (!p->formatter) {
        ParseDirError pde = parse_dir(p, path, pds);
        if (pde == PDE_FAILED) {
            error_full(ERROR, token.pos, "Could not import package '%s'", path);
            exit(1);
        }

        if (pde == PDE_EMPTY) {
            error_full(ERROR, token.pos, "Directory '%s' does not contain any glos files", path);
            exit(1);
        }
    }

    if (sv_match(package->name.sv, "main")) {
        error_full(ERROR, token.pos, "Package 'main' is a separate program and cannot be imported");
        exit(1);
    }

    Import *import = arena_alloc(p->arena, sizeof(*import));
    import->as = as;
    import->token = token;
    import->package = package;

    if (import->as.count) {
        import->aliased = true;
    } else {
        import->as = package->name.sv;
    }

    p->packages->current = packages_current_save;
    if (!p->formatter) {
        Import *previous_import = imports_find(p->packages->current->imports, import->as);
        if (previous_import) {
            error_full(ERROR, token.pos, "Duplicate import of package '" SVFmt "'", SVArg(import->as));
            fprintf(stderr, "\n");
            error_full(NOTE, previous_import->token.pos, "Imported here");
            exit(1);
        }
    }

    imports_push(&p->packages->current->imports, import);
}

static_assert(COUNT_TOKENS == 69, "");
static Node *parse_stmt(Parser *p) {
    Node *node = NULL;

    Token      token = lexer_next(&p->lexer);
    const bool fmt_toplevel_newline = !p->local && token.newlines > 1;

    switch (token.kind) {
    case TOKEN_LBRACE: {
        local_assert(p, token, true);
        NodeBlock *block = node_alloc(p, NODE_BLOCK, token);
        while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
            const bool fmt_newline = block->body.head && lexer_peek(&p->lexer).newlines > 1;
            nodes_push(&block->body, parse_stmt(p));
            block->body.tail->fmt_newline = fmt_newline;
        }

        assert(p->lexer.buffer.kind == TOKEN_RBRACE);
        block->rbrace_pos = p->lexer.buffer.pos;

        node = (Node *) block;
    } break;

    case TOKEN_ASSERT:
        local_assert(p, token, true);
        node = (Node *) parse_assert(p, token);
        break;

    case TOKEN_IF: {
        local_assert(p, token, true);

        NodeIf *iff = node_alloc(p, NODE_IF, token);
        iff->condition = parse_expr(p, POWER_SET, 0);

        lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE));
        iff->consequence = parse_stmt(p);

        if (lexer_read(&p->lexer, TOKEN_ELSE)) {
            lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE, TOKEN_IF));
            iff->antecedence = parse_stmt(p);
        }

        node = (Node *) iff;
    } break;

    case TOKEN_FOR: {
        local_assert(p, token, true);

        NodeFor *forr = node_alloc(p, NODE_FOR, token);

        token = lexer_peek(&p->lexer);
        if (token.kind == TOKEN_VAR) {
            p->dont_consume_eols = true;
            forr->condition = parse_stmt(p);
            p->dont_consume_eols = false;
            lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_EOL));
        } else if (token.kind != TOKEN_LBRACE) {
            forr->condition = parse_expr(p, POWER_NIL, 0);
            if (forr->condition->kind == NODE_BINARY && token_kind_to_power(forr->condition->token.kind) == POWER_SET) {
                lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_EOL));
            }
        }

        if (lexer_read(&p->lexer, TOKEN_EOL)) {
            consume(p, TOKEN_EOL);
            forr->init = forr->condition;
            forr->condition = parse_expr(p, POWER_SET, 0);

            if (lexer_read(&p->lexer, TOKEN_EOL)) {
                consume(p, TOKEN_EOL);
                forr->update = parse_expr(p, POWER_NIL, 0);
            }
        }

        const bool in_loop_save = p->in_loop;
        p->in_loop = true;

        lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE));
        forr->body = parse_stmt(p);

        p->in_loop = in_loop_save;

        node = (Node *) forr;
    } break;

    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
        if (!p->in_loop) {
            error_full(ERROR, token.pos, "Unexpected %s outside loop", token_kind_to_cstr(token.kind));
            exit(1);
        }

        node = node_alloc(p, NODE_JUMP, token);
        break;

    case TOKEN_RETURN: {
        NodeReturn *ret = node_alloc(p, NODE_RETURN, token);

        token = lexer_peek(&p->lexer);
        if (!token.newlines && token.kind != TOKEN_EOL && token.kind != TOKEN_RBRACE) {
            ret->value = parse_expr(p, POWER_SET, PF_COMPOUND_ALLOWED);
        }

        node = (Node *) ret;
    } break;

    case TOKEN_FN: {
        const Token fn_token = token;

        token = lexer_expect(&p->lexer, TOKEN_IDENT, TOKEN_LPAREN);
        if (token.kind == TOKEN_LPAREN) {
            if (p->local) {
                error_full(ERROR, fn_token.pos, "Cannot define methods in local scope");
                exit(1);
            }

            NodeVar *self = node_alloc(p, NODE_VAR, lexer_expect(&p->lexer, TOKEN_IDENT));
            self->type = parse_type(p);

            lexer_expect(&p->lexer, TOKEN_RPAREN);
            node = parse_fn(p, lexer_expect(&p->lexer, TOKEN_IDENT));

            NodeFn *fn = (NodeFn *) node;
            self->node.next = fn->args.head;
            fn->args.head = (Node *) self;
            fn->arity++;
            fn->is_method = true;
        } else {
            node = parse_fn(p, token);
        }
    } break;

    case TOKEN_VAR: {
        NodeVar *var = node_alloc(p, NODE_VAR, lexer_expect(&p->lexer, TOKEN_IDENT));
        if (p->in_extern) {
            var->type = parse_type(p);
            var->is_extern = true;
        } else {
            token = lexer_peek(&p->lexer);
            if (token.kind != TOKEN_SET) {
                var->type = parse_type(p);
            }

            if (lexer_read(&p->lexer, TOKEN_SET)) {
                var->expr = parse_expr(p, POWER_SET, PF_COMPOUND_ALLOWED);
            }
        }

        if (p->local) {
            var->kind = NODE_VAR_LOCAL;
        } else {
            var->kind = NODE_VAR_GLOBAL;
        }

        var->package = p->packages->current;
        node = (Node *) var;
    } break;

    case TOKEN_TYPE: {
        NodeType *type = node_alloc(p, NODE_TYPE, lexer_expect(&p->lexer, TOKEN_IDENT));
        if (sv_match(type->node.token.sv, "_")) {
            error_full(ERROR, type->node.token.pos, "Cannot use '_' as name of type");
            exit(1);
        }

        if (lexer_read(&p->lexer, TOKEN_LT)) {
            do {
                nodes_push(&type->generics, node_alloc(p, NODE_TYPE, lexer_expect(&p->lexer, TOKEN_IDENT)));
                type->generics.tail->token.as.integer = type->generics_count++;
                token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_GT);
            } while (token.kind != TOKEN_GT);
        }

        type->local = p->local;
        type->definition = parse_type(p);
        type->package = p->packages->current;
        node = (Node *) type;
    } break;

    case TOKEN_CONST: {
        NodeConst *constt = node_alloc(p, NODE_CONST, lexer_expect(&p->lexer, TOKEN_IDENT));

        if (!lexer_read(&p->lexer, TOKEN_SET)) {
            constt->type = parse_type(p);
            lexer_expect(&p->lexer, TOKEN_SET);
        }

        constt->expr = parse_expr(p, POWER_SET, PF_COMPOUND_ALLOWED | PF_CONSTANT_EXPR);
        constt->local = p->local;
        constt->package = p->packages->current;
        node = (Node *) constt;
    } break;

    case TOKEN_STRUCT: {
        NodeStruct *structt = node_alloc(p, NODE_STRUCT, lexer_expect(&p->lexer, TOKEN_IDENT));
        if (sv_match(structt->node.token.sv, "_")) {
            error_full(ERROR, structt->node.token.pos, "Cannot use '_' as name of type");
            exit(1);
        }

        structt->local = p->local;

        token = lexer_expect(&p->lexer, TOKEN_LT, TOKEN_LBRACE);
        if (token.kind == TOKEN_LT) {
            do {
                nodes_push(&structt->generics, node_alloc(p, NODE_TYPE, lexer_expect(&p->lexer, TOKEN_IDENT)));
                structt->generics.tail->token.as.integer = structt->generics_count++;
                token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_GT);
            } while (token.kind != TOKEN_GT);

            lexer_expect(&p->lexer, TOKEN_LBRACE);
        }

        while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
            NodeField *field = node_alloc(p, NODE_FIELD, lexer_expect(&p->lexer, TOKEN_IDENT));
            if (structt->fields.head) {
                field->node.fmt_newline = field->node.token.newlines > 1;
            }

            field->type = parse_type(p);
            consume(p, TOKEN_COMMA);
            nodes_push(&structt->fields, (Node *) field);
        }

        if (!structt->fields.head) {
            assert(p->lexer.buffer.kind == TOKEN_RBRACE);
            error_full(ERROR, p->lexer.buffer.pos, "Empty structs are not allowed");
            exit(1);
        }

        structt->package = p->packages->current;
        node = (Node *) structt;
    } break;

    case TOKEN_EXTERN: {
        assert(!p->in_extern);
        p->in_extern = true;

        NodeExtern *externn = node_alloc(p, NODE_EXTERN, token);

        token = lexer_expect(&p->lexer, TOKEN_LBRACE, TOKEN_STR);
        while (token.kind == TOKEN_STR) {
            nodes_push(&externn->libraries, node_alloc(p, NODE_ATOM, token));
            token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_LBRACE);
            if (token.kind != TOKEN_COMMA) {
                break;
            }

            token = lexer_expect(&p->lexer, TOKEN_LBRACE, TOKEN_STR);
        }

        while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
            token = lexer_expect(&p->lexer, TOKEN_FN, TOKEN_VAR, TOKEN_LINK);
            lexer_buffer(&p->lexer, token);
            nodes_push(&externn->definitions, parse_stmt(p));
        }

        p->in_extern = false;
        node = (Node *) externn;
    } break;

    case TOKEN_STATIC: {
        token = lexer_expect(&p->lexer, TOKEN_VAR, TOKEN_ASSERT);
        if (token.kind == TOKEN_VAR) {
            lexer_buffer(&p->lexer, token);
            node = parse_stmt(p);
            ((NodeVar *) node)->is_static = true;
        } else if (token.kind == TOKEN_ASSERT) {
            NodeAssert *assertt = parse_assert(p, token);
            assertt->is_static = true;
            node = (Node *) assertt;
        } else {
            unreachable();
        }
    } break;

    case TOKEN_LINK: {
        if (!p->in_extern) {
            local_assert(p, token, false);
        }

        Node *link = node_alloc(p, NODE_ATOM, lexer_expect(&p->lexer, TOKEN_STR));
        bool  is_public = false;

        token = lexer_expect(&p->lexer, TOKEN_FN, TOKEN_VAR, TOKEN_PUB);
        if (token.kind == TOKEN_PUB) {
            is_public = true;
            token = lexer_expect(&p->lexer, TOKEN_FN, TOKEN_VAR);
        }
        lexer_buffer(&p->lexer, token);

        node = parse_stmt(p);
        if (node->kind == NODE_FN) {
            NodeFn *fn = (NodeFn *) node;
            fn->link = link;
            fn->is_public = is_public;

            if (fn->generics.head) {
                error_full(ERROR, node->token.pos, "Externally linked function cannot be generic");
                exit(1);
            }
        } else if (node->kind == NODE_VAR) {
            NodeVar *var = (NodeVar *) node;
            var->link = link;
            var->is_public = is_public;
        } else {
            unreachable();
        }
    } break;

    case TOKEN_PUB: {
        local_assert(p, token, false);
        lexer_buffer(
            &p->lexer,
            lexer_expect(&p->lexer, TOKEN_FN, TOKEN_VAR, TOKEN_TYPE, TOKEN_CONST, TOKEN_STRUCT, TOKEN_EXTERN));

        node = parse_stmt(p);
        switch (node->kind) {
        case NODE_FN:
            ((NodeFn *) node)->is_public = true;
            break;

        case NODE_VAR:
            ((NodeVar *) node)->is_public = true;
            break;

        case NODE_TYPE:
            ((NodeType *) node)->is_public = true;
            break;

        case NODE_CONST:
            ((NodeConst *) node)->is_public = true;
            break;

        case NODE_STRUCT:
            ((NodeStruct *) node)->is_public = true;
            break;

        case NODE_EXTERN:
            for (Node *it = ((NodeExtern *) node)->definitions.head; it; it = it->next) {
                if (it->kind == NODE_FN) {
                    ((NodeFn *) it)->is_public = true;
                } else if (it->kind == NODE_VAR) {
                    ((NodeVar *) it)->is_public = true;
                } else {
                    unreachable();
                }
            }
            break;

        default:
            unreachable();
        }
    } break;

    case TOKEN_IMPORT: {
        local_assert(p, token, false);

        token = lexer_expect(&p->lexer, TOKEN_STR, TOKEN_IDENT, TOKEN_LPAREN);
        if (token.kind == TOKEN_LPAREN) {
            while (!lexer_read(&p->lexer, TOKEN_RPAREN)) {
                token = lexer_expect(&p->lexer, TOKEN_STR, TOKEN_IDENT);

                SV as = {0};
                if (token.kind == TOKEN_IDENT) {
                    as = token.sv;
                    token = lexer_expect(&p->lexer, TOKEN_STR);
                }

                do_import(p, token, as, PDS_YES);
                lexer_read(&p->lexer, TOKEN_COMMA);
            }
        } else {
            SV as = {0};
            if (token.kind == TOKEN_IDENT) {
                as = token.sv;
                token = lexer_expect(&p->lexer, TOKEN_STR);
            }

            do_import(p, token, as, PDS_YES);
        }
    } break;

    case TOKEN_PRINT: {
        local_assert(p, token, true);
        NodePrint *print = node_alloc(p, NODE_PRINT, token);
        print->operand = parse_expr(p, POWER_SET, PF_COMPOUND_ALLOWED);
        node = (Node *) print;
    } break;

    default:
        local_assert(p, token, true);
        lexer_buffer(&p->lexer, token);
        node = parse_expr(p, POWER_NIL, PF_COMPOUND_ALLOWED);
        break;
    }

    if (!p->dont_consume_eols) {
        consume(p, TOKEN_EOL);
    }

    if (node) {
        node->fmt_toplevel_newline = fmt_toplevel_newline;
    }
    return node;
}

static Node *parse_fn(Parser *p, Token token) {
    NodeFn *fn = node_alloc(p, NODE_FN, token);
    fn->local = p->local;

    token = lexer_expect(&p->lexer, TOKEN_LPAREN, TOKEN_LT);
    if (token.kind == TOKEN_LT) {
        if (p->in_extern) {
            error_full(ERROR, fn->node.token.pos, "Externally linked function cannot be generic");
            exit(1);
        }

        if (fn->node.token.kind == TOKEN_FN) {
            error_full(ERROR, fn->node.token.pos, "Anonymous function cannot be generic");
            exit(1);
        }

        do {
            nodes_push(&fn->generics, node_alloc(p, NODE_TYPE, lexer_expect(&p->lexer, TOKEN_IDENT)));
            fn->generics.tail->token.as.integer = fn->generics_count++;
            token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_GT);
        } while (token.kind != TOKEN_GT);

        token = lexer_expect(&p->lexer, TOKEN_LPAREN);
    }
    const size_t starting_row = token.pos.row;

    const bool local_save = p->local;
    p->local = true;

    const bool in_loop_save = p->in_loop;
    p->in_loop = false;

    while (!lexer_read(&p->lexer, TOKEN_RPAREN)) {
        const bool fmt_newline = fn->args.head && lexer_peek(&p->lexer).newlines > 1;

        NodeVar *arg = node_alloc(p, NODE_VAR, lexer_expect(&p->lexer, TOKEN_IDENT));
        arg->kind = NODE_VAR_ARG;
        arg->type = parse_type(p);

        nodes_push(&fn->args, (Node *) arg);
        fn->args.tail->fmt_newline = fmt_newline;
        fn->arity++;

        token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN);
        if (token.kind != TOKEN_COMMA) {
            break;
        }
    }

    if (p->lexer.pos.row != starting_row) {
        fn->fmt_multiline = true;
    }

    token = lexer_peek(&p->lexer);
    if (!token.newlines && token_kind_is_start_of_type(token.kind)) {
        fn->ret = parse_type(p);
    }

    if (!p->in_extern) {
        lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE));
        fn->body = parse_stmt(p);
    }

    p->local = local_save;
    p->in_loop = in_loop_save;
    fn->package = p->packages->current;
    return (Node *) fn;
}

bool parse_file(Parser *p, const char *path) {
    assert(p->arena);
    if (!lexer_open(&p->lexer, path, p->arena)) {
        return false;
    }

    if (p->formatter) {
        p->lexer.comments = &p->formatter->comments;
    }

    lexer_expect(&p->lexer, TOKEN_PACKAGE);
    const Token name = lexer_expect(&p->lexer, TOKEN_IDENT);

    Package *package = p->packages->current;
    if (!sv_eq(name.sv, package->name.sv)) {
        if (!p->formatter && package->name.sv.count) {
            error_full(
                ERROR,
                name.pos,
                "Expected package '" SVFmt "', got '" SVFmt "'",
                SVArg(package->name.sv),
                SVArg(name.sv));

            if (package->name.pos.path) {
                fprintf(stderr, "\n");
                error_full(NOTE, package->name.pos, "Package first set here");
            }

            exit(1);
        }

        package->name = name;
    }

    if (!package->name.pos.path) {
        package->name.pos = name.pos;
    }

    Nodes   nodes = {0};
    Import *imports = p->packages->current->imports.tail;

    while (true) {
        consume(p, TOKEN_EOL);
        if (lexer_read(&p->lexer, TOKEN_EOF)) {
            break;
        }

        nodes_push(&nodes, parse_stmt(p));
    }

    if (p->formatter) {
        if (!imports) {
            imports = p->packages->current->imports.head;
        }

        if (!format_file(p->formatter, path, name, imports, nodes.head)) {
            p->formatter_failed = true;
            error_standalone(ERROR, "Could not format file '%s'", path);
        }
    } else {
        nodes_append(&package->nodes, &nodes);
    }
    return true;
}

ParseDirError parse_dir(Parser *p, const char *path, ParseDirStd pds) {
    assert(p->arena);

    const SV     suffix = sv_from_cstr(".glos");
    const size_t start = p->paths.count;
    if (pds == PDS_ONLY || !read_dir(&p->paths, p->cwd, path, suffix, p->arena)) {
        if (!pds) {
            return PDE_FAILED;
        }

        path = arena_sprintf(p->arena, "%s%s", p->std, path);
        if (!read_dir(&p->paths, p->cwd, path, suffix, p->arena)) {
            return PDE_FAILED;
        }
    }
    p->packages->current->real_path = path;

    bool empty = true;

    const Lexer lexer_save = p->lexer;
    for (size_t i = start; i < p->paths.count; i++) {
        const char *it = p->paths.data[i];
        if (is_dir(it)) {
            continue;
        }

        empty = false;
        if (!parse_file(p, it)) {
            error_standalone(ERROR, "Could not read file '%s'", it);
            exit(1);
        }
    }

    p->paths.count = start;
    p->lexer = lexer_save;

    if (empty) {
        return PDE_EMPTY;
    }
    return PDE_NONE;
}

void parser_load_builtin(Parser *p) {
    Token builtin = {0};
    builtin.sv = sv_from_cstr("\"builtin\"");
    builtin.as.integer = strlen("builtin");
    do_import(p, builtin, (SV) {0}, PDS_ONLY);
}
