#include <ctype.h>
#include <stdarg.h>

#include "compiler.h"

typedef struct {
    char  *data;
    size_t count;
    size_t capacity;
} SB;

typedef struct {
    SB     sb;
    size_t indent;

    bool   local;
    size_t locals;
    size_t globals;

    Scope fns;
} Compiler;

static inline BackendData backend_data_new(Compiler *c) {
    BackendData data = {.local = c->local};
    if (c->local) {
        data.iota = ++c->locals;
    } else {
        data.iota = ++c->globals;
    }
    return data;
}

static PrintfLike(2) void compile_sprintf(Compiler *c, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    da_grow(&c->sb, n + 1);

    va_start(args, fmt);
    vsnprintf(&c->sb.data[c->sb.count], n + 1, fmt, args);
    c->sb.count += n;
    va_end(args);
}

static inline void compile_indent(Compiler *c) {
    for (size_t i = 0; i < c->indent; i++) {
        da_push(&c->sb, '\t');
    }
}

static inline void compile_quoted(Compiler *c, SV sv) {
    da_push(&c->sb, '"');
    for (size_t i = 0; i < sv.count; i++) {
        const char it = sv.data[i];
        if (it == '"') {
            compile_sprintf(c, "\\\"");
        } else if (isprint(it)) {
            compile_sprintf(c, "%c", it);
        } else {
            compile_sprintf(c, "\\x%x", it);
        }
    }
    da_push(&c->sb, '"');
}

static inline void compile_backend_data(Compiler *c, BackendData data) {
    if (data.local) {
        compile_sprintf(c, "glos_l%zu", data.iota);
    } else {
        compile_sprintf(c, "glos_g%zu", data.iota);
    }
}

static_assert(COUNT_NODES == 10, "");
static void compile_type(Compiler *c, Type type) {
    switch (type.kind) {
    case TYPE_UNIT:
        compile_sprintf(c, "void");
        break;

    case TYPE_BOOL:
        compile_sprintf(c, "bool");
        break;

    case TYPE_I64:
        compile_sprintf(c, "i64");
        break;

    case TYPE_FN:
        compile_backend_data(c, type.backend);
        break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 10, "");
static void compile_expr(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            compile_sprintf(c, "%zuL", n->token.as.integer);
            break;

        case TOKEN_BOOL:
            compile_sprintf(c, "%dL", n->token.as.boolean);
            break;

        case TOKEN_IDENT:
            compile_backend_data(c, atom->definition->backend);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        compile_expr(c, call->fn);
        compile_sprintf(c, "(");
        for (Node *it = call->args.head; it; it = it->next) {
            // TODO: This inherits the undefined order of call arguments from C
            //       Use "SSA" to define the evaluation from left to right
            compile_expr(c, it);
            if (it->next) {
                compile_sprintf(c, ", ");
            }
        }
        compile_sprintf(c, ")");
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            compile_sprintf(c, "-(");
            compile_expr(c, unary->operand);
            compile_sprintf(c, ")");
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
            compile_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sprintf(c, " + ");
            compile_expr(c, binary->rhs);
            compile_sprintf(c, ")");
            break;

        case TOKEN_SUB:
            compile_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sprintf(c, " - ");
            compile_expr(c, binary->rhs);
            compile_sprintf(c, ")");
            break;

        case TOKEN_MUL:
            compile_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sprintf(c, " * ");
            compile_expr(c, binary->rhs);
            compile_sprintf(c, ")");
            break;

        case TOKEN_DIV:
            compile_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sprintf(c, " / ");
            compile_expr(c, binary->rhs);
            compile_sprintf(c, ")");
            break;

        case TOKEN_SET:
            compile_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sprintf(c, " = ");
            compile_expr(c, binary->rhs);
            compile_sprintf(c, ")");
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_FN:
        da_push(&c->fns, n);
        compile_backend_data(c, n->backend);
        break;

    default:
        unreachable();
    }
}

static void compile_fn(Compiler *c, Node *n);

static_assert(COUNT_NODES == 10, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        compile_sprintf(c, "if (");
        compile_expr(c, iff->condition);
        compile_sprintf(c, ") ");
        compile_stmt(c, iff->consequence);

        if (iff->antecedence) {
            compile_sprintf(c, " else ");
            compile_stmt(c, iff->antecedence);
        }
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        compile_sprintf(c, "{\n");

        c->indent++;
        for (Node *it = block->body.head; it; it = it->next) {
            compile_sprintf(c, "#line %zu\n", it->token.pos.row + 1);
            compile_indent(c);
            compile_stmt(c, it);
            compile_sprintf(c, "\n");
        }
        c->indent--;

        compile_sprintf(c, "#line %zu\n", n->token.pos.row + 1);
        compile_indent(c);
        compile_sprintf(c, "}");
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        compile_sprintf(c, "return");
        if (ret->value) {
            compile_sprintf(c, " ");
            compile_expr(c, ret->value);
        }
        compile_sprintf(c, ";");
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        if (fn->local) {
            da_push(&c->fns, n);
        } else {
            compile_fn(c, n);
            while (c->fns.count) {
                compile_fn(c, c->fns.data[--c->fns.count]);
            }
        }
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (var->local) {
            compile_type(c, n->type);
            compile_sprintf(c, " ");

            n->backend = backend_data_new(c);
            compile_backend_data(c, n->backend);

            if (var->expr) {
                compile_sprintf(c, " = ");
                compile_expr(c, var->expr);
            }

            compile_sprintf(c, ";");
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        compile_sprintf(c, "printf(\"%%ld\\n\", (long) (");
        compile_expr(c, print->operand);
        compile_sprintf(c, "));");
    } break;

    default:
        compile_expr(c, n);
        compile_sprintf(c, ";");
        break;
    }
}

static void compile_fn(Compiler *c, Node *n) {
    NodeFn *fn = (NodeFn *) n;
    compile_sprintf(c, "\n#line %zu ", n->token.pos.row + 1);
    compile_quoted(c, sv_from_cstr(n->token.pos.path));

    compile_sprintf(c, "\nstatic ");
    compile_type(c, node_fn_return_type(fn));
    compile_sprintf(c, " ");
    compile_backend_data(c, n->backend);

    const bool local_save = c->local;
    c->local = true;
    c->locals = 0;

    compile_sprintf(c, "(");
    if (fn->args.head) {
        for (Node *it = fn->args.head; it; it = it->next) {
            compile_type(c, it->type);
            compile_sprintf(c, " ");

            it->backend = backend_data_new(c);
            compile_backend_data(c, it->backend);

            if (it->next) {
                compile_sprintf(c, ", ");
            }
        }
    } else {
        compile_sprintf(c, "void");
    }
    compile_sprintf(c, ") ");

    compile_stmt(c, fn->body);
    c->local = local_save;
    compile_sprintf(c, "\n");
}

static_assert(COUNT_NODES == 10, "");
static void pre_compile_type(Compiler *c, Type *type) {
    if (!type || type->backend.iota) {
        return;
    }

    switch (type->kind) {
    case TYPE_UNIT:
    case TYPE_BOOL:
    case TYPE_I64:
        break;

    case TYPE_FN: {
        NodeFn *spec = (NodeFn *) type->spec;
        for (Node *it = spec->args.head; it; it = it->next) {
            pre_compile_type(c, &it->type);
        }

        type->backend = backend_data_new(c);

        compile_sprintf(c, "typedef ");
        compile_type(c, node_fn_return_type(spec));
        compile_sprintf(c, " (*");
        compile_backend_data(c, type->backend);

        compile_sprintf(c, ")(");
        if (spec->args.head) {
            for (Node *it = spec->args.head; it; it = it->next) {
                compile_type(c, it->type);
                if (it->next) {
                    compile_sprintf(c, ", ");
                }
            }
        } else {
            compile_sprintf(c, "void");
        }
        compile_sprintf(c, ");\n");
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 10, "");
static void pre_compile_node(Compiler *c, Node *n) {
    if (!n || n->backend.iota) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM:
        break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        pre_compile_node(c, call->fn);

        for (Node *it = call->args.head; it; it = it->next) {
            pre_compile_node(c, it);
        }
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        pre_compile_node(c, unary->operand);
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;
        pre_compile_node(c, binary->lhs);
        pre_compile_node(c, binary->rhs);
    } break;

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        pre_compile_node(c, iff->condition);
        pre_compile_node(c, iff->consequence);
        pre_compile_node(c, iff->antecedence);
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            pre_compile_node(c, it);
        }
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        pre_compile_node(c, ret->value);
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        for (Node *it = fn->args.head; it; it = it->next) {
            pre_compile_node(c, it);
        }

        n->backend = backend_data_new(c);
        compile_sprintf(c, "static ");
        compile_type(c, node_fn_return_type(fn));
        compile_sprintf(c, " ");
        compile_backend_data(c, n->backend);

        compile_sprintf(c, "(");
        if (fn->args.head) {
            for (Node *it = fn->args.head; it; it = it->next) {
                compile_type(c, it->type);
                if (it->next) {
                    compile_sprintf(c, ", ");
                }
            }
        } else {
            compile_sprintf(c, "void");
        }
        compile_sprintf(c, ");\n");

        pre_compile_node(c, fn->body);
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        pre_compile_node(c, var->expr);
        pre_compile_type(c, &n->type);

        if (!var->local) {
            compile_type(c, n->type);
            compile_sprintf(c, " ");

            n->backend = backend_data_new(c);
            compile_backend_data(c, n->backend);

            compile_sprintf(c, ";\n");
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        pre_compile_node(c, print->operand);
    } break;

    default:
        unreachable();
        break;
    }
}

static NodeFn *get_main(Context *c) {
    Node *main = scope_find(c->globals, sv_from_cstr("main"));
    if (!main) {
        fprintf(stderr, "ERROR: Function 'main' is not defined\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "```\n");
        fprintf(stderr, "fn main() {\n");
        fprintf(stderr, "    // HINT: Define this\n");
        fprintf(stderr, "}\n");
        fprintf(stderr, "```\n");
        exit(1);
    }

    if (main->kind != NODE_FN) {
        fprintf(stderr, PosFmt "ERROR: Function 'main' must be a function literal\n", PosArg(main->token.pos));
        exit(1);
    }

    NodeFn *main_fn = (NodeFn *) main;
    if (main_fn->arity) {
        fprintf(stderr, PosFmt "ERROR: Function 'main' cannot take any arguments\n", PosArg(main->token.pos));
        exit(1);
    }

    if (main_fn->ret) {
        fprintf(stderr, PosFmt "ERROR: Function 'main' cannot return anything\n", PosArg(main->token.pos));
        exit(1);
    }
    return main_fn;
}

void compile_nodes(Context *context, Cmd *cmd, const char *output) {
    NodeFn *main = get_main(context);

    Compiler c = {0};
    compile_sprintf(&c, "typedef long i64;\n");
    compile_sprintf(&c, "typedef _Bool bool;\n");
    compile_sprintf(&c, "extern int printf(const char *fmt, ...);\n");

    for (size_t i = 0; i < context->globals.count; i++) {
        pre_compile_node(&c, context->globals.data[i]);
    }

    for (size_t i = 0; i < context->globals.count; i++) {
        compile_stmt(&c, context->globals.data[i]);
    }

    compile_sprintf(&c, "\n#line 1 \"glos_start_call_main.h\"");
    compile_sprintf(&c, "\nint main(void) {\n");
    {
        c.indent++;
        for (size_t i = 0; i < context->globals.count; i++) {
            Node *it = context->globals.data[i];
            if (it->kind == NODE_VAR) {
                NodeVar *var = (NodeVar *) it;
                if (var->expr) {
                    compile_indent(&c);
                    compile_backend_data(&c, it->backend);
                    compile_sprintf(&c, " = ");
                    compile_expr(&c, var->expr);
                    compile_sprintf(&c, ";\n");
                }
            }
        }

        compile_indent(&c);
        compile_sprintf(&c, "return (");
        compile_backend_data(&c, main->node.backend);
        compile_sprintf(&c, "(), 0);\n");
        c.indent--;
    }
    compile_sprintf(&c, "}\n");

    if (sv_has_suffix(sv_from_cstr(output), sv_from_cstr(".c"))) {
        FILE *f = fopen(output, "w");
        if (!f) {
            fprintf(stderr, "ERROR: Could not write file '%s'\n", output);
            exit(1);
        }

        fwrite(c.sb.data, c.sb.count, 1, f);
        fclose(f);
    } else {
        da_push(cmd, "cc");
        da_push(cmd, "-g");
        da_push(cmd, "-o");
        da_push(cmd, output);
        da_push(cmd, "-x");
        da_push(cmd, "c");
        da_push(cmd, "-");

        FILE *in = NULL;
        Proc  proc = cmd_run_async(cmd, (CmdStdio) {.in = &in});

        if (in) {
            fwrite(c.sb.data, c.sb.count, 1, in);
            fclose(in);
        }

        const int code = cmd_wait(proc);
        if (code) {
            exit(code);
        }
    }

    da_free(&c.sb);
    da_free(&c.fns);
}
