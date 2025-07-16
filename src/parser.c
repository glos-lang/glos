#include "parser.h"

static void nodes_push(Nodes *ns, Node *n) {
    if (ns->tail) {
        ns->tail->next = n;
        ns->tail = n;
    } else {
        ns->head = n;
        ns->tail = n;
    }
}

typedef enum {
    POWER_NIL,
    POWER_SET,
    POWER_LOR,
    POWER_CMP,
    POWER_SHL,
    POWER_ADD,
    POWER_BOR,
    POWER_MUL,
    POWER_PRE,
    POWER_DOT
} Power;

static_assert(COUNT_TOKENS == 56, "");
static Power token_kind_to_power(TokenKind kind) {
    switch (kind) {
    case TOKEN_DOT:
    case TOKEN_LPAREN:
    case TOKEN_LBRACE:
    case TOKEN_LBRACKET:
        return POWER_DOT;

    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
        return POWER_MUL;

    case TOKEN_SHL:
    case TOKEN_SHR:
        return POWER_SHL;

    case TOKEN_BOR:
    case TOKEN_BAND:
        return POWER_BOR;

    case TOKEN_LOR:
    case TOKEN_LAND:
        return POWER_LOR;

    case TOKEN_SET:
    case TOKEN_ADD_SET:
    case TOKEN_SUB_SET:
    case TOKEN_MUL_SET:
    case TOKEN_DIV_SET:
    case TOKEN_SHL_SET:
    case TOKEN_SHR_SET:
    case TOKEN_BOR_SET:
    case TOKEN_BAND_SET:
        return POWER_SET;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return POWER_CMP;

    default:
        return POWER_NIL;
    }
}

static_assert(COUNT_NODES == 22, "");
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
    fprintf(stderr, PosFmt "ERROR: Unexpected %s\n", PosArg(token.pos), token_kind_to_cstr(token.kind));
    exit(1);
}

static_assert(COUNT_TOKENS == 56, "");
static bool token_kind_is_start_of_type(TokenKind k) {
    switch (k) {
    case TOKEN_IDENT:
    case TOKEN_BAND:
    case TOKEN_LAND:
    case TOKEN_FN:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_TOKENS == 56, "");
static Node *parse_type(Parser *p) {
    Node *node = NULL;
    Token token = lexer_next(&p->lexer);

    switch (token.kind) {
    case TOKEN_IDENT:
        node = node_alloc(p, NODE_ATOM, token);
        break;

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
        NodeFn *fn = node_alloc(p, NODE_FN, token);

        lexer_expect(&p->lexer, TOKEN_LPAREN);
        while (!lexer_read(&p->lexer, TOKEN_RPAREN)) {
            NodeVar *arg = node_alloc(p, NODE_VAR, fn->node.token);
            arg->type = parse_type(p);

            nodes_push(&fn->args, (Node *) arg);
            fn->arity++;

            token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN);
            if (token.kind != TOKEN_COMMA) {
                break;
            }
        }

        token = lexer_peek(&p->lexer);
        if (!token.newline && token_kind_is_start_of_type(token.kind)) {
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

static_assert(COUNT_TOKENS == 56, "");
static Node *parse_expr(Parser *p, Power mbp, bool no_struct) {
    Node *node = NULL;
    Token token = lexer_next(&p->lexer);

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_IDENT:
        node = node_alloc(p, NODE_ATOM, token);
        break;

    case TOKEN_SUB:
    case TOKEN_MUL:
    case TOKEN_BAND:
    case TOKEN_BNOT:
    case TOKEN_LNOT: {
        NodeUnary *unary = node_alloc(p, NODE_UNARY, token);
        unary->operand = parse_expr(p, POWER_PRE, no_struct);
        node = (Node *) unary;
    } break;

    case TOKEN_SIZEOF: {
        NodeSizeof *sizeoff = node_alloc(p, NODE_SIZEOF, token);

        token = lexer_expect(&p->lexer, TOKEN_LPAREN, TOKEN_LT);
        if (token.kind == TOKEN_LPAREN) {
            sizeoff->expr = parse_expr(p, POWER_SET, false);
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
        cast->from = parse_expr(p, POWER_PRE, no_struct);

        node = (Node *) cast;
    } break;

    case TOKEN_LPAREN:
        node = parse_expr(p, POWER_SET, false);
        lexer_expect(&p->lexer, TOKEN_RPAREN);
        break;

    case TOKEN_FN:
        node = parse_fn(p, token);
        break;

    default:
        error_unexpected(token);
    }

    while (true) {
        token = lexer_peek(&p->lexer);
        if (token.newline) {
            break;
        }

        const Power lbp = token_kind_to_power(token.kind);
        if (lbp <= mbp) {
            break;
        }
        lexer_unbuffer(&p->lexer);

        switch (token.kind) {
        case TOKEN_DOT: {
            NodeMember *member = node_alloc(p, NODE_MEMBER, lexer_expect(&p->lexer, TOKEN_IDENT));
            member->lhs = node;
            node = (Node *) member;
        } break;

        case TOKEN_LPAREN: {
            NodeCall *call = node_alloc(p, NODE_CALL, token);
            call->fn = node;
            while (!lexer_read(&p->lexer, TOKEN_RPAREN)) {
                nodes_push(&call->args, parse_expr(p, POWER_SET, false));
                call->arity++;

                token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN);
                if (token.kind != TOKEN_COMMA) {
                    break;
                }
            }
            node = (Node *) call;
        } break;

        case TOKEN_LBRACE: {
            if (no_struct || !node_is_compound_literal_type(node)) {
                lexer_buffer(&p->lexer, token);
                return node;
            }

            NodeCompound *compound = node_alloc(p, NODE_COMPOUND, token);
            compound->type = node;

            typedef enum {
                COMPOUND_UNKNOWN,
                COMPOUND_ORDERED,
                COMPOUND_DESIGNATED,
            } CompoundKind;

            CompoundKind kind = COMPOUND_UNKNOWN;
            while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
                Node *expr = parse_expr(p, POWER_SET, false);

                token = lexer_peek(&p->lexer);
                if (token.kind == TOKEN_COLON) {
                    if (expr->kind != NODE_ATOM || expr->token.kind != TOKEN_IDENT) {
                        error_unexpected(token);
                    }
                    lexer_unbuffer(&p->lexer);

                    if (kind == COMPOUND_ORDERED) {
                        fprintf(
                            stderr,
                            PosFmt "ERROR: Cannot mix ordered and designated initializers\n",
                            PosArg(token.pos));

                        exit(1);
                    }
                    kind = COMPOUND_DESIGNATED;

                    NodeBinary *assign = node_alloc(p, NODE_BINARY, token);
                    assign->lhs = expr;
                    assign->rhs = parse_expr(p, POWER_SET, false);
                    nodes_push(&compound->nodes, (Node *) assign);
                } else {
                    if (kind == COMPOUND_DESIGNATED) {
                        fprintf(
                            stderr,
                            PosFmt "ERROR: Cannot mix ordered and designated initializers\n",
                            PosArg(expr->token.pos));

                        exit(1);
                    }
                    kind = COMPOUND_ORDERED;

                    nodes_push(&compound->nodes, expr);
                }

                token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_RBRACE);
                if (token.kind != TOKEN_COMMA) {
                    break;
                }
            }

            node = (Node *) compound;
        } break;

        case TOKEN_LBRACKET: {
            NodeIndex *index = node_alloc(p, NODE_INDEX, token);
            index->lhs = node;
            index->from = parse_expr(p, POWER_SET, false);
            if (lexer_read(&p->lexer, TOKEN_RANGE)) {
                index->to = parse_expr(p, POWER_SET, false);
            }

            lexer_expect(&p->lexer, TOKEN_RBRACKET);
            node = (Node *) index;
        } break;

        default: {
            NodeBinary *binary = node_alloc(p, NODE_BINARY, token);
            binary->lhs = node;
            binary->rhs = parse_expr(p, lbp, no_struct);
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
        fprintf(
            stderr,
            PosFmt "ERROR: Unexpected %s in %s scope\n",
            PosArg(token.pos),
            token_kind_to_cstr(token.kind),
            p->local ? "local" : "global");

        exit(1);
    }
}

static_assert(COUNT_TOKENS == 56, "");
static Node *parse_stmt(Parser *p) {
    Node *node = NULL;

    Token token = lexer_next(&p->lexer);
    switch (token.kind) {
    case TOKEN_LBRACE: {
        local_assert(p, token, true);
        NodeBlock *block = node_alloc(p, NODE_BLOCK, token);
        while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
            nodes_push(&block->body, parse_stmt(p));
        }

        assert(p->lexer.buffer.kind == TOKEN_RBRACE);
        block->node.token = p->lexer.buffer;

        node = (Node *) block;
    } break;

    case TOKEN_ASSERT: {
        local_assert(p, token, true);

        NodeAssert *assertt = node_alloc(p, NODE_ASSERT, token);
        assertt->expr = parse_expr(p, POWER_SET, false);

        node = (Node *) assertt;
    } break;

    case TOKEN_IF: {
        local_assert(p, token, true);

        NodeIf *iff = node_alloc(p, NODE_IF, token);
        iff->condition = parse_expr(p, POWER_SET, true);

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
            forr->condition = parse_expr(p, POWER_NIL, true);
            if (forr->condition->kind == NODE_BINARY && token_kind_to_power(forr->condition->token.kind) == POWER_SET) {
                lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_EOL));
            }
        }

        if (lexer_read(&p->lexer, TOKEN_EOL)) {
            consume(p, TOKEN_EOL);
            forr->init = forr->condition;
            forr->condition = parse_expr(p, POWER_SET, true);

            if (lexer_read(&p->lexer, TOKEN_EOL)) {
                consume(p, TOKEN_EOL);
                forr->update = parse_expr(p, POWER_NIL, true);
            }
        }

        lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE));
        forr->body = parse_stmt(p);

        node = (Node *) forr;
    } break;

    case TOKEN_RETURN: {
        NodeReturn *ret = node_alloc(p, NODE_RETURN, token);

        token = lexer_peek(&p->lexer);
        if (!token.newline && token.kind != TOKEN_EOL && token.kind != TOKEN_RBRACE) {
            ret->value = parse_expr(p, POWER_SET, false);
        }

        node = (Node *) ret;
    } break;

    case TOKEN_FN:
        node = parse_fn(p, lexer_expect(&p->lexer, TOKEN_IDENT));
        break;

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
                var->expr = parse_expr(p, POWER_SET, false);
            }
        }

        if (p->local) {
            var->kind = NODE_VAR_LOCAL;
        } else {
            var->kind = NODE_VAR_GLOBAL;
        }

        node = (Node *) var;
    } break;

    case TOKEN_TYPE: {
        NodeType *type = node_alloc(p, NODE_TYPE, lexer_expect(&p->lexer, TOKEN_IDENT));
        type->local = p->local;
        type->definition = parse_type(p);
        node = (Node *) type;
    } break;

    case TOKEN_CONST: {
        NodeConst *constt = node_alloc(p, NODE_CONST, lexer_expect(&p->lexer, TOKEN_IDENT));

        if (lexer_read(&p->lexer, TOKEN_SET)) {
            constt->expr = parse_expr(p, POWER_SET, false);
        } else {
            constt->type = parse_type(p);
            lexer_expect(&p->lexer, TOKEN_SET);
            constt->expr = parse_expr(p, POWER_SET, false);
        }

        constt->local = p->local;
        node = (Node *) constt;
    } break;

    case TOKEN_STRUCT: {
        NodeStruct *structt = node_alloc(p, NODE_STRUCT, lexer_expect(&p->lexer, TOKEN_IDENT));
        structt->local = p->local;

        lexer_expect(&p->lexer, TOKEN_LBRACE);
        while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
            NodeField *field = node_alloc(p, NODE_FIELD, lexer_expect(&p->lexer, TOKEN_IDENT));
            field->type = parse_type(p);
            consume(p, TOKEN_COMMA);
            nodes_push(&structt->fields, (Node *) field);
        }

        if (!structt->fields.head) {
            assert(p->lexer.buffer.kind == TOKEN_RBRACE);
            fprintf(stderr, PosFmt "ERROR: Empty structs are not allowed\n", PosArg(p->lexer.buffer.pos));
            exit(1);
        }

        node = (Node *) structt;
    } break;

    case TOKEN_EXTERN: {
        assert(!p->in_extern);
        p->in_extern = true;

        NodeExtern *externn = node_alloc(p, NODE_EXTERN, token);

        token = lexer_expect(&p->lexer, TOKEN_FN, TOKEN_VAR, TOKEN_LBRACE);
        switch (token.kind) {
        case TOKEN_FN:
        case TOKEN_VAR:
            lexer_buffer(&p->lexer, token);
            nodes_push(&externn->nodes, parse_stmt(p));
            break;

        case TOKEN_LBRACE:
            while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
                token = lexer_expect(&p->lexer, TOKEN_FN, TOKEN_VAR);
                lexer_buffer(&p->lexer, token);
                nodes_push(&externn->nodes, parse_stmt(p));
            }
            break;

        default:
            unreachable();
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
            NodeAssert *assertt = node_alloc(p, NODE_ASSERT, token);
            assertt->expr = parse_expr(p, POWER_SET, false);
            assertt->is_static = true;
            node = (Node *) assertt;
        } else {
            unreachable();
        }
    } break;

    case TOKEN_PRINT: {
        local_assert(p, token, true);
        NodePrint *print = node_alloc(p, NODE_PRINT, token);
        print->operand = parse_expr(p, POWER_SET, false);
        node = (Node *) print;
    } break;

    default:
        local_assert(p, token, true);
        lexer_buffer(&p->lexer, token);
        node = parse_expr(p, POWER_NIL, false);
        break;
    }

    if (!p->dont_consume_eols) {
        consume(p, TOKEN_EOL);
    }
    return node;
}

static Node *parse_fn(Parser *p, Token token) {
    NodeFn *fn = node_alloc(p, NODE_FN, token);
    fn->local = p->local;
    lexer_expect(&p->lexer, TOKEN_LPAREN);

    const bool local_save = p->local;
    p->local = true;

    while (!lexer_read(&p->lexer, TOKEN_RPAREN)) {
        NodeVar *arg = node_alloc(p, NODE_VAR, lexer_expect(&p->lexer, TOKEN_IDENT));
        arg->kind = NODE_VAR_ARG;
        arg->type = parse_type(p);

        nodes_push(&fn->args, (Node *) arg);
        fn->arity++;

        token = lexer_expect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN);
        if (token.kind != TOKEN_COMMA) {
            break;
        }
    }

    token = lexer_peek(&p->lexer);
    if (!token.newline && token_kind_is_start_of_type(token.kind)) {
        fn->ret = parse_type(p);
    }

    if (!p->in_extern) {
        lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE));
        fn->body = parse_stmt(p);
    }

    p->local = local_save;
    return (Node *) fn;
}

void parse_file(Parser *p, Lexer lexer) {
    assert(p->arena);

    p->lexer = lexer;
    while (true) {
        consume(p, TOKEN_EOL);
        if (lexer_read(&p->lexer, TOKEN_EOF)) {
            break;
        }

        nodes_push(&p->nodes, parse_stmt(p));
    }
}
