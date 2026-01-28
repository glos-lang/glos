#include "compiler.h"

static_assert(COUNT_AST_NODES == 4, "");
static LLVM_Node *compile_expr(Compiler *c, AST_Node *n) {
    if (!n) {
        return NULL;
    }

    LLVM_Node *result = NULL;

    switch (n->kind) {
    case AST_NODE_ATOM:
        return_defer(llvm_atom_int(&c->llvm, n->token.as.integer));

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        LLVM_Node      *value = compile_expr(c, unary->value);

        static_assert(COUNT_TOKENS == 12, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            return_defer(llvm_build_unary(&c->llvm, LLVM_UNARY_NEG, value));

        default:
            unreachable();
        }
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        LLVM_Node       *lhs = compile_expr(c, binary->lhs);
        LLVM_Node       *rhs = compile_expr(c, binary->rhs);

        static_assert(COUNT_TOKENS == 12, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
            return_defer(llvm_build_binary(&c->llvm, LLVM_BINARY_ADD, lhs, rhs));

        case TOKEN_SUB:
            return_defer(llvm_build_binary(&c->llvm, LLVM_BINARY_SUB, lhs, rhs));

        case TOKEN_MUL:
            return_defer(llvm_build_binary(&c->llvm, LLVM_BINARY_MUL, lhs, rhs));

        case TOKEN_DIV:
            return_defer(llvm_build_binary(&c->llvm, LLVM_BINARY_DIV, lhs, rhs));

        case TOKEN_MOD:
            return_defer(llvm_build_binary(&c->llvm, LLVM_BINARY_MOD, lhs, rhs));

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }

defer:
    if (result && n->kind != AST_NODE_ATOM) {
        llvm_debug_set_pos(&c->llvm, result, n->token.pos.row, n->token.pos.col);
    }
    return result;
}

static_assert(COUNT_AST_NODES == 4, "");
static void compile_stmt(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        LLVM_Node      *value = compile_expr(c, print->value);
        llvm_debug_set_pos(&c->llvm, llvm_build_print(&c->llvm, value), n->token.pos.row, n->token.pos.col);
    } break;

    default:
        compile_expr(c, n);
        break;
    }
}

void compiler_build(Compiler *c, AST_Nodes nodes, const char *output) {
    assert(c->cmd);
    assert(c->llvm.arena);

    llvm_debug_set_file(&c->llvm, c->path);
    for (AST_Node *it = nodes.head; it; it = it->next) {
        compile_stmt(c, it);
    }
    llvm_compile(&c->llvm);

#if 0
    fwrite(c->llvm.sb.data, c->llvm.sb.count, 1, stdout);
    sb_free(&c->llvm.sb);
    return;
#endif

    c->cmd->count = 0;
    cmd_push(c->cmd, "clang");
    cmd_push(c->cmd, "-Wno-override-module");
    cmd_push(c->cmd, "-o");
    cmd_push(c->cmd, output);
    cmd_push(c->cmd, "-x");
    cmd_push(c->cmd, "ir");
    cmd_push(c->cmd, "-");

    FILE *f = NULL;
    Proc  proc = cmd_run_async(c->cmd, (CmdStdio) {.in = &f});

    fwrite(c->llvm.sb.data, sizeof(char), c->llvm.sb.count, f);
    fclose(f);
    sb_free(&c->llvm.sb);

    const int code = cmd_wait(proc);
    if (code != 0) {
        fprintf(stderr, "ERROR: Process 'clang' exited abnormally with code %d\n", code);
        exit(1);
    }
}
