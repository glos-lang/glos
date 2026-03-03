#include "parser.h"
#include "ast.h"
#include "basic.h"

typedef enum {
    POWER_NIL,
    POWER_SET,
    POWER_CMP,
    POWER_SHL,
    POWER_ADD,
    POWER_BOR,
    POWER_MUL,
    POWER_PRE,
    POWER_CALL,
    POWER_REF,
    POWER_DOT,
} Power;

static_assert(COUNT_TOKENS == 41, "");
static Power token_kind_to_power(Token_Kind kind) {
    switch (kind) {
    case TOKEN_DOT:
    case TOKEN_LBRACE:
        return POWER_DOT;

    case TOKEN_COLON:
        return POWER_SET;

    case TOKEN_LPAREN:
        return POWER_CALL;

    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_MOD:
        return POWER_MUL;

    case TOKEN_SHL:
    case TOKEN_SHR:
        return POWER_SHL;

    case TOKEN_BOR:
    case TOKEN_BAND:
        return POWER_BOR;

    case TOKEN_SET:
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

static void error_unexpected(Token token) {
    fprintf(stderr, Pos_Fmt "ERROR: Unexpected %s\n", Pos_Arg(token.pos), token_kind_to_cstr(token.kind));
    exit(1);
}

static void buffer_token(Parser *p, Token token) {
    p->peeked = true;
    p->ahead = token;
}

static Token next_token(Parser *p) {
    if (p->peeked) {
        p->peeked = false;
        return p->ahead;
    }

    return lexer_iter(&p->lexer);
}

static Token peek_token(Parser *p) {
    if (p->peeked) {
        return p->ahead;
    }

    buffer_token(p, lexer_iter(&p->lexer));
    return p->ahead;
}

static bool read_token(Parser *p, Token_Kind kind) {
    peek_token(p);
    p->peeked = p->ahead.kind != kind;
    return !p->peeked;
}

static Token expect_token(Parser *p, const Token_Kind *kinds) {
    const Token token = next_token(p);
    for (const Token_Kind *it = kinds; *it != TOKEN_EOF; it++) {
        if (token.kind == *it) {
            return token;
        }
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected ", Pos_Arg(token.pos));
    for (const Token_Kind *it = kinds; *it != TOKEN_EOF; it++) {
        if (it != kinds) {
            fprintf(stderr, " or ");
        }

        fprintf(stderr, "%s", token_kind_to_cstr(*it));
    }

    fprintf(stderr, ", got %s\n", token_kind_to_cstr(token.kind));
    exit(1);
}
#define expect_token(p, ...) expect_token((p), (const Token_Kind[]) {__VA_ARGS__, TOKEN_EOF})

static void consume_tokens(Parser *p, Token_Kind kind) {
    while (read_token(p, kind));
}

static_assert(COUNT_AST_NODES == 16, "");
static AST_Node *ast_node_alloc(Parser *p, AST_Node_Kind kind, Token token) {
    static const size_t sizes[COUNT_AST_NODES] = {
        [AST_NODE_ATOM] = sizeof(AST_Node_Atom), // Prevent clang-format from messing this up
        [AST_NODE_UNARY] = sizeof(AST_Node_Unary),
        [AST_NODE_BINARY] = sizeof(AST_Node_Binary),
        [AST_NODE_MEMBER] = sizeof(AST_Node_Member),

        [AST_NODE_FN] = sizeof(AST_Node_Fn),
        [AST_NODE_STRUCT] = sizeof(AST_Node_Struct),
        [AST_NODE_COMPOUND] = sizeof(AST_Node_Compound),

        [AST_NODE_CALL] = sizeof(AST_Node_Call),

        [AST_NODE_DEFINE] = sizeof(AST_Node_Define),
        [AST_NODE_BLOCK] = sizeof(AST_Node_Block),
        [AST_NODE_IF] = sizeof(AST_Node_If),
        [AST_NODE_FOR] = sizeof(AST_Node_For),

        [AST_NODE_JUMP] = sizeof(AST_Node_Jump),
        [AST_NODE_RETURN] = sizeof(AST_Node_Return),

        [AST_NODE_EXTERN] = sizeof(AST_Node_Extern),

        [AST_NODE_PRINT] = sizeof(AST_Node_Print),
    };

    assert(kind >= AST_NODE_ATOM && kind < COUNT_AST_NODES);
    AST_Node *node = arena_alloc(p->arena, sizes[kind]);
    node->kind = kind;
    node->token = token;
    return node;
}

static AST_Node *parse_expr(Parser *p, Power mbp, bool are_compounds_allowed);
static AST_Node *parse_stmt(Parser *p);

static AST_Node *parse_block(Parser *p, Token token) {
    AST_Node_Block *block = (AST_Node_Block *) ast_node_alloc(p, AST_NODE_BLOCK, token);
    while (!read_token(p, TOKEN_RBRACE)) {
        ast_nodes_push(&block->body, parse_stmt(p));
    }

    assert(p->ahead.kind == TOKEN_RBRACE);
    block->end = p->ahead.pos;
    return (AST_Node *) block;
}

static AST_Node *parse_if(Parser *p, Token token) {
    AST_Node_If *iff = (AST_Node_If *) ast_node_alloc(p, AST_NODE_IF, token);
    iff->condition = parse_expr(p, POWER_SET, false);

    token = expect_token(p, TOKEN_LBRACE);
    iff->consequence = parse_block(p, token);

    if (read_token(p, TOKEN_ELSE)) {
        token = expect_token(p, TOKEN_LBRACE, TOKEN_IF);
        if (token.kind == TOKEN_LBRACE) {
            iff->antecedence = parse_block(p, token);
        } else {
            iff->antecedence = parse_if(p, token);
        }
    }

    return (AST_Node *) iff;
}

static AST_Node *parse_for(Parser *p, Token token) {
    AST_Node_For *forr = (AST_Node_For *) ast_node_alloc(p, AST_NODE_FOR, token);

    if (peek_token(p).kind != TOKEN_LBRACE) {
        forr->condition = parse_expr(p, POWER_NIL, false);
        if (forr->condition->kind == AST_NODE_DEFINE ||
            (forr->condition->kind == AST_NODE_BINARY &&
             token_kind_to_power(forr->condition->token.kind) == POWER_SET)) {
            buffer_token(p, expect_token(p, TOKEN_EOL));
        }

        if (read_token(p, TOKEN_EOL)) {
            consume_tokens(p, TOKEN_EOL);
            forr->init = forr->condition;
            forr->condition = parse_expr(p, POWER_SET, false);

            if (read_token(p, TOKEN_EOL)) {
                consume_tokens(p, TOKEN_EOL);
                forr->update = parse_expr(p, POWER_NIL, false);
            }
        }
    }

    const bool inside_loop_save = p->in_loop;
    p->in_loop = true;
    {
        token = expect_token(p, TOKEN_LBRACE);
        forr->body = parse_block(p, token);
    }
    p->in_loop = inside_loop_save;
    return (AST_Node *) forr;
}

static void not_in_extern_assert(Parser *p, Token token) {
    if (p->in_extern) {
        fprintf(
            stderr,
            Pos_Fmt "ERROR: Extern blocks can only have variable and function definitions\n",
            Pos_Arg(token.pos));
        exit(1);
    }
}

static void definition_atom_setup(Parser *p, AST_Node_Define *define) {
    const bool is_assignment_const = define->expr && (define->is_const || p->fn_current == NULL);

    if (define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT) {
        AST_Node_Atom *it = (AST_Node_Atom *) define->name;
        AST_Node      *it_expr = define->expr;

        it->is_const = define->is_const;
        it->is_local = p->fn_current != NULL;
        it->is_extern = p->in_extern;
        it->is_assigned = define->expr != NULL;
        it->definition_node = define;
        it->assignment_node = define->expr;
        it->is_assignment_const = is_assignment_const;

        if (it->is_const) {
            assert(it_expr);
            if (it_expr->kind == AST_NODE_FN) {
                ((AST_Node_Fn *) it_expr)->defined_as = it;
            } else if (it_expr->kind == AST_NODE_STRUCT) {
                ((AST_Node_Struct *) it_expr)->defined_as = it;
            }
        }
    } else {
        unreachable();
    }
}

static_assert(COUNT_TOKENS == 41, "");
static AST_Node *parse_expr(Parser *p, Power mbp, bool are_compounds_allowed) {
    AST_Node *node = NULL;
    Token     token = next_token(p);

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_IDENT:
        node = ast_node_alloc(p, AST_NODE_ATOM, token);
        break;

    case TOKEN_SUB:
    case TOKEN_MUL:
    case TOKEN_BNOT:
    case TOKEN_LNOT: {
        node = ast_node_alloc(p, AST_NODE_UNARY, token);
        AST_Node_Unary *unary = (AST_Node_Unary *) node;
        unary->value = parse_expr(p, POWER_PRE, are_compounds_allowed);
    } break;

    case TOKEN_BAND: {
        node = ast_node_alloc(p, AST_NODE_UNARY, token);
        AST_Node_Unary *unary = (AST_Node_Unary *) node;
        unary->value = parse_expr(p, POWER_REF, are_compounds_allowed);
    } break;

    case TOKEN_LPAREN: {
        bool is_fn = false;
        if (read_token(p, TOKEN_RPAREN)) {
            is_fn = true;
        } else {
            node = parse_expr(p, POWER_NIL, true);
            if (node->kind == AST_NODE_DEFINE) {
                is_fn = true;
            } else if (node->kind == AST_NODE_BINARY && token_kind_to_power(node->token.kind) == POWER_SET) {
                error_unexpected(node->token);
            } else {
                expect_token(p, TOKEN_RPAREN);
            }
        }

        if (is_fn) {
            AST_Node *arg = node;
            node = ast_node_alloc(p, AST_NODE_FN, token);

            AST_Node_Fn *fn = (AST_Node_Fn *) node;
            fn->outer_fn = p->fn_current;

            AST_Node_Fn *fn_current_save = p->fn_current;
            p->fn_current = fn;

            if (arg) {
                definition_atom_setup(p, (AST_Node_Define *) arg);
            }

            while (arg) {
                AST_Node_Define *define = (AST_Node_Define *) arg;
                if (define->is_const) {
                    fprintf(
                        stderr, Pos_Fmt "ERROR: Expected argument, got constant definition\n", Pos_Arg(arg->token.pos));
                    exit(1);
                }

                if (define->expr) {
                    fprintf(
                        stderr, Pos_Fmt "ERROR: Argument definition cannot have assignment\n", Pos_Arg(arg->token.pos));
                    exit(1);
                }

                ast_nodes_push(&fn->args, arg);
                fn->args_count += define->count;

                if (expect_token(p, TOKEN_COMMA, TOKEN_RPAREN).kind != TOKEN_COMMA) {
                    break;
                }

                arg = parse_expr(p, POWER_NIL, true);
                if (arg->kind != AST_NODE_DEFINE) {
                    fprintf(stderr, Pos_Fmt "ERROR: Expected argument, got expression\n", Pos_Arg(arg->token.pos));
                    exit(1);
                }
            }

            if (read_token(p, TOKEN_ARROW)) {
                fn->returnn = parse_expr(p, POWER_PRE, false);
            }

            if (peek_token(p).kind == TOKEN_LBRACE) {
                fn->body = parse_block(p, next_token(p));
            } else {
                fn->is_type = true;
            }

            p->fn_current = fn_current_save;
        }
    } break;

    case TOKEN_STRUCT: {
        node = ast_node_alloc(p, AST_NODE_STRUCT, token);
        AST_Node_Struct *structt = (AST_Node_Struct *) node;

        expect_token(p, TOKEN_LBRACE);
        while (!read_token(p, TOKEN_RBRACE)) {
            AST_Node *field = parse_expr(p, POWER_NIL, true);
            if (field->kind != AST_NODE_DEFINE) {
                fprintf(stderr, Pos_Fmt "ERROR: Expected field, got expression\n", Pos_Arg(field->token.pos));
                exit(1);
            }

            AST_Node_Define *define = (AST_Node_Define *) field;
            if (define->is_const) {
                fprintf(stderr, Pos_Fmt "ERROR: Expected field, got constant definition\n", Pos_Arg(field->token.pos));
                exit(1);
            }

            if (define->expr) {
                fprintf(stderr, Pos_Fmt "ERROR: Field definition cannot have assignment\n", Pos_Arg(field->token.pos));
                exit(1);
            }

            ast_nodes_push(&structt->fields, field);
            structt->fields_count += define->count;

            if (expect_token(p, TOKEN_COMMA, TOKEN_RBRACE).kind != TOKEN_COMMA) {
                break;
            }
        }
    } break;

    case TOKEN_SIZEOF: {
        node = ast_node_alloc(p, AST_NODE_UNARY, token);
        AST_Node_Unary *unary = (AST_Node_Unary *) node;
        expect_token(p, TOKEN_LPAREN);
        unary->value = parse_expr(p, POWER_SET, true);
        expect_token(p, TOKEN_RPAREN);
    } break;

    default:
        error_unexpected(token);
    }

    while (true) {
        token = peek_token(p);
        if (token.newline) {
            break;
        }

        const Power lbp = token_kind_to_power(token.kind);
        if (lbp <= mbp) {
            break;
        }
        p->peeked = false;

        switch (token.kind) {
        case TOKEN_DOT: {
            AST_Node_Member *member = (AST_Node_Member *) ast_node_alloc(p, AST_NODE_MEMBER, token);
            member->lhs = node;
            member->field = expect_token(p, TOKEN_IDENT);
            node = (AST_Node *) member;
        } break;

        case TOKEN_COLON: {
            AST_Node_Define *define = (AST_Node_Define *) ast_node_alloc(p, AST_NODE_DEFINE, token);
            if (node->kind == AST_NODE_ATOM && node->token.kind == TOKEN_IDENT) {
                define->count = 1;
            } else {
                error_unexpected(token);
            }
            define->name = node;

            token = peek_token(p);
            if (token.kind != TOKEN_SET && token.kind != TOKEN_COLON) {
                define->type = parse_expr(p, POWER_PRE, true);
            }

            if (read_token(p, TOKEN_SET)) {
                if (p->in_extern) {
                    assert(p->ahead.kind == TOKEN_SET);
                    fprintf(stderr, Pos_Fmt "ERROR: External variable cannot have assignment\n", Pos_Arg(p->ahead.pos));
                    exit(1);
                }

                define->expr = parse_expr(p, POWER_SET, true);
            } else if (read_token(p, TOKEN_COLON)) {
                define->expr = parse_expr(p, POWER_SET, true);
                define->is_const = true;

                if (p->in_extern) {
                    if (define->expr->kind != AST_NODE_FN) {
                        not_in_extern_assert(p, define->expr->token);
                    }

                    AST_Node_Fn *fn = (AST_Node_Fn *) define->expr;
                    if (fn->body) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: External function cannot have body\n",
                            Pos_Arg(fn->body->token.pos));
                        exit(1);
                    }

                    fn->is_type = false;
                    fn->is_extern = true;
                }
            }

            definition_atom_setup(p, define);
            return (AST_Node *) define;
        } break;

        case TOKEN_LPAREN: {
            AST_Node_Call *call = (AST_Node_Call *) ast_node_alloc(p, AST_NODE_CALL, token);
            call->fn = node;

            while (!read_token(p, TOKEN_RPAREN)) {
                ast_nodes_push(&call->args, parse_expr(p, POWER_SET, true));
                if (expect_token(p, TOKEN_COMMA, TOKEN_RPAREN).kind != TOKEN_COMMA) {
                    break;
                }
            }

            assert(p->ahead.kind == TOKEN_RPAREN);
            call->end = p->ahead.pos;
            node = (AST_Node *) call;
        } break;

        case TOKEN_LBRACE: {
            if (!are_compounds_allowed) {
                buffer_token(p, token);
                return node;
            }

            AST_Node_Compound *compound = (AST_Node_Compound *) ast_node_alloc(p, AST_NODE_COMPOUND, token);
            compound->lhs = node;

            while (!read_token(p, TOKEN_RBRACE)) {
                ast_nodes_push(&compound->children, parse_expr(p, POWER_SET, true));
                if (expect_token(p, TOKEN_COMMA, TOKEN_RBRACE).kind != TOKEN_COMMA) {
                    break;
                }
            }

            node = (AST_Node *) compound;
        } break;

        default: {
            AST_Node_Binary *binary = (AST_Node_Binary *) ast_node_alloc(p, AST_NODE_BINARY, token);
            binary->lhs = node;
            binary->rhs = parse_expr(p, lbp, are_compounds_allowed);
            node = (AST_Node *) binary;
            if (lbp == POWER_SET) {
                return node;
            }
        } break;
        }
    }

    return node;
}

static void local_assert(Parser *p, bool expected_is_local, Token token, const char *label) {
    if ((p->fn_current != NULL) != expected_is_local) {
        if (!label) {
            label = token_kind_to_cstr(token.kind);
        }

        fprintf(
            stderr,
            Pos_Fmt "ERROR: Unexpected %s %s function\n",
            Pos_Arg(token.pos),
            label,
            p->fn_current ? "inside" : "outside");

        exit(1);
    }
}

static_assert(COUNT_AST_NODES == 16, "");
static AST_Node *parse_stmt(Parser *p) {
    AST_Node *node = NULL;

    Token token = next_token(p);
    switch (token.kind) {
    case TOKEN_LBRACE:
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        node = parse_block(p, token);
        break;

    case TOKEN_IF:
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        node = parse_if(p, token);
        break;

    case TOKEN_FOR:
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        node = parse_for(p, token);
        break;

    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
        not_in_extern_assert(p, token);
        if (!p->in_loop) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot use %s outside of a loop\n",
                Pos_Arg(token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        node = ast_node_alloc(p, AST_NODE_JUMP, token);
        break;

    case TOKEN_RETURN: {
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        node = ast_node_alloc(p, AST_NODE_RETURN, token);

        AST_Node_Return *returnn = (AST_Node_Return *) node;
        if (!peek_token(p).newline) {
            returnn->value = parse_expr(p, POWER_SET, true);
        }
    } break;

    case TOKEN_EXTERN: {
        if (p->in_extern) {
            fprintf(stderr, Pos_Fmt "ERROR: Cannot have nested externs\n", Pos_Arg(token.pos));
            exit(1);
        }

        node = ast_node_alloc(p, AST_NODE_EXTERN, token);
        AST_Node_Extern *externn = (AST_Node_Extern *) node;

        expect_token(p, TOKEN_LBRACE);
        p->in_extern = true;
        while (!read_token(p, TOKEN_RBRACE)) {
            ast_nodes_push(&externn->nodes, parse_stmt(p));
        }
        p->in_extern = false;
    } break;

    case TOKEN_PRINT: {
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        AST_Node_Print *print = (AST_Node_Print *) ast_node_alloc(p, AST_NODE_PRINT, token);
        print->value = parse_expr(p, POWER_SET, true);
        node = (AST_Node *) print;
        break;
    }

    default:
        buffer_token(p, token);
        node = parse_expr(p, POWER_NIL, true);
        if (node->kind != AST_NODE_DEFINE) {
            not_in_extern_assert(p, token);
            local_assert(p, true, node->token, "expression");
        }
        break;
    }

    consume_tokens(p, TOKEN_EOL);
    return node;
}

bool parse_file(Parser *p, const char *path) {
    assert(p->arena);
    if (!lexer_open(&p->lexer, path, p->arena)) {
        return false;
    }

    while (true) {
        consume_tokens(p, TOKEN_EOL);
        if (read_token(p, TOKEN_EOF)) {
            break;
        }

        ast_nodes_push(&p->nodes, parse_stmt(p));
    }
    return true;
}
