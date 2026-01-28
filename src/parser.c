#include "parser.h"

typedef enum {
    POWER_NIL,
    POWER_ADD,
    POWER_MUL,
    POWER_PRE
} Power;

static Power token_kind_to_power(Token_Kind kind) {
    switch (kind) {
    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_MOD:
        return POWER_MUL;

    default:
        return POWER_NIL;
    }
}

static void error_unexpected(Token token) {
    fprintf(stderr, Pos_Fmt "ERROR: Unexpected %s", Pos_Arg(token.pos), token_kind_to_cstr(token.kind));
    exit(1);
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

    p->peeked = true;
    p->ahead = lexer_iter(&p->lexer);
    return p->ahead;
}

static bool read_token(Parser *p, Token_Kind kind) {
    peek_token(p);
    p->peeked = p->ahead.kind != kind;
    return !p->peeked;
}

static void consume_tokens(Parser *p, Token_Kind kind) {
    while (read_token(p, kind));
}

static_assert(COUNT_AST_NODES == 4, "");
static AST_Node *ast_node_alloc(Parser *p, AST_Node_Kind kind, Token token) {
    static const size_t sizes[COUNT_AST_NODES] = {
        [AST_NODE_ATOM] = sizeof(AST_Node_Atom), // Prevent clang-format from messing this up
        [AST_NODE_UNARY] = sizeof(AST_Node_Unary),
        [AST_NODE_BINARY] = sizeof(AST_Node_Binary),

        [AST_NODE_PRINT] = sizeof(AST_Node_Print),
    };

    assert(kind >= AST_NODE_ATOM && kind < COUNT_AST_NODES);
    AST_Node *node = arena_alloc(p->arena, sizes[kind]);
    node->kind = kind;
    node->token = token;
    return node;
}

static_assert(COUNT_AST_NODES == 4, "");
static_assert(COUNT_TOKENS == 12, "");
static AST_Node *parse_expr(Parser *p, Power mbp) {
    AST_Node *node = NULL;
    Token     token = next_token(p);

    switch (token.kind) {
    case TOKEN_INT:
        node = ast_node_alloc(p, AST_NODE_ATOM, token);
        break;

    case TOKEN_SUB: {
        node = ast_node_alloc(p, AST_NODE_UNARY, token);
        AST_Node_Unary *unary = (AST_Node_Unary *) node;
        unary->value = parse_expr(p, POWER_PRE);
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

        AST_Node_Binary *binary = (AST_Node_Binary *) ast_node_alloc(p, AST_NODE_BINARY, token);
        binary->lhs = node;
        binary->rhs = parse_expr(p, lbp);
        node = (AST_Node *) binary;
    }

    return node;
}

static_assert(COUNT_AST_NODES == 4, "");
static AST_Node *parse_stmt(Parser *p) {
    AST_Node *node = NULL;

    Token token = next_token(p);
    switch (token.kind) {
    case TOKEN_PRINT: {
        node = ast_node_alloc(p, AST_NODE_PRINT, token);
        AST_Node_Print *print = (AST_Node_Print *) node;
        print->value = parse_expr(p, POWER_NIL);
    } break;

    default:
        node = parse_expr(p, POWER_NIL);
        break;
    }

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
