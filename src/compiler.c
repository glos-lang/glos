#include "compiler.h"
#include "message.h"

static_assert(COUNT_TYPES == 17, "");
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

static_assert(COUNT_TYPES == 17, "");
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

    case TYPE_SLICE:
        type->qbe = c->slice_type;
        break;

    case TYPE_ARRAY:
        compile_type(c, type->spec_type);
        type->qbe = qbe_type_array(c->qbe, type->spec_type->qbe, type->spec_count);
        break;

    case TYPE_STRUCT: {
        NodeStruct *spec = (NodeStruct *) type->spec_node;
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

    case TYPE_GENERIC:
        type->qbe = type->spec_node->type.spec_type->qbe;
        break;

    default:
        unreachable();
    }
}

static void compile_stmt(Compiler *c, Node *n);

static QbeCall *compile_panic_begin(Compiler *c) {
    const char *stderr_name = "stderr";
    {
        QbeTarget target = qbe_target_default();
        if (target == QBE_TARGET_ARM64_MACOS || target == QBE_TARGET_X86_64_MACOS) {
            stderr_name = "__stderrp";
        }
    }

    QbeNode *stderr_symbol = qbe_atom_extern(c->qbe, qbe_sv_from_cstr(stderr_name), qbe_type_basic(QBE_TYPE_I64));
    QbeNode *fprintf_symbol = qbe_atom_extern_fn(c->qbe, qbe_sv_from_cstr("fprintf"));

    QbeCall *call = qbe_call_new(c->qbe, fprintf_symbol, qbe_type_basic(QBE_TYPE_I32));
    qbe_call_add_arg(c->qbe, call, qbe_build_load(c->qbe, c->fn, stderr_symbol, qbe_type_basic(QBE_TYPE_I64), false));
    return call;
}

static void compile_panic_end(Compiler *c, QbeCall *call) {
    qbe_build_call(c->qbe, c->fn, call);

    const char *stdout_name = "stdout";
    const char *stderr_name = "stderr";
    {
        QbeTarget target = qbe_target_default();
        if (target == QBE_TARGET_ARM64_MACOS || target == QBE_TARGET_X86_64_MACOS) {
            stdout_name = "__stdoutp";
            stderr_name = "__stderrp";
        }
    }

    QbeNode *abort_symbol = qbe_atom_extern_fn(c->qbe, qbe_sv_from_cstr("abort"));
    QbeNode *fflush_symbol = qbe_atom_extern_fn(c->qbe, qbe_sv_from_cstr("fflush"));
    QbeNode *stdout_symbol = qbe_atom_extern(c->qbe, qbe_sv_from_cstr(stdout_name), qbe_type_basic(QBE_TYPE_I64));
    QbeNode *stderr_symbol = qbe_atom_extern(c->qbe, qbe_sv_from_cstr(stderr_name), qbe_type_basic(QBE_TYPE_I64));

    call = qbe_call_new(c->qbe, fflush_symbol, qbe_type_basic(QBE_TYPE_I32));
    qbe_call_add_arg(c->qbe, call, qbe_build_load(c->qbe, c->fn, stdout_symbol, qbe_type_basic(QBE_TYPE_I64), false));
    qbe_build_call(c->qbe, c->fn, call);

    call = qbe_call_new(c->qbe, fflush_symbol, qbe_type_basic(QBE_TYPE_I32));
    qbe_call_add_arg(c->qbe, call, qbe_build_load(c->qbe, c->fn, stderr_symbol, qbe_type_basic(QBE_TYPE_I64), false));
    qbe_build_call(c->qbe, c->fn, call);

    qbe_build_call(c->qbe, c->fn, qbe_call_new(c->qbe, abort_symbol, qbe_type_basic(QBE_TYPE_I0)));
}

static QbeNode *compile_str(Compiler *c, SV sv, bool ref) {
    QbeNode *slice_data = qbe_str_new(c->qbe, (QbeSV) {.data = sv.data, .count = sv.count});
    QbeNode *slice_count = qbe_atom_int(c->qbe, QBE_TYPE_I64, sv.count);

    QbeNode *slice_struct = qbe_fn_add_var(c->qbe, c->fn, c->slice_type);
    qbe_build_store(c->qbe, c->fn, slice_struct, slice_data);
    qbe_build_store(
        c->qbe,
        c->fn,
        qbe_build_binary(
            c->qbe,
            c->fn,
            QBE_BINARY_ADD,
            qbe_type_basic(QBE_TYPE_I64),
            slice_struct,
            qbe_atom_int(c->qbe, QBE_TYPE_I64, 8)),
        slice_count);

    if (ref) {
        return slice_struct;
    }

    return qbe_build_load(c->qbe, c->fn, slice_struct, c->slice_type, false);
}

static QbeNode *compile_fn(Compiler *c, NodeFn *fn, QbeNode **export_qbe) {
    QbeSV link_as = {0};
    if (fn->link) {
        const SV link = resolve_str_token(fn->link->token, c->context.arena);
        link_as.data = link.data;
        link_as.count = link.count;
    }

    if (!fn->body) {
        if (!link_as.data) {
            link_as.data = fn->node.token.sv.data;
            link_as.count = fn->node.token.sv.count;
        }

        fn->qbe = qbe_atom_extern_fn(c->qbe, link_as);
        if (export_qbe) {
            *export_qbe = fn->qbe;
        }
        return fn->qbe;
    }

    Type return_type = node_fn_return_type(fn);
    compile_type(c, &return_type);

    QbeFn *fn_save = c->fn;
    c->fn = qbe_fn_new(c->qbe, link_as, return_type.qbe);
    fn->qbe = (QbeNode *) c->fn;
    if (export_qbe) {
        *export_qbe = fn->qbe;
    }

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
    qbe_fn_set_debug(c->qbe, c->fn, qbe_sv_from_cstr(fn->node.token.pos.path), fn_row + 1);
    qbe_build_return(c->qbe, c->fn, NULL);

    c->fn = fn_save;
    return fn->qbe;
}

static_assert(COUNT_NODES == 22, "");
static QbeNode *compile_expr(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    compile_type(c, &n->type);
    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 67, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            return qbe_atom_int(c->qbe, integer_type_kind(n->type.kind), n->token.as.integer);

        case TOKEN_STR: {
            return compile_str(c, resolve_str_token(n->token, c->context.arena), ref);
        } break;

        case TOKEN_BOOL:
            return qbe_atom_int(c->qbe, QBE_TYPE_I8, n->token.as.boolean);

        case TOKEN_CHAR:
            return qbe_atom_int(c->qbe, QBE_TYPE_I8, n->token.as.integer);

        case TOKEN_IDENT:
            switch (atom->definition->kind) {
            case NODE_FN: {
                NodeFn *fn = (NodeFn *) atom->definition;
                if (fn->generics.head) {
                    assert(fn->generics_count == atom->generics_count);
                    Type *types = temp_alloc(atom->generics_count * sizeof(Type));
                    {
                        Node *generic = atom->generics.head;
                        for (size_t i = 0; i < atom->generics_count; i++) {
                            assert(generic);

                            Type type = generic->type;
                            while (type.kind == TYPE_GENERIC) {
                                assert(type.spec_node->type.spec_type);
                                type = *type.spec_node->type.spec_type;
                            }
                            types[i] = type;

                            generic = generic->next;
                        }
                    }

                    Instantiation *instantiation = instantiations_find(fn->instantiations, types, atom->generics_count);
                    if (instantiation) {
                        return instantiation->qbe;
                    }

                    instantiation = arena_alloc(c->context.arena, sizeof(Instantiation));
                    instantiation->count = atom->generics_count;
                    instantiation->types = arena_clone(c->context.arena, types, atom->generics_count * sizeof(Type));

                    instantiations_push(&fn->instantiations, instantiation);
                    temp_reset(types);

                    Type **save = temp_alloc(atom->generics_count * sizeof(Type *));
                    {
                        Node *generic = fn->generics.head;
                        Node *specific = atom->generics.head;
                        for (size_t i = 0; i < atom->generics_count; i++) {
                            assert(generic);
                            save[i] = generic->type.spec_type;

                            assert(specific);
                            compile_type(c, &specific->type);
                            generic->type.spec_type = &specific->type;

                            generic = generic->next;
                            specific = specific->next;
                        }
                    }

                    compile_fn(c, fn, &instantiation->qbe);

                    {
                        Node *generic = fn->generics.head;
                        for (size_t i = 0; i < atom->generics_count; i++) {
                            assert(generic);
                            generic->type.spec_type = save[i];
                            generic = generic->next;
                        }
                    }

                    temp_reset(save);
                    return instantiation->qbe;
                }

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

            case NODE_CONST: {
                NodeConst *constt = (NodeConst *) atom->definition;
                if (constt->value.is_string) {
                    return compile_str(c, constt->value.as.sv, false);
                }

                if (n->type.kind == TYPE_BOOL) {
                    return qbe_atom_int(c->qbe, QBE_TYPE_I8, constt->value.as.boolean);
                }

                return qbe_atom_int(c->qbe, integer_type_kind(n->type.kind), constt->value.as.integer);
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

        if (cast->from->kind == NODE_ATOM && cast->from->token.kind == TOKEN_STR) {
            return qbe_build_load(c->qbe, c->fn, from, n->type.qbe, type_is_signed(n->type));
        }

        return qbe_build_cast(c->qbe, c->fn, from, n->type.qbe.kind, type_is_signed(n->type));
    }

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 67, "");
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

        case TOKEN_LNOT: {
            QbeNode *operand = compile_expr(c, unary->operand, false);
            return qbe_build_unary(c->qbe, c->fn, QBE_UNARY_LNOT, n->type.qbe, operand);
        }

        case TOKEN_LEN: {
            if (unary->operand->type.kind == TYPE_ARRAY) {
                return qbe_atom_int(c->qbe, n->type.qbe.kind, unary->operand->type.spec_count);
            }

            QbeNode *operand = compile_expr(c, unary->operand, true);
            operand = qbe_build_binary(
                c->qbe,
                c->fn,
                QBE_BINARY_ADD,
                qbe_type_basic(QBE_TYPE_I64),
                operand,
                qbe_atom_int(c->qbe, QBE_TYPE_I64, 8));

            return qbe_build_load(c->qbe, c->fn, operand, n->type.qbe, type_is_signed(n->type));
        }

        default:
            unreachable();
        }
    }

    case NODE_INDEX: {
        NodeIndex *index = (NodeIndex *) n;

        QbeNode *base = compile_expr(c, index->base, index->base->type.kind == TYPE_ARRAY && !index->base->type.ref);
        QbeNode *from = compile_expr(c, index->from, false);

        if (index->ranged) {
            const size_t element_size = compile_sizeof(c, n->type.spec_type);

            QbeNode *to = compile_expr(c, index->to, false);
            if (from) {
                from = qbe_build_cast(c->qbe, c->fn, from, QBE_TYPE_I64, false);
            } else {
                from = qbe_atom_int(c->qbe, QBE_TYPE_I64, 0);
            }

            if (to) {
                to = qbe_build_cast(c->qbe, c->fn, to, QBE_TYPE_I64, false);
            }

            QbeNode *slice_data = NULL;
            if (index->base->type.ref) {
                slice_data = base;
            } else if (index->base->type.kind == TYPE_SLICE || index->base->type.kind == TYPE_ARRAY) {
                QbeNode *count = NULL;
                if (index->base->type.kind == TYPE_SLICE) {
                    slice_data = qbe_build_load(c->qbe, c->fn, base, qbe_type_basic(QBE_TYPE_I64), false);
                    count = qbe_build_load(
                        c->qbe,
                        c->fn,
                        qbe_build_binary(
                            c->qbe,
                            c->fn,
                            QBE_BINARY_ADD,
                            qbe_type_basic(QBE_TYPE_I64),
                            base,
                            qbe_atom_int(c->qbe, QBE_TYPE_I64, 8)),
                        qbe_type_basic(QBE_TYPE_I64),
                        true);
                } else {
                    slice_data = base;
                    count = qbe_atom_int(c->qbe, QBE_TYPE_I64, index->base->type.spec_count);
                }

                if (!to) {
                    to = count;
                }

                // Bounds Check
                {
                    QbeBlock *failure = qbe_block_new(c->qbe);
                    QbeBlock *success = qbe_block_new(c->qbe);

                    QbeNode *bounds_check_from =
                        qbe_build_binary(c->qbe, c->fn, QBE_BINARY_ULE, qbe_type_basic(QBE_TYPE_I8), from, count);

                    QbeNode *bounds_check_to =
                        qbe_build_binary(c->qbe, c->fn, QBE_BINARY_ULE, qbe_type_basic(QBE_TYPE_I8), to, count);

                    QbeNode *bounds_check = qbe_build_binary(
                        c->qbe, c->fn, QBE_BINARY_AND, qbe_type_basic(QBE_TYPE_I8), bounds_check_from, bounds_check_to);

                    qbe_build_branch(c->qbe, c->fn, bounds_check, success, failure);

                    // Out of Bounds
                    qbe_build_block(c->qbe, c->fn, failure);

                    // Panic
                    {
                        QbeCall *call = compile_panic_begin(c);

                        QbeSV message = qbe_sv_from_cstr(arena_sprintf(
                            c->context.arena,
                            PosFmt " Range (%%ld..%%ld) is out of bounds in slice of length %%ld\n",
                            PosArg(n->token.pos)));

                        qbe_call_add_arg(c->qbe, call, qbe_str_new(c->qbe, message));
                        qbe_call_start_variadic(c->qbe, call);
                        qbe_call_add_arg(c->qbe, call, from);
                        qbe_call_add_arg(c->qbe, call, to);
                        qbe_call_add_arg(c->qbe, call, count);
                        compile_panic_end(c, call);
                    }

                    // Success
                    qbe_build_block(c->qbe, c->fn, success);
                }

                // Check if bounds are ascending
                {
                    QbeBlock *failure = qbe_block_new(c->qbe);
                    QbeBlock *success = qbe_block_new(c->qbe);

                    QbeNode *check =
                        qbe_build_binary(c->qbe, c->fn, QBE_BINARY_ULE, qbe_type_basic(QBE_TYPE_I8), from, to);

                    qbe_build_branch(c->qbe, c->fn, check, success, failure);

                    // Out of Bounds
                    qbe_build_block(c->qbe, c->fn, failure);

                    // Panic
                    {
                        QbeCall *call = compile_panic_begin(c);

                        QbeSV message = qbe_sv_from_cstr(arena_sprintf(
                            c->context.arena,
                            PosFmt " Range (%%ld..%%ld) is invalid: Start of range is more than end\n",
                            PosArg(n->token.pos)));

                        qbe_call_add_arg(c->qbe, call, qbe_str_new(c->qbe, message));
                        qbe_call_start_variadic(c->qbe, call);
                        qbe_call_add_arg(c->qbe, call, from);
                        qbe_call_add_arg(c->qbe, call, to);
                        compile_panic_end(c, call);
                    }

                    // Success
                    qbe_build_block(c->qbe, c->fn, success);
                }
            } else {
                unreachable();
            }

            QbeNode *offset = qbe_build_binary(
                c->qbe,
                c->fn,
                QBE_BINARY_MUL,
                qbe_type_basic(QBE_TYPE_I64),
                from,
                qbe_atom_int(c->qbe, QBE_TYPE_I64, element_size));

            slice_data =
                qbe_build_binary(c->qbe, c->fn, QBE_BINARY_ADD, qbe_type_basic(QBE_TYPE_I64), slice_data, offset);

            QbeNode *slice_count =
                qbe_build_binary(c->qbe, c->fn, QBE_BINARY_SUB, qbe_type_basic(QBE_TYPE_I64), to, from);

            QbeNode *slice_struct = qbe_fn_add_var(c->qbe, c->fn, c->slice_type);
            qbe_build_store(c->qbe, c->fn, slice_struct, slice_data);
            qbe_build_store(
                c->qbe,
                c->fn,
                qbe_build_binary(
                    c->qbe,
                    c->fn,
                    QBE_BINARY_ADD,
                    qbe_type_basic(QBE_TYPE_I64),
                    slice_struct,
                    qbe_atom_int(c->qbe, QBE_TYPE_I64, 8)),
                slice_count);

            return qbe_build_load(c->qbe, c->fn, slice_struct, c->slice_type, false);
        } else {
            const size_t element_size = compile_sizeof(c, index->base->type.spec_type);

            from = qbe_build_cast(c->qbe, c->fn, from, QBE_TYPE_I64, false);

            // Bounds Check
            {
                QbeNode *count = NULL;
                if (index->base->type.kind == TYPE_SLICE) {
                    count = qbe_build_load(
                        c->qbe,
                        c->fn,
                        qbe_build_binary(
                            c->qbe,
                            c->fn,
                            QBE_BINARY_ADD,
                            qbe_type_basic(QBE_TYPE_I64),
                            base,
                            qbe_atom_int(c->qbe, QBE_TYPE_I64, 8)),
                        qbe_type_basic(QBE_TYPE_I64),
                        true);
                } else if (index->base->type.kind == TYPE_ARRAY) {
                    count = qbe_atom_int(c->qbe, QBE_TYPE_I64, index->base->type.spec_count);
                } else {
                    unreachable();
                }

                QbeBlock *failure = qbe_block_new(c->qbe);
                QbeBlock *success = qbe_block_new(c->qbe);

                QbeNode *bounds_check =
                    qbe_build_binary(c->qbe, c->fn, QBE_BINARY_ULT, qbe_type_basic(QBE_TYPE_I8), from, count);

                qbe_build_branch(c->qbe, c->fn, bounds_check, success, failure);

                // Out of Bounds
                qbe_build_block(c->qbe, c->fn, failure);

                // Panic
                {
                    QbeCall *call = compile_panic_begin(c);

                    QbeSV message = qbe_sv_from_cstr(arena_sprintf(
                        c->context.arena,
                        PosFmt " Index %%ld is out of bounds in slice of length %%ld\n",
                        PosArg(n->token.pos)));

                    qbe_call_add_arg(c->qbe, call, qbe_str_new(c->qbe, message));
                    qbe_call_start_variadic(c->qbe, call);
                    qbe_call_add_arg(c->qbe, call, from);
                    qbe_call_add_arg(c->qbe, call, count);
                    compile_panic_end(c, call);
                }

                // Success
                qbe_build_block(c->qbe, c->fn, success);
            }

            QbeNode *ptr = base;
            if (index->base->type.kind == TYPE_SLICE) {
                ptr = qbe_build_load(c->qbe, c->fn, base, qbe_type_basic(QBE_TYPE_I64), false);
            }

            QbeNode *offset = qbe_build_binary(
                c->qbe,
                c->fn,
                QBE_BINARY_MUL,
                qbe_type_basic(QBE_TYPE_I64),
                from,
                qbe_atom_int(c->qbe, QBE_TYPE_I64, element_size));

            QbeNode *element =
                qbe_build_binary(c->qbe, c->fn, QBE_BINARY_ADD, qbe_type_basic(QBE_TYPE_I64), ptr, offset);

            if (ref) {
                return element;
            }

            return qbe_build_load(c->qbe, c->fn, element, n->type.qbe, type_is_signed(n->type));
        }
    }

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        typedef struct {
            QbeBinaryOp s; // Mandatory
            QbeBinaryOp u; // Optional
        } BinaryOp;

        static_assert(COUNT_TOKENS == 67, "");
        static const BinaryOp direct_ops[COUNT_TOKENS] = {
            [TOKEN_ADD] = {.s = QBE_BINARY_ADD},
            [TOKEN_SUB] = {.s = QBE_BINARY_SUB},
            [TOKEN_MUL] = {.s = QBE_BINARY_MUL},
            [TOKEN_DIV] = {.s = QBE_BINARY_SDIV, .u = QBE_BINARY_UDIV},
            [TOKEN_MOD] = {.s = QBE_BINARY_SMOD, .u = QBE_BINARY_UMOD},

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

        static_assert(COUNT_TOKENS == 67, "");
        static const BinaryOp assign_ops[COUNT_TOKENS] = {
            [TOKEN_ADD_SET] = {.s = QBE_BINARY_ADD},
            [TOKEN_SUB_SET] = {.s = QBE_BINARY_SUB},
            [TOKEN_MUL_SET] = {.s = QBE_BINARY_MUL},
            [TOKEN_DIV_SET] = {.s = QBE_BINARY_SDIV, .u = QBE_BINARY_UDIV},
            [TOKEN_MOD_SET] = {.s = QBE_BINARY_SMOD, .u = QBE_BINARY_UMOD},

            [TOKEN_SHL_SET] = {.s = QBE_BINARY_SHL},
            [TOKEN_SHR_SET] = {.s = QBE_BINARY_SSHR, .u = QBE_BINARY_USHR},
            [TOKEN_BOR_SET] = {.s = QBE_BINARY_OR},
            [TOKEN_BAND_SET] = {.s = QBE_BINARY_AND},
        };

        op = assign_ops[n->token.kind];
        if (op.s) {
            const bool is_signed = type_is_signed(binary->lhs->type);

            QbeBinaryOp actual = op.s;
            if (op.u && !is_signed) {
                actual = op.u;
            }

            QbeNode *ptr = compile_expr(c, binary->lhs, true);
            QbeNode *lhs = qbe_build_load(c->qbe, c->fn, ptr, binary->lhs->type.qbe, is_signed);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            QbeNode *value = qbe_build_binary(c->qbe, c->fn, actual, binary->lhs->type.qbe, lhs, rhs);
            qbe_build_store(c->qbe, c->fn, ptr, value);
            return NULL;
        }

        static_assert(COUNT_TOKENS == 67, "");
        switch (n->token.kind) {
        case TOKEN_SET: {
            QbeNode *lhs = compile_expr(c, binary->lhs, true);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            qbe_build_store(c->qbe, c->fn, lhs, rhs);
            return NULL;
        }

        case TOKEN_LOR: {
            QbeNode *lhs = compile_expr(c, binary->lhs, false);

            QbeBlock *lhs_block = qbe_fn_get_current_block(c->fn);
            QbeBlock *rhs_block = qbe_block_new(c->qbe);
            QbeBlock *final_block = qbe_block_new(c->qbe);
            qbe_build_branch(c->qbe, c->fn, lhs, final_block, rhs_block);

            // Rhs
            qbe_build_block(c->qbe, c->fn, rhs_block);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            qbe_build_jump(c->qbe, c->fn, final_block);

            // Finally
            qbe_build_block(c->qbe, c->fn, final_block);

            const QbePhiBranch lhs_phi = {
                .block = lhs_block,
                .value = lhs,
            };

            const QbePhiBranch rhs_phi = {
                .block = rhs_block,
                .value = rhs,
            };

            return qbe_build_phi(c->qbe, c->fn, lhs_phi, rhs_phi);
        }

        case TOKEN_LAND: {
            QbeNode *lhs = compile_expr(c, binary->lhs, false);

            QbeBlock *lhs_block = qbe_fn_get_current_block(c->fn);
            QbeBlock *rhs_block = qbe_block_new(c->qbe);
            QbeBlock *final_block = qbe_block_new(c->qbe);
            qbe_build_branch(c->qbe, c->fn, lhs, rhs_block, final_block);

            // Rhs
            qbe_build_block(c->qbe, c->fn, rhs_block);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            qbe_build_jump(c->qbe, c->fn, final_block);

            // Finally
            qbe_build_block(c->qbe, c->fn, final_block);

            const QbePhiBranch lhs_phi = {
                .block = lhs_block,
                .value = lhs,
            };

            const QbePhiBranch rhs_phi = {
                .block = rhs_block,
                .value = rhs,
            };

            return qbe_build_phi(c->qbe, c->fn, lhs_phi, rhs_phi);
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

            if (member->lhs->type.kind == TYPE_STRUCT) {
                Type spec = type_remove_ref(member->lhs->type);
                compile_type(c, &spec);
            }
        } else {
            lhs = compile_expr(c, member->lhs, true);
        }

        NodeField   *field = (NodeField *) member->definition;
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
        qbe_build_store_zero(c->qbe, c->fn, temp, n->type.qbe);

        // For array literal
        size_t array_item_size = 0;
        size_t array_items_count = 0;

        Node *struct_fields_iota = NULL; // For structure literal
        if (n->type.kind == TYPE_STRUCT) {
            struct_fields_iota = ((NodeStruct *) n->type.spec_node)->fields.head;
        } else {
            array_item_size = compile_sizeof(c, n->type.spec_type);
        }

        for (Node *it = compound->nodes.head; it; it = it->next) {
            if (it->kind == NODE_BINARY && it->token.kind == TOKEN_COLON) {
                NodeBinary *assign = (NodeBinary *) it;

                size_t offset = 0;
                if (n->type.kind == TYPE_STRUCT) {
                    assert(assign->lhs->kind == NODE_ATOM && assign->lhs->token.kind == TOKEN_IDENT);
                    NodeAtom *lhs = (NodeAtom *) assign->lhs;

                    assert(lhs->definition->kind == NODE_FIELD);
                    offset = qbe_offsetof(((NodeField *) lhs->definition)->qbe);
                } else {
                    offset = assign->lhs->token.as.integer * array_item_size;
                }

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
                size_t offset = 0;
                if (n->type.kind == TYPE_STRUCT) {
                    assert(struct_fields_iota->kind == NODE_FIELD);
                    offset = qbe_offsetof(((NodeField *) struct_fields_iota)->qbe);
                    struct_fields_iota = struct_fields_iota->next;
                } else {
                    offset = array_items_count++ * array_item_size;
                }

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

                qbe_build_store(c->qbe, c->fn, ptr, compile_expr(c, it, false));
            }
        }

        if (ref) {
            return temp;
        }

        return qbe_build_load(c->qbe, c->fn, temp, n->type.qbe, false);
    }

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;

        QbeBlock *consequence_block = qbe_block_new(c->qbe);
        QbeBlock *antecedence_block = qbe_block_new(c->qbe);
        QbeBlock *final_block = qbe_block_new(c->qbe);

        // Condition
        QbeNode *condition = compile_expr(c, iff->condition, false);
        qbe_build_branch(c->qbe, c->fn, condition, consequence_block, antecedence_block);

        // Consequence
        qbe_build_block(c->qbe, c->fn, consequence_block);
        QbeNode *consequence = compile_expr(c, iff->consequence, false);
        qbe_build_jump(c->qbe, c->fn, final_block);

        // Antecedence
        qbe_build_block(c->qbe, c->fn, antecedence_block);
        QbeNode *antecedence = compile_expr(c, iff->antecedence, false);
        qbe_build_jump(c->qbe, c->fn, final_block);

        // Finally
        qbe_build_block(c->qbe, c->fn, final_block);
        const QbePhiBranch consequence_phi = {.block = consequence_block, .value = consequence};
        const QbePhiBranch antecedence_phi = {.block = antecedence_block, .value = antecedence};
        return qbe_build_phi(c->qbe, c->fn, consequence_phi, antecedence_phi);
    } break;

    case NODE_FN:
        return compile_fn(c, (NodeFn *) n, NULL);

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 22, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        NodeAssert *assertt = (NodeAssert *) n;
        if (!assertt->is_static) {
            QbeBlock *failure = qbe_block_new(c->qbe);
            QbeBlock *success = qbe_block_new(c->qbe);

            // Condition
            QbeNode *condition = compile_expr(c, assertt->expr, false);
            qbe_build_branch(c->qbe, c->fn, condition, success, failure);

            // Failed
            qbe_build_block(c->qbe, c->fn, failure);

            {
                QbeCall *call = compile_panic_begin(c);

                const char *text = NULL;
                if (assertt->message) {
                    SV sv = assertt->message->token.sv;
                    sv.data++;
                    sv.count -= 2;

                    char *buffer = temp_alloc(assertt->message->token.as.integer);
                    resolve_escape_chars(buffer, &sv);

                    text = arena_sprintf(
                        c->context.arena,
                        PosFmt " Assertion Failed: " SVFmt "\n",
                        PosArg(assertt->expr->token.pos),
                        SVArg(sv));

                    temp_reset(buffer);
                } else {
                    text =
                        arena_sprintf(c->context.arena, PosFmt " Assertion Failed\n", PosArg(assertt->expr->token.pos));
                }

                QbeSV message = qbe_sv_from_cstr(text);

                qbe_call_add_arg(c->qbe, call, qbe_str_new(c->qbe, message));
                compile_panic_end(c, call);
            }

            // Success
            qbe_build_block(c->qbe, c->fn, success);
        }
    } break;

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
            // TODO: Don't generate debugs for "compilation elements":
            //   NODE_VAR (If static)
            //   NODE_TYPE
            //   NODE_CONST
            //   NODE_STRUCT
            //   NODE_EXTERN
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
        if (fn->qbe) {
            return;
        }

        if (fn->generics.head) {
            return;
        }

        compile_fn(c, fn, NULL);
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (var->qbe) {
            return;
        }

        QbeSV link_as = {0};
        if (var->link) {
            const SV link = resolve_str_token(var->link->token, c->context.arena);
            link_as.data = link.data;
            link_as.count = link.count;
        }

        compile_type(c, &n->type);
        if (var->is_extern) {
            if (!link_as.data) {
                link_as.data = n->token.sv.data;
                link_as.count = n->token.sv.count;
            }

            var->qbe = qbe_atom_extern(c->qbe, link_as, qbe_type_basic(QBE_TYPE_I64));
        } else if (var->kind == NODE_VAR_GLOBAL || var->is_static) {
            var->qbe = qbe_var_new(c->qbe, link_as, n->type.qbe, NULL);
            if (var->kind != NODE_VAR_GLOBAL && var->expr) {
                da_push(&c->context.statics, n);
            }
        } else {
            var->qbe = qbe_fn_add_var(c->qbe, c->fn, n->type.qbe);
            if (var->expr) {
                qbe_build_store(c->qbe, c->fn, var->qbe, compile_expr(c, var->expr, false));
            } else {
                qbe_build_store_zero(c->qbe, c->fn, var->qbe, n->type.qbe);
            }
        }
    } break;

    case NODE_TYPE:
    case NODE_CONST:
    case NODE_STRUCT:
        // Pass
        break;

    case NODE_EXTERN: {
        NodeExtern *externn = (NodeExtern *) n;
        for (Node *it = externn->definitions.head; it; it = it->next) {
            compile_stmt(c, it);
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;

        QbeNode *operand = compile_expr(c, print->operand, false);
        QbeCall *call = qbe_call_new(c->qbe, c->print_fn, qbe_type_basic(QBE_TYPE_I32));
        qbe_call_add_arg(c->qbe, call, type_is_signed(print->operand->type) ? c->print_sfmt : c->print_ufmt);
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
    Package *package = packages_find_by_name(*c->packages, sv_from_cstr("main"));
    assert(package);

    Node *main = scope_find(package->globals, sv_from_cstr("main"), false);
    if (!main) {
        error_full(
            ERROR,
            (Pos) {0},
            "Function 'main' is not defined\n"
            "\n"
            "```\n"
            "fn main() {\n"
            "    // Define this\n"
            "}\n"
            "```");
        exit(1);
    }

    if (main->kind != NODE_FN) {
        error_full(ERROR, main->token.pos, "Function 'main' must be a function literal");
        exit(1);
    }

    NodeFn *main_fn = (NodeFn *) main;
    if (main_fn->arity) {
        error_full(ERROR, main->token.pos, "Function 'main' cannot take any arguments");
        exit(1);
    }

    if (main_fn->ret) {
        error_full(ERROR, main->token.pos, "Function 'main' cannot return anything");
        exit(1);
    }

    if (main_fn->generics.head) {
        error_full(ERROR, main->token.pos, "Function 'main' cannot be generic");
        exit(1);
    }
    return main_fn;
}

void compiler_init(Compiler *c) {
    c->qbe = qbe_new();

    c->print_fn = qbe_atom_extern_fn(c->qbe, qbe_sv_from_cstr("printf"));
    c->print_sfmt = qbe_str_new(c->qbe, qbe_sv_from_cstr("%ld\n"));
    c->print_ufmt = qbe_str_new(c->qbe, qbe_sv_from_cstr("%zu\n"));

    QbeStruct *slice_struct = qbe_struct_new(c->qbe, false);
    qbe_struct_add_field(c->qbe, slice_struct, qbe_type_basic(QBE_TYPE_I64));
    qbe_struct_add_field(c->qbe, slice_struct, qbe_type_basic(QBE_TYPE_I64));
    c->slice_type = qbe_type_struct(slice_struct);
}

static void compile_global_var_assignment(Compiler *c, Node *n) {
    if (n->kind == NODE_VAR) {
        NodeVar *var = (NodeVar *) n;
        if (var->expr) {
            qbe_build_store(c->qbe, c->fn, var->qbe, compile_expr(c, var->expr, false));
        }
    }
}

void compiler_build(Compiler *c, const char *object_file_path) {
    assert(c->context.arena);

    NodeFn *main = get_main(&c->context);
    for (Package *p = c->context.packages->head; p; p = p->next) {
        for (size_t i = 0; i < p->globals.count; i++) {
            compile_stmt(c, p->globals.data[i]);
        }
    }

    // Entry
    c->fn = qbe_fn_new(c->qbe, qbe_sv_from_cstr("main"), qbe_type_basic(QBE_TYPE_I32));
    qbe_fn_set_debug(c->qbe, c->fn, qbe_sv_from_cstr("glos_start_call_main.h"), 1);

    for (Package *p = c->context.packages->head; p; p = p->next) {
        for (size_t i = 0; i < p->globals.count; i++) {
            compile_global_var_assignment(c, p->globals.data[i]);
        }
    }

    for (size_t i = 0; i < c->context.statics.count; i++) {
        compile_global_var_assignment(c, c->context.statics.data[i]);
    }

    qbe_build_call(c->qbe, c->fn, qbe_call_new(c->qbe, main->qbe, qbe_type_basic(QBE_TYPE_I32)));
    qbe_build_return(c->qbe, c->fn, qbe_atom_int(c->qbe, QBE_TYPE_I32, 0));

#if 0
    qbe_compile(c->qbe);
    QbeSV program = qbe_get_compiled_program(c->qbe);
    fwrite(program.data, program.count, 1, stdout);
    exit(0);
#endif

    if (qbe_generate(c->qbe, QBE_TARGET_DEFAULT, object_file_path, c->link_flags.data, c->link_flags.count)) {
        error_standalone(ERROR, "Could not generate '%s'", object_file_path);
        exit(1);
    }

    qbe_free(c->qbe);
    da_free(&c->link_flags);
    context_free(&c->context);
}

size_t compile_sizeof(Compiler *c, Type *type) {
    compile_type(c, type);
    return qbe_sizeof(type->qbe);
}
