#include "compiler.h"

static_assert(COUNT_TYPES == 14, "");
static QbeTypeKind integer_type_kind(TypeKind kind) {
    switch (kind) {
    case TYPE_I8:
    case TYPE_U8:
        return QBE_TYPE_I8;

    case TYPE_I16:
    case TYPE_U16:
        return QBE_TYPE_I16;

    case TYPE_I32:
    case TYPE_U32:
        return QBE_TYPE_I32;

    case TYPE_I64:
    case TYPE_U64:
    case TYPE_INT:
        return QBE_TYPE_I64;

    default:
        unreachable();
    }
}

static_assert(COUNT_TYPES == 14, "");
static void compile_type(Compiler *c, Type *type) {
    if (!type) {
        return;
    }

    if (type_is_pointer(*type)) {
        type->qbe = qbe_type_basic(QBE_TYPE_I64);
        return;
    }

    if (type_is_integer(*type)) {
        type->qbe = qbe_type_basic(integer_type_kind(type->kind));
        return;
    }

    switch (type->kind) {
    case TYPE_UNIT:
        type->qbe = qbe_type_basic(QBE_TYPE_I0);
        break;

    case TYPE_BOOL:
        type->qbe = qbe_type_basic(QBE_TYPE_I8);
        break;

    case TYPE_FN:
        type->qbe = qbe_type_basic(QBE_TYPE_I64);
        break;

    case TYPE_STRUCT: {
        NodeStruct *spec = (NodeStruct *) type->spec;
        if (!spec->qbe) {
            spec->qbe = qbe_struct_new(c->qbe, false);
            for (Node *it = spec->fields.head; it; it = it->next) {
                compile_type(c, &it->type);
                NodeField *field = (NodeField *) it;
                field->qbe = qbe_struct_add_field(c->qbe, spec->qbe, it->type.qbe);
            }
        }

        type->qbe = qbe_type_struct(spec->qbe);
    } break;

    default:
        unreachable();
    }
}

static void compile_stmt(Compiler *c, Node *n);

static_assert(COUNT_NODES == 18, "");
static QbeNode *compile_expr(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    compile_type(c, &n->type);
    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 38, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            return qbe_atom_int(c->qbe, integer_type_kind(n->type.kind), n->token.as.integer);

        case TOKEN_BOOL:
            return qbe_atom_int(c->qbe, QBE_TYPE_I8, n->token.as.boolean);

        case TOKEN_IDENT:
            switch (atom->definition->kind) {
            case NODE_FN: {
                NodeFn *fn = (NodeFn *) atom->definition;
                if (!fn->qbe) {
                    compile_stmt(c, atom->definition);
                }
                return fn->qbe;
            }

            case NODE_VAR: {
                NodeVar *var = (NodeVar *) atom->definition;
                if (!var->qbe) {
                    compile_stmt(c, atom->definition);
                }

                if (ref || var->kind == NODE_VAR_ARG) {
                    return var->qbe;
                }

                return qbe_build_load(c->qbe, c->fn, var->qbe, n->type.qbe, type_is_signed(n->type));
            }

            default:
                unreachable();
            }
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        QbeNode  *fn = compile_expr(c, call->fn, false);

        QbeCall *fn_call = qbe_call_new(c->qbe, fn, n->type.qbe);
        for (Node *it = call->args.head; it; it = it->next) {
            qbe_call_add_arg(c->qbe, fn_call, compile_expr(c, it, false));
        }

        qbe_build_call(c->qbe, c->fn, fn_call);
        return (QbeNode *) fn_call;
    }

    case NODE_CAST: {
        NodeCast *cast = (NodeCast *) n;
        QbeNode  *from = compile_expr(c, cast->from, false);
        if (type_eq(n->type, (Type) {.kind = TYPE_BOOL}) && !type_eq(cast->from->type, (Type) {.kind = TYPE_BOOL})) {
            QbeNode *zero = qbe_atom_int(c->qbe, cast->from->type.qbe.kind, 0);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_NE, n->type.qbe, from, zero);
        }

        return qbe_build_cast(c->qbe, c->fn, from, n->type.qbe.kind, type_is_signed(n->type));
    }

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 38, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            QbeNode *operand = compile_expr(c, unary->operand, false);
            return qbe_build_unary(c->qbe, c->fn, QBE_UNARY_NEG, n->type.qbe, operand);
        }

        case TOKEN_MUL: {
            QbeNode *operand = compile_expr(c, unary->operand, false);
            if (ref) {
                return operand;
            }
            return qbe_build_load(c->qbe, c->fn, operand, n->type.qbe, type_is_signed(n->type));
        }

        case TOKEN_BAND:
            return compile_expr(c, unary->operand, true);

        case TOKEN_BNOT: {
            QbeNode *operand = compile_expr(c, unary->operand, false);
            return qbe_build_unary(c->qbe, c->fn, QBE_UNARY_BNOT, n->type.qbe, operand);
        }

        default:
            unreachable();
        }
    }

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        typedef struct {
            QbeBinaryOp s; // Mandatory
            QbeBinaryOp u; // Optional
        } BinaryOp;

        static_assert(COUNT_TOKENS == 38, "");
        static const BinaryOp direct_ops[COUNT_TOKENS] = {
            [TOKEN_ADD] = {.s = QBE_BINARY_ADD},
            [TOKEN_SUB] = {.s = QBE_BINARY_SUB},
            [TOKEN_MUL] = {.s = QBE_BINARY_MUL},
            [TOKEN_DIV] = {.s = QBE_BINARY_SDIV, .u = QBE_BINARY_UDIV},

            [TOKEN_SHL] = {.s = QBE_BINARY_SHL},
            [TOKEN_SHR] = {.s = QBE_BINARY_SSHR, .u = QBE_BINARY_USHR},
            [TOKEN_BOR] = {.s = QBE_BINARY_OR},
            [TOKEN_BAND] = {.s = QBE_BINARY_AND},

            [TOKEN_GT] = {.s = QBE_BINARY_SGT, .u = QBE_BINARY_UGT},
            [TOKEN_GE] = {.s = QBE_BINARY_SGE, .u = QBE_BINARY_UGE},
            [TOKEN_LT] = {.s = QBE_BINARY_SLT, .u = QBE_BINARY_ULT},
            [TOKEN_LE] = {.s = QBE_BINARY_SLE, .u = QBE_BINARY_ULE},
            [TOKEN_EQ] = {.s = QBE_BINARY_EQ},
            [TOKEN_NE] = {.s = QBE_BINARY_NE},
        };

        BinaryOp op = direct_ops[n->token.kind];
        if (op.s) {
            QbeBinaryOp actual = op.s;
            if (op.u && !type_is_signed(binary->lhs->type)) {
                actual = op.u;
            }

            QbeNode *lhs = compile_expr(c, binary->lhs, false);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            return qbe_build_binary(c->qbe, c->fn, actual, n->type.qbe, lhs, rhs);
        }

        static_assert(COUNT_TOKENS == 38, "");
        switch (n->token.kind) {
        case TOKEN_SET: {
            QbeNode *lhs = compile_expr(c, binary->lhs, true);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            qbe_build_store(c->qbe, c->fn, lhs, rhs);
            return NULL;
        }

        default:
            unreachable();
        }
    }

    case NODE_MEMBER: {
        NodeMember *member = (NodeMember *) n;
        QbeNode    *lhs = NULL;

        if (member->lhs->type.ref) {
            lhs = compile_expr(c, member->lhs, false);
            for (size_t i = 1; i < member->lhs->type.ref; i++) {
                lhs = qbe_build_load(c->qbe, c->fn, lhs, qbe_type_basic(QBE_TYPE_I64), false);
            }
        } else {
            lhs = compile_expr(c, member->lhs, true);
        }

        NodeField *field = (NodeField *) member->definition;

        // Ensure the struct is compiled
        {
            Type spec = type_remove_ref(member->lhs->type);
            compile_type(c, &spec);
        }

        const size_t offset = qbe_offsetof(field->qbe);
        if (offset) {
            lhs = qbe_build_binary(
                c->qbe,
                c->fn,
                QBE_BINARY_ADD,
                qbe_type_basic(QBE_TYPE_I64),
                lhs,
                qbe_atom_int(c->qbe, QBE_TYPE_I64, offset));
        }

        if (ref) {
            return lhs;
        }

        return qbe_build_load(c->qbe, c->fn, lhs, n->type.qbe, type_is_signed(n->type));
    }

    case NODE_SIZEOF: {
        NodeSizeof *sizeoff = (NodeSizeof *) n;

        Type *type = NULL;
        if (sizeoff->type) {
            type = &sizeoff->type->type;
        } else {
            type = &sizeoff->expr->type;
        }

        compile_type(c, type);
        return qbe_atom_int(c->qbe, n->type.qbe.kind, qbe_sizeof(type->qbe));
    }

    case NODE_COMPOUND: {
        NodeCompound *compound = (NodeCompound *) n;

        QbeNode *temp = qbe_fn_add_var(c->qbe, c->fn, n->type.qbe);
        qbe_build_bzero(c->qbe, c->fn, temp, n->type.qbe);

        assert(n->type.kind == TYPE_STRUCT);
        for (Node *it = compound->nodes.head; it; it = it->next) {
            if (it->kind == NODE_BINARY && it->token.kind == TOKEN_COLON) {
                NodeBinary *assign = (NodeBinary *) it;

                assert(assign->lhs->kind == NODE_ATOM && assign->lhs->token.kind == TOKEN_IDENT);
                NodeAtom *lhs = (NodeAtom *) assign->lhs;

                assert(lhs->definition->kind == NODE_FIELD);
                const size_t offset = qbe_offsetof(((NodeField *) lhs->definition)->qbe);

                QbeNode *ptr = temp;
                if (offset) {
                    ptr = qbe_build_binary(
                        c->qbe,
                        c->fn,
                        QBE_BINARY_ADD,
                        qbe_type_basic(QBE_TYPE_I64),
                        temp,
                        qbe_atom_int(c->qbe, QBE_TYPE_I64, offset));
                }

                QbeNode *rhs = compile_expr(c, assign->rhs, false);
                qbe_build_store(c->qbe, c->fn, ptr, rhs);
            } else {
                unreachable();
            }
        }

        if (ref) {
            return temp;
        }

        return qbe_build_load(c->qbe, c->fn, temp, n->type.qbe, false);
    }

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        compile_stmt(c, n);
        return fn->qbe;
    }

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 18, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;

        QbeBlock *consequence = qbe_block_new(c->qbe);
        QbeBlock *antecedence = qbe_block_new(c->qbe);

        QbeBlock *end = antecedence;
        if (iff->antecedence) {
            end = qbe_block_new(c->qbe);
        }

        // Condition
        QbeNode *condition = compile_expr(c, iff->condition, false);
        qbe_build_branch(c->qbe, c->fn, condition, consequence, antecedence);

        // Consequence
        qbe_build_block(c->qbe, c->fn, consequence);
        compile_stmt(c, iff->consequence);
        qbe_build_jump(c->qbe, c->fn, end);

        // Antecedence
        if (iff->antecedence) {
            qbe_build_block(c->qbe, c->fn, antecedence);
            compile_stmt(c, iff->antecedence);
            qbe_build_jump(c->qbe, c->fn, end);
        }

        // End
        qbe_build_block(c->qbe, c->fn, end);
    } break;

    case NODE_FOR: {
        NodeFor *forr = (NodeFor *) n;
        compile_stmt(c, forr->init);

        QbeBlock *condition = qbe_block_new(c->qbe);
        QbeBlock *body = qbe_block_new(c->qbe);
        QbeBlock *end = qbe_block_new(c->qbe);

        QbeBlock *update = NULL;
        if (forr->update) {
            update = qbe_block_new(c->qbe);
        }

        // Condition
        qbe_build_block(c->qbe, c->fn, condition);
        if (forr->condition) {
            qbe_build_branch(c->qbe, c->fn, compile_expr(c, forr->condition, false), body, end);
        }

        // Body
        qbe_build_block(c->qbe, c->fn, body);
        compile_stmt(c, forr->body);

        // Update
        if (forr->update) {
            qbe_build_block(c->qbe, c->fn, update);
            compile_expr(c, forr->update, false);
        }

        // Loop
        qbe_build_jump(c->qbe, c->fn, condition);

        // End
        qbe_build_block(c->qbe, c->fn, end);
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            qbe_build_debug_line(c->qbe, c->fn, it->token.pos.row + 1);
            compile_stmt(c, it);
        }
        qbe_build_debug_line(c->qbe, c->fn, n->token.pos.row + 1);
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        qbe_build_return(c->qbe, c->fn, compile_expr(c, ret->value, false));
        qbe_build_block(c->qbe, c->fn, qbe_block_new(c->qbe));
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        if (!fn->body) {
            const QbeSV name = {.data = n->token.sv.data, .count = n->token.sv.count};
            fn->qbe = qbe_atom_symbol(c->qbe, name, qbe_type_basic(QBE_TYPE_I64));
            return;
        }

        Type return_type = node_fn_return_type(fn);
        compile_type(c, &return_type);

        QbeFn *fn_save = c->fn;
        c->fn = qbe_fn_new(c->qbe, (QbeSV) {0}, return_type.qbe);
        fn->qbe = (QbeNode *) c->fn;

        for (Node *it = fn->args.head; it; it = it->next) {
            NodeVar *arg = (NodeVar *) it;
            compile_type(c, &it->type);
            arg->qbe = qbe_fn_add_arg(c->qbe, c->fn, it->type.qbe);
            if (arg->kind == NODE_VAR_LOCAL) {
                QbeNode *var = qbe_fn_add_var(c->qbe, c->fn, it->type.qbe);
                qbe_build_store(c->qbe, c->fn, var, arg->qbe);
                arg->qbe = var;
            }
        }

        assert(fn->body->kind == NODE_BLOCK);
        NodeBlock *fn_block = (NodeBlock *) fn->body;

        size_t fn_row = 0;
        if (fn_block->body.head) {
            fn_row = fn_block->body.head->token.pos.row;

            compile_stmt(c, fn_block->body.head);
            for (Node *it = fn_block->body.head->next; it; it = it->next) {
                qbe_build_debug_line(c->qbe, c->fn, it->token.pos.row + 1);
                compile_stmt(c, it);
            }
        } else {
            fn_row = fn_block->node.token.pos.row;
        }

        qbe_build_debug_line(c->qbe, c->fn, fn_block->node.token.pos.row + 1);
        qbe_fn_set_debug(c->qbe, c->fn, qbe_sv_from_cstr(n->token.pos.path), fn_row + 1);
        qbe_build_return(c->qbe, c->fn, NULL);

        c->fn = fn_save;
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;

        compile_type(c, &n->type);
        if (var->is_extern) {
            const QbeSV name = {.data = n->token.sv.data, .count = n->token.sv.count};
            var->qbe = qbe_atom_symbol(c->qbe, name, qbe_type_basic(QBE_TYPE_I64));
        } else if (var->kind == NODE_VAR_GLOBAL) {
            var->qbe = qbe_var_new(c->qbe, (QbeSV) {0}, n->type.qbe);
        } else {
            var->qbe = qbe_fn_add_var(c->qbe, c->fn, n->type.qbe);
            if (var->expr) {
                qbe_build_store(c->qbe, c->fn, var->qbe, compile_expr(c, var->expr, false));
            } else {
                qbe_build_bzero(c->qbe, c->fn, var->qbe, n->type.qbe);
            }
        }
    } break;

    case NODE_STRUCT:
        // Pass
        break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            compile_stmt(c, it);
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;

        static QbeNode *fn;
        if (!fn) {
            fn = qbe_atom_symbol(c->qbe, qbe_sv_from_cstr("printf"), qbe_type_basic(QBE_TYPE_I64));
        }

        static QbeNode *sfmt;
        if (!sfmt) {
            sfmt = qbe_str_new(c->qbe, qbe_sv_from_cstr("%ld\n"));
        }

        static QbeNode *ufmt;
        if (!ufmt) {
            ufmt = qbe_str_new(c->qbe, qbe_sv_from_cstr("%zu\n"));
        }

        QbeNode *operand = compile_expr(c, print->operand, false);
        QbeCall *call = qbe_call_new(c->qbe, fn, qbe_type_basic(QBE_TYPE_I32));
        qbe_call_add_arg(c->qbe, call, type_is_signed(print->operand->type) ? sfmt : ufmt);
        qbe_call_start_variadic(c->qbe, call);
        qbe_call_add_arg(
            c->qbe, call, qbe_build_cast(c->qbe, c->fn, operand, QBE_TYPE_I64, type_is_signed(print->operand->type)));
        qbe_build_call(c->qbe, c->fn, call);
    } break;

    default:
        compile_expr(c, n, false);
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

void compiler_init(Compiler *c) {
    c->qbe = qbe_new();
}

void compiler_run(Compiler *c, const char *output, const char **flags, size_t flags_count) {
    NodeFn *main = get_main(&c->context);
    for (size_t i = 0; i < c->context.globals.count; i++) {
        compile_stmt(c, c->context.globals.data[i]);
    }

    // Entry
    c->fn = qbe_fn_new(c->qbe, qbe_sv_from_cstr("main"), qbe_type_basic(QBE_TYPE_I32));
    qbe_fn_set_debug(c->qbe, c->fn, qbe_sv_from_cstr("glos_start_call_main.h"), 1);

    for (size_t i = 0; i < c->context.globals.count; i++) {
        Node *it = c->context.globals.data[i];
        if (it->kind == NODE_VAR) {
            NodeVar *var = (NodeVar *) it;
            if (var->expr) {
                qbe_build_store(c->qbe, c->fn, var->qbe, compile_expr(c, var->expr, false));
            }
        }
    }

    qbe_build_call(c->qbe, c->fn, qbe_call_new(c->qbe, main->qbe, qbe_type_basic(QBE_TYPE_I0)));
    qbe_build_return(c->qbe, c->fn, qbe_atom_int(c->qbe, QBE_TYPE_I32, 0));

#if 0
    qbe_compile(c->qbe);
    QbeSV program = qbe_get_compiled_program(c->qbe);
    fwrite(program.data, program.count, 1, stdout);
    exit(0);
#endif

    const int code = qbe_generate(c->qbe, QBE_TARGET_DEFAULT, output, flags, flags_count);
    qbe_free(c->qbe);
    context_free(&c->context);

    if (code) {
        exit(code);
    }
}

size_t compile_sizeof(Compiler *c, Type *type) {
    compile_type(c, type);
    return qbe_sizeof(type->qbe);
}
