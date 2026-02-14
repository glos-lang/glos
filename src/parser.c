#include "parser.h"
#include <stdbool.h>

typedef enum {
    POWER_NIL,
    POWER_SET,
    POWER_CMP,
    POWER_ADD,
    POWER_MUL,
    POWER_PRE,
    POWER_CALL
} Power;

static_assert(COUNT_TOKENS == 29, "");
static Power token_kind_to_power(Token_Kind kind) {
    switch (kind) {
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

static_assert(COUNT_AST_NODES == 11, "");
static AST_Node *ast_node_alloc(Parser *p, AST_Node_Kind kind, Token token) {
    static const size_t sizes[COUNT_AST_NODES] = {
        [AST_NODE_ATOM] = sizeof(AST_Node_Atom), // Prevent clang-format from messing this up
        [AST_NODE_UNARY] = sizeof(AST_Node_Unary),
        [AST_NODE_BINARY] = sizeof(AST_Node_Binary),

        [AST_NODE_FN] = sizeof(AST_Node_Fn),
        [AST_NODE_CALL] = sizeof(AST_Node_Call),

        [AST_NODE_DEFINE] = sizeof(AST_Node_Define),
        [AST_NODE_BLOCK] = sizeof(AST_Node_Block),
        [AST_NODE_IF] = sizeof(AST_Node_If),
        [AST_NODE_FOR] = sizeof(AST_Node_For),
        [AST_NODE_JUMP] = sizeof(AST_Node_Jump),

        [AST_NODE_PRINT] = sizeof(AST_Node_Print),
    };

    assert(kind >= AST_NODE_ATOM && kind < COUNT_AST_NODES);
    AST_Node *node = arena_alloc(p->arena, sizes[kind]);
    node->kind = kind;
    node->token = token;
    return node;
}

static AST_Node *parse_expr(Parser *p, Power mbp);
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
    iff->condition = parse_expr(p, POWER_SET);

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
        forr->condition = parse_expr(p, POWER_NIL);
        if (forr->condition->kind == AST_NODE_DEFINE ||
            (forr->condition->kind == AST_NODE_BINARY &&
             token_kind_to_power(forr->condition->token.kind) == POWER_SET)) {
            buffer_token(p, expect_token(p, TOKEN_EOL));
        }

        if (read_token(p, TOKEN_EOL)) {
            consume_tokens(p, TOKEN_EOL);
            forr->init = forr->condition;
            forr->condition = parse_expr(p, POWER_SET);

            if (read_token(p, TOKEN_EOL)) {
                consume_tokens(p, TOKEN_EOL);
                forr->update = parse_expr(p, POWER_NIL);
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

static_assert(COUNT_TOKENS == 29, "");
static AST_Node *parse_expr(Parser *p, Power mbp) {
    AST_Node *node = NULL;
    Token     token = next_token(p);

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_IDENT:
        node = ast_node_alloc(p, AST_NODE_ATOM, token);
        break;

    case TOKEN_SUB:
    case TOKEN_LNOT: {
        node = ast_node_alloc(p, AST_NODE_UNARY, token);
        AST_Node_Unary *unary = (AST_Node_Unary *) node;
        unary->value = parse_expr(p, POWER_PRE);
    } break;

    case TOKEN_LPAREN: {
        bool fn = false;
        if (read_token(p, TOKEN_RPAREN)) {
            fn = true;
        } else {
            node = parse_expr(p, POWER_SET);
            expect_token(p, TOKEN_RPAREN);
        }

        if (fn) {
            node = ast_node_alloc(p, AST_NODE_FN, token);
            AST_Node_Fn *fn = (AST_Node_Fn *) node;

            const bool is_local_save = p->is_local;
            p->is_local = true;
            {
                token = expect_token(p, TOKEN_LBRACE);
                fn->body = parse_block(p, token);
            }
            p->is_local = is_local_save;
        }
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
        case TOKEN_COLON:
            if (node->kind == AST_NODE_ATOM && node->token.kind == TOKEN_IDENT) {
                AST_Node_Define *define = (AST_Node_Define *) ast_node_alloc(p, AST_NODE_DEFINE, token);
                define->name = node;
                define->is_local = p->is_local;

                token = peek_token(p);
                if (token.kind != TOKEN_SET && token.kind != TOKEN_COLON) {
                    define->type = parse_expr(p, POWER_PRE);
                }

                if (read_token(p, TOKEN_SET)) {
                    define->expr = parse_expr(p, POWER_SET);
                } else if (read_token(p, TOKEN_COLON)) {
                    define->expr = parse_expr(p, POWER_SET);
                    define->is_const = true;
                }
                return (AST_Node *) define;
            } else {
                error_unexpected(token);
            }
            break;

        case TOKEN_LPAREN: {
            AST_Node_Call *call = (AST_Node_Call *) ast_node_alloc(p, AST_NODE_CALL, token);
            call->fn = node;

            expect_token(p, TOKEN_RPAREN);
            node = (AST_Node *) call;
        } break;

        default: {
            AST_Node_Binary *binary = (AST_Node_Binary *) ast_node_alloc(p, AST_NODE_BINARY, token);
            binary->lhs = node;
            binary->rhs = parse_expr(p, lbp);
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
    if (p->is_local != expected_is_local) {
        if (!label) {
            label = token_kind_to_cstr(token.kind);
        }

        fprintf(
            stderr,
            Pos_Fmt "ERROR: Unexpected %s in %s scope\n",
            Pos_Arg(token.pos),
            label,
            p->is_local ? "local" : "file");

        exit(1);
    }
}

static_assert(COUNT_AST_NODES == 11, "");
static AST_Node *parse_stmt(Parser *p) {
    AST_Node *node = NULL;

    Token token = next_token(p);
    switch (token.kind) {
    case TOKEN_LBRACE:
        local_assert(p, true, token, NULL);
        node = parse_block(p, token);
        break;

    case TOKEN_IF:
        local_assert(p, true, token, NULL);
        node = parse_if(p, token);
        break;

    case TOKEN_FOR:
        local_assert(p, true, token, NULL);
        node = parse_for(p, token);
        break;

    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
        if (!p->in_loop) {
            error_unexpected(token);
        }

        node = ast_node_alloc(p, AST_NODE_JUMP, token);
        break;

    case TOKEN_PRINT: {
        local_assert(p, true, token, NULL);
        AST_Node_Print *print = (AST_Node_Print *) ast_node_alloc(p, AST_NODE_PRINT, token);
        print->value = parse_expr(p, POWER_SET);
        node = (AST_Node *) print;
        break;
    }

    default:
        buffer_token(p, token);
        node = parse_expr(p, POWER_NIL);
        if (node->kind != AST_NODE_DEFINE) {
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
