#include "compiler.h"

static_assert(COUNT_AST_TYPES == 3, "");
static void compile_type(AST_Type *type) {
    if (!type) {
        return;
    }

    switch (type->kind) {
    case AST_TYPE_UNIT:
        type->llvm = llvm_type_basic(LLVM_TYPE_I0);
        break;

    case AST_TYPE_BOOL:
        type->llvm = llvm_type_basic(LLVM_TYPE_I1);
        break;

    case AST_TYPE_I64:
        type->llvm = llvm_type_basic(LLVM_TYPE_I64);
        break;

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_AST_NODES == 6, "");
static LLVM_Node *compile_expr(Compiler *c, AST_Node *n) {
    if (!n) {
        return NULL;
    }

    LLVM_Node *result = NULL;

    compile_type(&n->type);
    switch (n->kind) {
    case AST_NODE_ATOM: {
        static_assert(COUNT_TOKENS == 24, "");
        return_defer(llvm_atom_int(&c->llvm, n->type.llvm, n->token.as.integer));
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        LLVM_Node      *value = compile_expr(c, unary->value);

        static_assert(COUNT_TOKENS == 24, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            return_defer(llvm_build_unary(&c->llvm, LLVM_UNARY_NEG, n->type.llvm, value));

        case TOKEN_LNOT:
            return_defer(llvm_build_unary(&c->llvm, LLVM_UNARY_LNOT, n->type.llvm, value));

        default:
            unreachable();
        }
    } break;

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;
        LLVM_Node       *lhs = compile_expr(c, binary->lhs);
        LLVM_Node       *rhs = compile_expr(c, binary->rhs);

        static_assert(COUNT_TOKENS == 24, "");
        static const LLVM_Binary_Kind ops[COUNT_TOKENS] = {
            [TOKEN_ADD] = LLVM_BINARY_ADD,
            [TOKEN_SUB] = LLVM_BINARY_SUB,
            [TOKEN_MUL] = LLVM_BINARY_MUL,
            [TOKEN_DIV] = LLVM_BINARY_DIV,
            [TOKEN_MOD] = LLVM_BINARY_MOD,

            [TOKEN_GT] = LLVM_BINARY_GT,
            [TOKEN_GE] = LLVM_BINARY_GE,
            [TOKEN_LT] = LLVM_BINARY_LT,
            [TOKEN_LE] = LLVM_BINARY_LE,
            [TOKEN_EQ] = LLVM_BINARY_EQ,
            [TOKEN_NE] = LLVM_BINARY_NE,
        };

        const LLVM_Binary_Kind op = ops[n->token.kind];
        assert(op);
        return_defer(llvm_build_binary(&c->llvm, op, n->type.llvm, lhs, rhs));
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

static_assert(COUNT_AST_NODES == 6, "");
static void compile_stmt(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_BLOCK: {
        AST_Node_Block *block = (AST_Node_Block *) n;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            compile_stmt(c, it);
        }
    } break;

    case AST_NODE_IF: {
        AST_Node_If *iff = (AST_Node_If *) n;

        LLVM_Node_Block *consequence = llvm_block_new(&c->llvm);
        LLVM_Node_Block *antecedence = llvm_block_new(&c->llvm);

        LLVM_Node_Block *end = antecedence;
        if (iff->antecedence) {
            end = llvm_block_new(&c->llvm);
        }

        // Condition
        LLVM_Node *condition = compile_expr(c, iff->condition);
        llvm_build_branch(&c->llvm, condition, consequence, antecedence);

        // Consequence
        llvm_build_block(&c->llvm, consequence);
        compile_stmt(c, iff->consequence);
        llvm_build_jump(&c->llvm, end);

        // Antecedence
        if (iff->antecedence) {
            llvm_build_block(&c->llvm, antecedence);
            compile_stmt(c, iff->antecedence);
            llvm_build_jump(&c->llvm, end);
        }

        // End
        llvm_build_block(&c->llvm, end);
    } break;

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
    exit(0);
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
    Proc  proc = cmd_run_async(c->cmd, (Cmd_Stdio) {.in = &f});
    if (proc == PROC_INVALID) {
        fprintf(stderr, "ERROR: Could not start process 'clang'\n");
        exit(1);
    }

    if (f) {
        fwrite(c->llvm.sb.data, sizeof(char), c->llvm.sb.count, f);
        fclose(f);
    }
    sb_free(&c->llvm.sb);

    const int code = cmd_wait(proc);
    if (code != 0) {
        fprintf(stderr, "ERROR: Process 'clang' exited abnormally with code %d\n", code);
        exit(1);
    }
}
