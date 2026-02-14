#include "compiler.h"

static_assert(COUNT_AST_TYPES == 5, "");
static void compile_type(Compiler *c, AST_Type *type) {
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

    case AST_TYPE_FN:
        if (type->llvm.kind != LLVM_TYPE_FN) {
            const size_t arity = type->spec.fn.arity;
            LLVM_Type   *args = arena_alloc(c->llvm.arena, arity * sizeof(*args));
            type->llvm = llvm_type_fn(args, arity);

            for (size_t i = 0; i < arity; i++) {
                AST_Type *it = &type->spec.fn.args[i];
                compile_type(c, it);
                args[i] = it->llvm;
            }
        }
        break;

    case AST_TYPE_TYPE:
        unreachable();
        break;

    default:
        unreachable();
        break;
    }
}

static LLVM_Node *compile_expr(Compiler *c, AST_Node *n, bool ref);
static void       compile_stmt(Compiler *c, AST_Node *n);

static LLVM_Node *compile_fn(Compiler *c, AST_Node_Fn *fn) {
    if (fn->llvm) {
        return fn->llvm;
    }

    LLVM_Node_Fn *llvm_fn_save = c->llvm.fn;
    {
        const char *name = NULL;
        if (fn->defined_as) {
            name = arena_sprintf(c->llvm.arena, "main." SV_Fmt, SV_Arg(fn->defined_as->node.token.sv));
        } else {
            name = arena_sprintf(c->llvm.arena, "main.anonymous.%zu", ++c->iota_fn);
        }

        compile_type(c, &fn->node.type);
        c->llvm.fn = llvm_fn_new(&c->llvm, sv_from_cstr(name), fn->node.type.llvm);
        fn->llvm = (LLVM_Node *) c->llvm.fn;
        llvm_fn_debug_set_pos(&c->llvm, c->llvm.fn, fn->node.token.pos.row, fn->node.token.pos.col);

        llvm_debug_scope_push(&c->llvm, fn->node.token.pos.row, fn->node.token.pos.col);
        {
            size_t arg_iota = 0;
            for (AST_Node *arg = fn->args.head; arg; arg = arg->next) {
                assert(arg->kind == AST_NODE_DEFINE);
                AST_Node_Define *define = (AST_Node_Define *) arg;

                assert(define->name->kind == AST_NODE_ATOM);
                AST_Node_Atom *it = (AST_Node_Atom *) define->name;

                LLVM_Node_Var *var = llvm_fn_arg_get(c->llvm.fn, arg_iota++);
                llvm_var_set_name(var, it->node.token.sv);
                llvm_var_debug_set_pos(&c->llvm, var, it->node.token.pos.row, it->node.token.pos.col);
                it->llvm = (LLVM_Node *) var;
            }

            assert(fn->body->kind == AST_NODE_BLOCK);
            AST_Node_Block *block = (AST_Node_Block *) fn->body;

            for (AST_Node *it = block->body.head; it; it = it->next) {
                compile_stmt(c, it);
            }
            llvm_fn_debug_set_return_pos(&c->llvm, c->llvm.fn, block->end.row, block->end.col);
        }
        llvm_debug_scope_pop(&c->llvm);
    }
    c->llvm.fn = llvm_fn_save;

    return fn->llvm;
}

static_assert(COUNT_AST_NODES == 11, "");
static LLVM_Node *compile_expr(Compiler *c, AST_Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    bool       debug = true;
    LLVM_Node *result = NULL;

    compile_type(c, &n->type);
    switch (n->kind) {
    case AST_NODE_ATOM: {
        AST_Node_Atom *atom = (AST_Node_Atom *) n;
        static_assert(COUNT_TOKENS == 30, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
        case TOKEN_INT:
            debug = false;
            return_defer(llvm_atom_int(&c->llvm, n->type.llvm, n->token.as.integer));

        case TOKEN_IDENT: {
            AST_Node_Atom *definition = (AST_Node_Atom *) atom->definition;
            assert(definition);

            if (definition->is_const) {
                debug = false;

                static_assert(COUNT_CONST_VALUES == 3, "");
                switch (definition->const_value.kind) {
                case CONST_VALUE_INT:
                    return_defer(llvm_atom_int(&c->llvm, n->type.llvm, definition->const_value.as.integer));
                    break;

                case CONST_VALUE_FN:
                    return_defer(compile_fn(c, definition->const_value.as.fn));
                    break;

                case CONST_VALUE_TYPE:
                    unreachable();
                    break;

                default:
                    unreachable();
                    break;
                }
            }

            if (ref) {
                debug = false;
                return_defer(definition->llvm);
            }

            return_defer(llvm_build_load(&c->llvm, definition->llvm, n->type.llvm));
        } break;

        default:
            unreachable();
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        LLVM_Node      *value = compile_expr(c, unary->value, false);

        static_assert(COUNT_TOKENS == 30, "");
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

        static_assert(COUNT_TOKENS == 30, "");
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
        if (op) {
            LLVM_Node *lhs = compile_expr(c, binary->lhs, false);
            LLVM_Node *rhs = compile_expr(c, binary->rhs, false);
            return_defer(llvm_build_binary(&c->llvm, op, n->type.llvm, lhs, rhs));
        }

        static_assert(COUNT_TOKENS == 30, "");
        switch (n->token.kind) {
        case TOKEN_SET: {
            LLVM_Node *lhs = compile_expr(c, binary->lhs, true);
            LLVM_Node *rhs = compile_expr(c, binary->rhs, false);
            return_defer(llvm_build_store(&c->llvm, lhs, rhs));
        }

        default:
            unreachable();
        }
    } break;

    case AST_NODE_FN:
        return compile_fn(c, (AST_Node_Fn *) n);

    case AST_NODE_CALL: {
        AST_Node_Call *call = (AST_Node_Call *) n;
        LLVM_Node    **args = arena_alloc(c->llvm.arena, call->arity * sizeof(*args));

        size_t iota = 0;
        for (AST_Node *arg = call->args.head; arg; arg = arg->next) {
            args[iota++] = compile_expr(c, arg, false);
        }

        assert(iota == call->arity);
        return_defer(llvm_build_call(&c->llvm, compile_expr(c, call->fn, false), args, iota));
    } break;

    default:
        unreachable();
    }

defer:
    assert(result);
    if (debug) {
        llvm_debug_set_pos(&c->llvm, result, n->token.pos.row, n->token.pos.col);
    }
    return result;
}

static void compile_var_init(Compiler *c, AST_Node_Atom *atom) {
    LLVM_Node_Var *var = (LLVM_Node_Var *) atom->llvm;

    static_assert(COUNT_CONST_VALUES == 3, "");
    switch (atom->const_value.kind) {
    case CONST_VALUE_INT:
        llvm_var_init_add_int(&c->llvm, var, atom->node.type.llvm, atom->const_value.as.integer);
        break;

    case CONST_VALUE_FN:
        llvm_var_init_add_node(&c->llvm, var, compile_fn(c, atom->const_value.as.fn));
        break;

    case CONST_VALUE_TYPE:
        unreachable();
        break;

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_AST_NODES == 11, "");
static void compile_stmt(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        if (define->is_const) {
            return;
        }

        assert(define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT);
        AST_Node_Atom *it = (AST_Node_Atom *) define->name;
        AST_Node      *it_expr = define->expr;

        if (!it->llvm) {
            compile_type(c, &it->node.type);

            LLVM_Node_Var *var =
                llvm_var_new(&c->llvm, it->node.token.sv, it->node.type.llvm, define->is_local, it_expr == NULL);

            llvm_var_debug_set_pos(&c->llvm, var, it->node.token.pos.row, it->node.token.pos.col);
            it->llvm = (LLVM_Node *) var;

            if (it_expr) {
                if (define->is_local) {
                    llvm_debug_set_pos(
                        &c->llvm,
                        llvm_build_store(&c->llvm, it->llvm, compile_expr(c, it_expr, false)),
                        n->token.pos.row,
                        n->token.pos.col);
                } else {
                    compile_var_init(c, it);
                }
            }
        }
    } break;

    case AST_NODE_BLOCK: {
        llvm_debug_scope_push(&c->llvm, n->token.pos.row, n->token.pos.col);

        AST_Node_Block *block = (AST_Node_Block *) n;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            compile_stmt(c, it);
        }

        llvm_debug_scope_pop(&c->llvm);
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
        LLVM_Node *condition = compile_expr(c, iff->condition, false);
        llvm_debug_set_pos(
            &c->llvm,
            llvm_build_branch(&c->llvm, condition, consequence, antecedence),
            n->token.pos.row,
            n->token.pos.col);

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

    case AST_NODE_FOR: {
        AST_Node_For *forr = (AST_Node_For *) n;
        if (forr->init) {
            llvm_debug_scope_push(&c->llvm, n->token.pos.row, n->token.pos.col);
            compile_stmt(c, forr->init);
        }

        LLVM_Node_Block *body = llvm_block_new(&c->llvm);
        LLVM_Node_Block *end = llvm_block_new(&c->llvm);

        LLVM_Node_Block *start = body;
        LLVM_Node_Block *update = start;
        if (forr->update) {
            update = llvm_block_new(&c->llvm);
        }

        LLVM_Node_Block *loop_break_save = c->loop_break;
        LLVM_Node_Block *loop_condition_save = c->loop_continue;
        c->loop_break = end;
        c->loop_continue = update;
        {
            // Condition
            if (forr->condition) {
                start = llvm_block_new(&c->llvm);
                llvm_build_jump(&c->llvm, start);
                llvm_build_block(&c->llvm, start);

                llvm_debug_set_pos(
                    &c->llvm,
                    llvm_build_branch(&c->llvm, compile_expr(c, forr->condition, false), body, end),
                    forr->condition->token.pos.row,
                    forr->condition->token.pos.col);
            } else {
                llvm_build_jump(&c->llvm, body);
            }

            // Body
            llvm_build_block(&c->llvm, body);
            compile_stmt(c, forr->body);

            // Update
            if (forr->update) {
                llvm_build_jump(&c->llvm, update);
                llvm_build_block(&c->llvm, update);
                compile_expr(c, forr->update, false);
            }

            // Loop
            llvm_build_jump(&c->llvm, start);

            // End
            llvm_build_block(&c->llvm, end);
        }
        c->loop_break = loop_break_save;
        c->loop_continue = loop_condition_save;

        if (forr->init) {
            llvm_debug_scope_pop(&c->llvm);
        }
    } break;

    case AST_NODE_JUMP:
        if (n->token.kind == TOKEN_BREAK) {
            llvm_build_jump(&c->llvm, c->loop_break);
        } else if (n->token.kind == TOKEN_CONTINUE) {
            llvm_build_jump(&c->llvm, c->loop_continue);
        } else {
            unreachable();
        }
        break;

    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        LLVM_Node      *value = compile_expr(c, print->value, false);
        llvm_debug_set_pos(&c->llvm, llvm_build_print(&c->llvm, value), n->token.pos.row, n->token.pos.col);
    } break;

    default:
        compile_expr(c, n, false);
        break;
    }
}

static AST_Node_Fn *get_main(Compiler *c) {
    AST_Node_Atom *main = scope_find(c->globals, sv_from_cstr("main"), 0);
    if (!main) {
        fprintf(
            stderr,
            "ERROR: Function 'main' is not defined\n"
            "\n"
            "+ main :: () {\n"
            "+ }\n");
        exit(1);
    }

    if (!main->is_const || main->const_value.kind != CONST_VALUE_FN) {
        fprintf(stderr, "ERROR: Identifier 'main' must be a constant function\n");
        exit(1);
    }

    return main->const_value.as.fn;
}

void compiler_build(Compiler *c, AST_Nodes nodes, const char *output) {
    unused(nodes);
    assert(c->cmd);
    assert(c->llvm.arena);

    llvm_debug_set_file(&c->llvm, c->path);

    AST_Node_Fn *fn = get_main(c);
    for (size_t i = 0; i < c->globals.count; i++) {
        AST_Node_Atom *it = c->globals.data[i];
        if (it->llvm) {
            continue;
        }

        if (it->is_const) {
            if (it->const_value.kind == CONST_VALUE_FN) {
                compile_fn(c, it->const_value.as.fn);
            }
        } else {
            compile_type(c, &it->node.type);

            LLVM_Node_Var *var = llvm_var_new(&c->llvm, it->node.token.sv, it->node.type.llvm, false, it->is_assigned);
            llvm_var_debug_set_pos(&c->llvm, var, it->node.token.pos.row, it->node.token.pos.col);
            it->llvm = (LLVM_Node *) var;

            if (it->is_assigned) {
                compile_var_init(c, it);
            }
        }
    }

    c->llvm.main_fn = llvm_fn_new(&c->llvm, sv_from_cstr("main"), llvm_type_fn(NULL, 0));
    c->llvm.fn = c->llvm.main_fn;
    llvm_build_call(&c->llvm, fn->llvm, NULL, 0);
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

    llvm_free(&c->llvm);
    da_free(&c->locals);
    da_free(&c->globals);

    const int code = cmd_wait(proc);
    if (code != 0) {
        fprintf(stderr, "ERROR: Process 'clang' exited abnormally with code %d\n", code);
        exit(1);
    }
}

// TODO: Prefix global variables with 'main' for consistency
