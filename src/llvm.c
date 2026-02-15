#include "llvm.h"

typedef enum {
    LLVM_NODE_ATOM,
    LLVM_NODE_UNARY,
    LLVM_NODE_BINARY,

    LLVM_NODE_LOAD,
    LLVM_NODE_STORE,

    LLVM_NODE_CALL,

    LLVM_NODE_BLOCK,
    LLVM_NODE_JUMP,
    LLVM_NODE_BRANCH,
    LLVM_NODE_RETURN,

    LLVM_NODE_FN,
    LLVM_NODE_VAR,

    LLVM_NODE_PRINT,
    COUNT_LLVM_NODES
} LLVM_Node_Kind;

struct LLVM_Node {
    LLVM_Node_Kind kind;

    LLVM_Type type;
    size_t    iota;
    SV        sv;

    LLVM_Debug_Pos *debug;
    LLVM_Node      *next;
};

struct LLVM_Debug_Pos {
    size_t iota;

    size_t row;
    size_t col;

    LLVM_Debug_Scope *scope;
    LLVM_Debug_Pos   *next;
};

struct LLVM_Debug_File {
    size_t      iota;
    const char *path;
};

struct LLVM_Debug_Scope {
    size_t iota;

    size_t row;
    size_t col;

    LLVM_Node_Fn     *fn;
    LLVM_Debug_Scope *outer;
};

typedef struct {
    LLVM_Node node;
    union {
        long integer;
    } as;
    bool is_zeroed;
} LLVM_Node_Atom;

typedef struct {
    LLVM_Node       node;
    LLVM_Unary_Kind kind;
    LLVM_Node      *value;
} LLVM_Node_Unary;

typedef struct {
    LLVM_Node        node;
    LLVM_Binary_Kind kind;
    LLVM_Node       *lhs;
    LLVM_Node       *rhs;
} LLVM_Node_Binary;

typedef struct {
    LLVM_Node  node;
    LLVM_Node *ptr;
} LLVM_Node_Load;

typedef struct {
    LLVM_Node  node;
    LLVM_Node *ptr;
    LLVM_Node *value;
} LLVM_Node_Store;

typedef struct {
    LLVM_Node   node;
    LLVM_Node  *fn;
    LLVM_Node **args;
    size_t      arity;
} LLVM_Node_Call;

struct LLVM_Node_Block {
    LLVM_Node node;
};

typedef struct {
    LLVM_Node        node;
    LLVM_Node_Block *block;
} LLVM_Node_Jump;

typedef struct {
    LLVM_Node        node;
    LLVM_Node       *condition;
    LLVM_Node_Block *consequence;
    LLVM_Node_Block *antecedence;
} LLVM_Node_Branch;

typedef struct {
    LLVM_Node  node;
    LLVM_Node *value;
} LLVM_Node_Return;

struct LLVM_Node_Fn {
    LLVM_Node  node;
    LLVM_Nodes vars;
    LLVM_Nodes body;

    LLVM_Node_Var **args;
    size_t          arity;

    LLVM_Debug_Pos    debug;
    LLVM_Debug_Scope *debug_scope;
};

typedef struct LLVM_Node_Var_Init LLVM_Node_Var_Init;

struct LLVM_Node_Var_Init {
    LLVM_Node *node;

    LLVM_Type type;
    long      n;

    LLVM_Node_Var_Init *next;
};

struct LLVM_Node_Var {
    LLVM_Node node;
    SV        name;

    bool   is_arg;
    size_t arg_index;

    bool                is_zeroed;
    LLVM_Node_Var_Init *init_head;
    LLVM_Node_Var_Init *init_tail;

    LLVM_Debug_Pos debug;
    size_t         debug_local;
};

typedef struct {
    LLVM_Node  node;
    LLVM_Node *value;
} LLVM_Node_Print;

static void llvm_nodes_push(LLVM_Nodes *ns, LLVM_Node *n) {
    if (!n) {
        return;
    }

    if (ns->tail) {
        ns->tail->next = n;
    } else {
        ns->head = n;
    }

    ns->tail = n;
}

static_assert(COUNT_LLVM_NODES == 13, "");
static LLVM_Node *llvm_node_alloc(LLVM *l, LLVM_Node_Kind kind, LLVM_Type type) {
    static const size_t sizes[COUNT_LLVM_NODES] = {
        [LLVM_NODE_ATOM] = sizeof(LLVM_Node_Atom), // Prevent clang-format from messing this up
        [LLVM_NODE_UNARY] = sizeof(LLVM_Node_Unary),
        [LLVM_NODE_BINARY] = sizeof(LLVM_Node_Binary),

        [LLVM_NODE_LOAD] = sizeof(LLVM_Node_Load),
        [LLVM_NODE_STORE] = sizeof(LLVM_Node_Store),

        [LLVM_NODE_CALL] = sizeof(LLVM_Node_Call),

        [LLVM_NODE_BLOCK] = sizeof(LLVM_Node_Block),
        [LLVM_NODE_JUMP] = sizeof(LLVM_Node_Jump),
        [LLVM_NODE_BRANCH] = sizeof(LLVM_Node_Branch),
        [LLVM_NODE_RETURN] = sizeof(LLVM_Node_Return),

        [LLVM_NODE_FN] = sizeof(LLVM_Node_Fn),
        [LLVM_NODE_VAR] = sizeof(LLVM_Node_Var),

        [LLVM_NODE_PRINT] = sizeof(LLVM_Node_Print),
    };

    assert(kind >= LLVM_NODE_ATOM && kind < COUNT_LLVM_NODES);
    LLVM_Node *node = arena_alloc(l->arena, sizes[kind]);
    node->kind = kind;
    node->type = type;
    return node;
}

static LLVM_Node *llvm_node_build(LLVM *l, LLVM_Node_Kind kind, LLVM_Type type) {
    LLVM_Node *node = llvm_node_alloc(l, kind, type);
    llvm_nodes_push(&l->fn->body, node);
    return node;
}

static inline size_t llvm_block_iota(LLVM *l, LLVM_Node_Block *block) {
    if (!block->node.iota) {
        block->node.iota = ++l->iota_local;
    }

    return block->node.iota;
}

static inline void llvm_debug_pos_emit(LLVM *l, LLVM_Debug_Pos *pos) {
    if (!pos) {
        return;
    }
    pos->iota = ++l->iota_debug;
    sb_sprintf(&l->sb, ", !dbg !%zu", pos->iota);
}

static inline void llvm_debug_file_emit(LLVM *l, LLVM_Debug_File *file) {
    file->iota = ++l->iota_debug;
    sb_sprintf(&l->sb, "!%zu = !DIFile(filename: \"%s\", directory: \"\")\n", file->iota, file->path);
}

static_assert(COUNT_LLVM_NODES == 13, "");
static void llvm_node_emit(LLVM *l, LLVM_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case LLVM_NODE_ATOM: {
        LLVM_Node_Atom *atom = (LLVM_Node_Atom *) n;
        if (atom->is_zeroed) {
            sb_push_cstr(&l->sb, "zeroinitializer");
        } else {
            sb_sprintf(&l->sb, "%ld", atom->as.integer);
        }
    } break;

    case LLVM_NODE_BLOCK:
        sb_sprintf(&l->sb, "label %%.%zu", llvm_block_iota(l, (LLVM_Node_Block *) n));
        break;

    default:
        if (n->sv.count) {
            sb_sprintf(&l->sb, "@" SV_Fmt, SV_Arg(n->sv));
        } else {
            assert(n->iota);
            sb_sprintf(&l->sb, "%%.%zu", n->iota);
        }
        break;
    }
}

static_assert(COUNT_LLVM_TYPES == 6, "");
static void llvm_type_emit(LLVM *l, LLVM_Type type, bool i1_to_i8) {
    switch (type.kind) {
    case LLVM_TYPE_I0:
        sb_push_cstr(&l->sb, "void");
        break;

    case LLVM_TYPE_I1:
        sb_push_cstr(&l->sb, i1_to_i8 ? "i8" : "i1");
        break;

    case LLVM_TYPE_I8:
        sb_push_cstr(&l->sb, "i8");
        break;

    case LLVM_TYPE_I32:
        sb_push_cstr(&l->sb, "i32");
        break;

    case LLVM_TYPE_I64:
        sb_push_cstr(&l->sb, "i64");
        break;

    case LLVM_TYPE_FN:
        sb_push_cstr(&l->sb, "ptr");
        break;

    default:
        unreachable();
    }
}

static void llvm_node_build_cast(LLVM *l, LLVM_Node *n, LLVM_Type_Kind to) {
    if (n->type.kind == to) {
        return;
    }

    if (n->kind == LLVM_NODE_ATOM) {
        n->type.kind = to;
        return;
    }

    const size_t temp = ++l->iota_local;
    sb_sprintf(&l->sb, "  %%.%zu = ", temp);

    if (n->type.kind < to) {
        // Lower -> Higher
        if (n->type.kind == LLVM_TYPE_I1) {
            sb_push_cstr(&l->sb, "zext");
        } else {
            sb_push_cstr(&l->sb, "sext");
        }
    } else {
        // Higher -> Lower
        sb_push_cstr(&l->sb, "trunc");
    }

    sb_push(&l->sb, ' ');
    llvm_type_emit(l, n->type, false);
    sb_push(&l->sb, ' ');
    llvm_node_emit(l, n);
    sb_push_cstr(&l->sb, " to ");
    llvm_type_emit(l, (LLVM_Type) {.kind = to}, false);
    sb_push(&l->sb, '\n');
    n->iota = temp;
    n->type.kind = to;
}

static_assert(COUNT_LLVM_NODES == 13, "");
static void llvm_node_compile(LLVM *l, LLVM_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case LLVM_NODE_ATOM:
        unreachable();

    case LLVM_NODE_UNARY: {
        LLVM_Node_Unary *unary = (LLVM_Node_Unary *) n;

        n->iota = ++l->iota_local;
        sb_push_cstr(&l->sb, "  ");
        llvm_node_emit(l, n);
        sb_push_cstr(&l->sb, " = ");

        static_assert(COUNT_LLVM_UNARYS == 3, "");
        switch (unary->kind) {
        case LLVM_UNARY_NEG:
            sb_push_cstr(&l->sb, "sub ");
            llvm_type_emit(l, n->type, false);
            sb_push_cstr(&l->sb, " 0, ");
            llvm_node_emit(l, unary->value);
            break;

        case LLVM_UNARY_LNOT:
            sb_push_cstr(&l->sb, "icmp eq ");
            llvm_type_emit(l, n->type, false);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, unary->value);
            sb_push_cstr(&l->sb, ", 0");
            break;

        default:
            unreachable();
            break;
        }

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_BINARY: {
        LLVM_Node_Binary *binary = (LLVM_Node_Binary *) n;

        n->iota = ++l->iota_local;
        sb_push_cstr(&l->sb, "  ");
        llvm_node_emit(l, n);
        sb_push_cstr(&l->sb, " = ");

        static_assert(COUNT_LLVM_BINARYS == 12, "");
        static const char *ops[COUNT_LLVM_BINARYS] = {
            [LLVM_BINARY_ADD] = "add",
            [LLVM_BINARY_SUB] = "sub",
            [LLVM_BINARY_MUL] = "mul",
            [LLVM_BINARY_DIV] = "sdiv",
            [LLVM_BINARY_MOD] = "srem",

            [LLVM_BINARY_GT] = "icmp sgt",
            [LLVM_BINARY_GE] = "icmp sge",
            [LLVM_BINARY_LT] = "icmp slt",
            [LLVM_BINARY_LE] = "icmp sle",
            [LLVM_BINARY_EQ] = "icmp eq",
            [LLVM_BINARY_NE] = "icmp ne",
        };

        const char *op = ops[binary->kind];
        assert(op);

        sb_sprintf(&l->sb, "%s ", op);
        llvm_type_emit(l, binary->lhs->type, false);
        sb_push(&l->sb, ' ');
        llvm_node_emit(l, binary->lhs);
        sb_sprintf(&l->sb, ", ");
        llvm_node_emit(l, binary->rhs);

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_LOAD: {
        LLVM_Node_Load *load = (LLVM_Node_Load *) n;
        n->iota = ++l->iota_local;
        sb_push_cstr(&l->sb, "  ");
        llvm_node_emit(l, n);
        sb_push_cstr(&l->sb, " = load ");
        llvm_type_emit(l, n->type, true);
        sb_push_cstr(&l->sb, ", ptr ");
        llvm_node_emit(l, load->ptr);
        sb_sprintf(&l->sb, ", align %zu", llvm_type_info(n->type).align);

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');

        if (n->type.kind == LLVM_TYPE_I1) {
            n->type.kind = LLVM_TYPE_I8;
            llvm_node_build_cast(l, n, LLVM_TYPE_I1);
        }
    } break;

    case LLVM_NODE_STORE: {
        LLVM_Node_Store *store = (LLVM_Node_Store *) n;
        if (store->value->type.kind == LLVM_TYPE_I1) {
            llvm_node_build_cast(l, store->value, LLVM_TYPE_I8);
        }

        sb_push_cstr(&l->sb, "  store ");
        llvm_type_emit(l, store->value->type, true);
        sb_push(&l->sb, ' ');
        llvm_node_emit(l, store->value);
        sb_push_cstr(&l->sb, ", ptr ");
        llvm_node_emit(l, store->ptr);
        sb_sprintf(&l->sb, ", align %zu", llvm_type_info(store->value->type).align);

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_CALL: {
        LLVM_Node_Call *call = (LLVM_Node_Call *) n;
        sb_push_cstr(&l->sb, "  ");

        if (n->type.kind != LLVM_TYPE_I0) {
            n->iota = ++l->iota_local;
            llvm_node_emit(l, n);
            sb_push_cstr(&l->sb, " = ");
        }

        sb_push_cstr(&l->sb, "call ");
        llvm_type_emit(l, n->type, false);
        sb_push(&l->sb, ' ');

        llvm_node_emit(l, call->fn);
        sb_push(&l->sb, '(');
        for (size_t i = 0; i < call->arity; i++) {
            if (i) {
                sb_push_cstr(&l->sb, ", ");
            }

            LLVM_Node *arg = call->args[i];
            llvm_type_emit(l, arg->type, false);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, arg);
        }
        sb_push(&l->sb, ')');

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_BLOCK:
        sb_sprintf(&l->sb, ".%zu:\n", llvm_block_iota(l, (LLVM_Node_Block *) n));
        break;

    case LLVM_NODE_JUMP: {
        LLVM_Node_Jump *jump = (LLVM_Node_Jump *) n;
        sb_push_cstr(&l->sb, "  br ");
        llvm_node_emit(l, (LLVM_Node *) jump->block);
        sb_push_cstr(&l->sb, "\n");
    } break;

    case LLVM_NODE_BRANCH: {
        LLVM_Node_Branch *branch = (LLVM_Node_Branch *) n;
        sb_push_cstr(&l->sb, "  br ");
        llvm_type_emit(l, branch->condition->type, false);
        sb_push(&l->sb, ' ');
        llvm_node_emit(l, branch->condition);

        sb_push_cstr(&l->sb, ", ");
        llvm_node_emit(l, (LLVM_Node *) branch->consequence);

        sb_push_cstr(&l->sb, ", ");
        llvm_node_emit(l, (LLVM_Node *) branch->antecedence);

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_RETURN: {
        LLVM_Node_Return *returnn = (LLVM_Node_Return *) n;

        if (returnn->value) {
            sb_push_cstr(&l->sb, "  ret ");
            llvm_type_emit(l, n->type, false);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, returnn->value);
        } else {
            sb_push_cstr(&l->sb, "  ret void");
        }

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_PRINT: {
        LLVM_Node_Print *print = (LLVM_Node_Print *) n;
        llvm_node_build_cast(l, print->value, LLVM_TYPE_I64);

        n->iota = ++l->iota_local;
        sb_push_cstr(&l->sb, "  ");
        llvm_node_emit(l, n);
        sb_push_cstr(&l->sb, " = call i32 (ptr, ...) @printf(ptr @.iprint, i64 ");
        llvm_node_emit(l, print->value);
        sb_push(&l->sb, ')');
        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    default:
        unreachable();
    }
}

void llvm_free(LLVM *l) {
    sb_free(&l->sb);
}

static size_t llvm_type_debug_compile(LLVM *l, LLVM_Type *type);

static size_t llvm_type_fn_debug_compile(LLVM *l, LLVM_Type *type) {
    assert(type->kind == LLVM_TYPE_FN);

    if (type->returnn->kind != LLVM_TYPE_I0) {
        llvm_type_debug_compile(l, type->returnn);
    }

    for (size_t i = 0; i < type->children_count; i++) {
        llvm_type_debug_compile(l, &type->children[i]);
    }

    type->debug = ++l->iota_debug;
    sb_sprintf(&l->sb, "!%zu = !DISubroutineType(types: !{", type->debug);

    if (type->returnn->kind != LLVM_TYPE_I0) {
        sb_sprintf(&l->sb, "!%zu", type->returnn->debug);
        if (type->children_count) {
            sb_push_cstr(&l->sb, ", ");
        }
    }

    for (size_t i = 0; i < type->children_count; i++) {
        if (i) {
            sb_push_cstr(&l->sb, ", ");
        }
        sb_sprintf(&l->sb, "!%zu", type->children[i].debug);
    }

    sb_push_cstr(&l->sb, "})\n");
    return type->debug;
}

// TODO: Temporary solution to permananent problem
static size_t llvm_type_debug_compile(LLVM *l, LLVM_Type *type) {
    static_assert(COUNT_LLVM_TYPES == 6, "");
    switch (type->kind) {
    case LLVM_TYPE_I0:
        unreachable();
        break;

    case LLVM_TYPE_I1:
        type->debug = l->debug_bool_type;
        break;

    case LLVM_TYPE_I8:
        type->debug = l->debug_i8_type;
        break;

    case LLVM_TYPE_I32:
        type->debug = l->debug_i32_type;
        break;

    case LLVM_TYPE_I64:
        type->debug = l->debug_i64_type;
        break;

    case LLVM_TYPE_FN: {
        llvm_type_fn_debug_compile(l, type);
        const size_t debug = ++l->iota_debug;
        sb_sprintf(&l->sb, "!%zu = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !%zu)\n", debug, type->debug);
        type->debug = debug;
    } break;

    default:
        unreachable();
        break;
    }

    return type->debug;
}

static void llvm_debug_scope_compile(LLVM *l, LLVM_Debug_Scope *scope) {
    if (!scope || scope->iota) {
        return;
    }

    if (!scope->outer) {
        // This is the toplevel scope of a function
        scope->iota = scope->fn->debug.iota;
        return;
    }

    llvm_debug_scope_compile(l, scope->outer);

    scope->iota = ++l->iota_debug;
    sb_sprintf(
        &l->sb,
        "!%zu = distinct !DILexicalBlock(scope: !%zu, file: !%zu, line: %zu, column: %zu)\n",
        scope->iota,
        scope->outer->iota,
        l->debug_file->iota,
        scope->row + 1,
        scope->col + 1);
}

static void llvm_debug_pos_compile(LLVM *l, LLVM_Debug_Pos *pos) {
    llvm_debug_scope_compile(l, pos->scope);
    sb_sprintf(
        &l->sb,
        "!%zu = !DILocation(line: %zu, column: %zu, scope: !%zu)\n",
        pos->iota,
        pos->row + 1,
        pos->col + 1,
        pos->scope->iota);
}

static void llvm_var_init_emit(LLVM *l, LLVM_Node_Var_Init *init) {
    if (init->node) {
        sb_push_cstr(&l->sb, "ptr ");
        llvm_node_emit(l, init->node);
    } else {
        llvm_type_emit(l, init->type, true);
        sb_sprintf(&l->sb, " %ld", init->n);
    }
}

void llvm_compile(LLVM *l) {
    assert(l->main_fn);

    l->debug_bool_type = ++l->iota_debug;
    l->debug_i8_type = ++l->iota_debug;
    l->debug_i32_type = ++l->iota_debug;
    l->debug_i64_type = ++l->iota_debug;

    const size_t debug_compilation_unit = ++l->iota_debug;

    llvm_debug_file_emit(l, l->debug_file);
    sb_push_cstr(
        &l->sb,
        "\n"
        "@.iprint = private unnamed_addr constant [5 x i8] c\"%ld\\0A\\00\", align 1\n"
        "declare i32 @printf(ptr, ...)\n");

    if (l->vars.head) {
        sb_push(&l->sb, '\n');
        for (LLVM_Node *it = l->vars.head; it; it = it->next) {
            LLVM_Node_Var *var = (LLVM_Node_Var *) it;

            sb_sprintf(&l->sb, "@" SV_Fmt " = global ", SV_Arg(it->sv));
            if (var->init_head) {
                for (LLVM_Node_Var_Init *it = var->init_head; it; it = it->next) {
                    llvm_var_init_emit(l, it);
                }
            } else {
                llvm_type_emit(l, it->type, true);
                sb_push(&l->sb, ' ');
                sb_sprintf(&l->sb, " zeroinitializer");
            }
            sb_sprintf(&l->sb, ", align %zu", llvm_type_info(it->type).align);

            llvm_debug_pos_emit(l, it->debug);
            sb_push(&l->sb, '\n');
        }
    }

    for (LLVM_Node *it = l->fns.head; it; it = it->next) {
        LLVM_Node_Fn *fn = (LLVM_Node_Fn *) it;
        fn->debug.iota = ++l->iota_debug;

        if (fn == l->main_fn) {
            sb_sprintf(&l->sb, "\ndefine i32 @" SV_Fmt "() {\n", SV_Arg(it->sv));
        } else {
            sb_sprintf(&l->sb, "\ndefine ");
            llvm_type_emit(l, *fn->node.type.returnn, false);
            sb_sprintf(&l->sb, " @" SV_Fmt "(", SV_Arg(it->sv));
            for (size_t i = 0; i < fn->arity; i++) {
                if (i > 0) {
                    sb_push_cstr(&l->sb, ", ");
                }

                llvm_type_emit(l, fn->node.type.children[i], false);
                sb_sprintf(&l->sb, " %%a%zu", i);
            }
            sb_sprintf(&l->sb, ") #0 !dbg !%zu {\n", fn->debug.iota);
        }

        l->iota_local = 0;
        for (LLVM_Node *n = fn->vars.head; n; n = n->next) {
            LLVM_Node_Var *var = (LLVM_Node_Var *) n;

            n->iota = ++l->iota_local;
            sb_push_cstr(&l->sb, "  ");
            llvm_node_emit(l, n);
            sb_push_cstr(&l->sb, " = alloca ");
            llvm_type_emit(l, n->type, true);
            sb_sprintf(&l->sb, ", align %zu\n", llvm_type_info(n->type).align);

            if (var->is_arg) {
                if (n->type.kind == LLVM_TYPE_I1) {
                    const size_t temp = ++l->iota_local;
                    sb_sprintf(&l->sb, "  %%.%zu = zext i1 %%a%zu to i8\n", temp, var->arg_index);
                    sb_sprintf(&l->sb, "  store i8 %%.%zu, ptr ", temp);
                    llvm_node_emit(l, n);
                    sb_push_cstr(&l->sb, ", align 1\n");
                } else {
                    sb_push_cstr(&l->sb, "  store ");
                    llvm_type_emit(l, n->type, false);
                    sb_sprintf(&l->sb, " %%a%zu, ptr ", var->arg_index);
                    llvm_node_emit(l, n);
                    sb_push(&l->sb, '\n');
                }
            } else if (var->is_zeroed) {
                sb_push_cstr(&l->sb, "  store ");
                llvm_type_emit(l, n->type, true);
                sb_push_cstr(&l->sb, " zeroinitializer, ptr ");
                llvm_node_emit(l, n);
                sb_push(&l->sb, '\n');
            }

            var->debug_local = ++l->iota_debug;
            var->debug.iota = ++l->iota_debug;
            sb_push_cstr(&l->sb, "  call void @llvm.dbg.declare(metadata ptr ");
            llvm_node_emit(l, n);
            sb_sprintf(&l->sb, ", metadata !%zu, metadata !DIExpression())", var->debug_local);
            llvm_debug_pos_emit(l, n->debug);
            sb_push(&l->sb, '\n');
        }

        size_t first_row = 0;
        bool   first_row_set = false;
        for (LLVM_Node *n = fn->body.head; n; n = n->next) {
            if (n->debug && !first_row_set) {
                first_row = n->debug->row;
                first_row_set = true;
            }
            llvm_node_compile(l, n);
        }
        sb_push_cstr(&l->sb, "}\n");

        if (fn == l->main_fn) {
            continue;
        }

        const size_t debug_fn_type = llvm_type_fn_debug_compile(l, &fn->node.type);
        sb_sprintf(
            &l->sb,
            "!%zu = distinct !DISubprogram("
            "name: \"" SV_Fmt "\", "
            "scope: !%zu, "
            "file: !%zu, "
            "line: %zu, "
            "scopeLine: %zu, "
            "type: !%zu, "
            "flags: DIFlagPrototyped, "
            "spFlags: DISPFlagDefinition, "
            "unit: !%zu)\n",
            fn->debug.iota,
            SV_Arg(fn->node.sv),
            l->debug_file->iota,
            l->debug_file->iota,
            fn->debug.row + 1,
            first_row + 1,
            debug_fn_type,
            debug_compilation_unit);

        for (LLVM_Node *n = fn->vars.head; n; n = n->next) {
            const size_t debug_type = llvm_type_debug_compile(l, &n->type);

            LLVM_Node_Var *var = (LLVM_Node_Var *) n;
            sb_sprintf(
                &l->sb,
                "!%zu = !DILocalVariable(name: \"" SV_Fmt "\", scope: !%zu, file: !%zu, line: %zu, type: !%zu",
                var->debug_local,
                SV_Arg(var->name),
                fn->debug.iota,
                l->debug_file->iota,
                var->debug.row + 1,
                debug_type);

            if (var->is_arg) {
                sb_sprintf(&l->sb, ", arg: %zu", var->arg_index + 1);
            }
            sb_push_cstr(&l->sb, ")\n");

            llvm_debug_pos_compile(l, n->debug);
        }
    }

    sb_push_cstr(&l->sb, "\nattributes #0 = { \"frame-pointer\"=\"all\" }\n");

    const size_t debug_version = ++l->iota_debug;
    const size_t debug_info_version = ++l->iota_debug;
    sb_sprintf(&l->sb, "!llvm.module.flags = !{!%zu, !%zu}\n", debug_version, debug_info_version);

#ifdef PLATFORM_X86_64_WINDOWS
    sb_sprintf(&l->sb, "!%zu = !{i32 2, !\"CodeView\", i32 1}\n", debug_version);
#else
    sb_sprintf(&l->sb, "!%zu = !{i32 7, !\"Dwarf Version\", i32 5}\n", debug_version);
#endif // PLATFORM_X86_64_WINDOWS
    sb_sprintf(&l->sb, "!%zu = !{i32 2, !\"Debug Info Version\", i32 3}\n", debug_info_version);

    const size_t debug_globals_list = ++l->iota_debug;
    sb_sprintf(&l->sb, "!llvm.dbg.cu = !{!%zu}\n", debug_compilation_unit);
    sb_sprintf(
        &l->sb,
        "!%zu = distinct !DICompileUnit("
        "language: DW_LANG_C11, "
        "globals: !%zu, "
        "file: !%zu, "
        "producer: \"glos\", "
        "isOptimized: false, "
        "runtimeVersion: 0, "
        "emissionKind: FullDebug, "
        "splitDebugInlining: false, "
        "nameTableKind: None)\n",
        debug_compilation_unit,
        debug_globals_list,
        l->debug_file->iota);

    sb_sprintf(&l->sb, "!%zu = !DIBasicType(name: \"bool\", size: 8, encoding: DW_ATE_boolean)\n", l->debug_bool_type);
    sb_sprintf(&l->sb, "!%zu = !DIBasicType(name: \"i8\", size: 8, encoding: DW_ATE_signed)\n", l->debug_i8_type);
    sb_sprintf(&l->sb, "!%zu = !DIBasicType(name: \"i32\", size: 32, encoding: DW_ATE_signed)\n", l->debug_i32_type);
    sb_sprintf(&l->sb, "!%zu = !DIBasicType(name: \"i64\", size: 64, encoding: DW_ATE_signed)\n", l->debug_i64_type);

    for (LLVM_Node *it = l->vars.head; it; it = it->next) {
        const size_t debug_var = ++l->iota_debug;
        sb_sprintf(
            &l->sb,
            "!%zu = !DIGlobalVariableExpression(var: !%zu, expr: !DIExpression())\n",
            it->debug->iota,
            debug_var);

        const size_t debug_type = llvm_type_debug_compile(l, &it->type);
        sb_sprintf(
            &l->sb,
            "!%zu = distinct !DIGlobalVariable(name: \"" SV_Fmt "\", "
            "scope: !%zu, "
            "file: !%zu, "
            "line: %zu, "
            "type: !%zu, "
            "isLocal: false, "
            "isDefinition: true)\n",
            debug_var,
            SV_Arg(it->sv),
            debug_compilation_unit,
            l->debug_file->iota,
            it->debug->row + 1,
            debug_type);
    }

    sb_sprintf(&l->sb, "!%zu = !{", debug_globals_list);
    for (LLVM_Node *it = l->vars.head; it; it = it->next) {
        sb_sprintf(&l->sb, "!%zu", it->debug->iota);
        if (it->next) {
            sb_push_cstr(&l->sb, ", ");
        }
    }
    sb_push_cstr(&l->sb, "}\n");

    for (LLVM_Debug_Pos *it = l->debug_pos; it; it = it->next) {
        llvm_debug_pos_compile(l, it);
    }
}

static_assert(COUNT_LLVM_TYPES == 6, "");
LLVM_Type_Info llvm_type_info(LLVM_Type type) {
    switch (type.kind) {
    case LLVM_TYPE_I0:
        return (LLVM_Type_Info) {.align = 0, .size = 0};

    case LLVM_TYPE_I1:
    case LLVM_TYPE_I8:
        return (LLVM_Type_Info) {.align = 1, .size = 1};

    case LLVM_TYPE_I32:
        return (LLVM_Type_Info) {.align = 4, .size = 4};

    case LLVM_TYPE_I64:
        return (LLVM_Type_Info) {.align = 8, .size = 8};

    case LLVM_TYPE_FN:
        return (LLVM_Type_Info) {.align = 8, .size = 8};

    default:
        unreachable();
        break;
    }
}

LLVM_Type llvm_type_basic(LLVM_Type_Kind kind) {
    return (LLVM_Type) {.kind = kind};
}

LLVM_Type llvm_type_fn(LLVM *l, LLVM_Type *args, size_t arity, LLVM_Type returnn) {
    return (LLVM_Type) {
        .kind = LLVM_TYPE_FN,
        .children = args,
        .children_count = arity,
        .returnn = arena_clone(l->arena, &returnn, sizeof(returnn)),
    };
}

LLVM_Node *llvm_atom_int(LLVM *l, LLVM_Type type, long n) {
    LLVM_Node_Atom *atom = (LLVM_Node_Atom *) llvm_node_alloc(l, LLVM_NODE_ATOM, type);
    atom->as.integer = n;
    return (LLVM_Node *) atom;
}

LLVM_Node *llvm_atom_zero(LLVM *l, LLVM_Type type) {
    LLVM_Node_Atom *atom = (LLVM_Node_Atom *) llvm_node_alloc(l, LLVM_NODE_ATOM, type);
    atom->is_zeroed = true;
    return (LLVM_Node *) atom;
}

LLVM_Node_Block *llvm_block_new(LLVM *l) {
    return (LLVM_Node_Block *) llvm_node_alloc(l, LLVM_NODE_BLOCK, llvm_type_basic(LLVM_TYPE_I0));
}

LLVM_Node_Fn *llvm_fn_new(LLVM *l, SV name, LLVM_Type type) {
    LLVM_Node_Fn *fn = (LLVM_Node_Fn *) llvm_node_alloc(l, LLVM_NODE_FN, type);
    fn->node.sv = name;
    fn->node.debug = &fn->debug;

    fn->args = arena_alloc(l->arena, type.children_count * sizeof(*fn->args));
    fn->arity = type.children_count;

    LLVM_Node_Fn *fn_save = l->fn;
    l->fn = fn;
    for (size_t i = 0; i < type.children_count; i++) {
        LLVM_Node_Var *var = llvm_var_new(l, (SV) {0}, type.children[i], true, false);
        var->is_arg = true;
        var->arg_index = i;
        fn->args[i] = var;
    }
    l->fn = fn_save;

    llvm_nodes_push(&l->fns, (LLVM_Node *) fn);
    return fn;
}

void llvm_fn_debug_set_pos(LLVM *l, LLVM_Node_Fn *fn, size_t row, size_t col) {
    unused(l);
    fn->debug.row = row;
    fn->debug.col = col;
}

LLVM_Node_Var *llvm_fn_arg_get(LLVM_Node_Fn *fn, size_t index) {
    assert(index < fn->node.type.children_count);
    return fn->args[index];
}

LLVM_Node_Var *llvm_var_new(LLVM *l, SV name, LLVM_Type type, bool is_local, bool is_zeroed) {
    LLVM_Node_Var *var = (LLVM_Node_Var *) llvm_node_alloc(l, LLVM_NODE_VAR, type);
    var->node.debug = &var->debug;
    var->name = name;
    var->is_zeroed = is_zeroed;
    if (is_local) {
        llvm_nodes_push(&l->fn->vars, (LLVM_Node *) var);
    } else {
        var->node.sv = name;
        llvm_nodes_push(&l->vars, (LLVM_Node *) var);
    }
    return var;
}

void llvm_var_debug_set_pos(LLVM *l, LLVM_Node_Var *var, size_t row, size_t col) {
    var->debug.row = row;
    var->debug.col = col;
    if (l->fn) {
        var->debug.scope = l->fn->debug_scope;
    }
}

void llvm_var_set_name(LLVM_Node_Var *var, SV name) {
    var->name = name;
}

static void llvm_var_init_push(LLVM_Node_Var *var, LLVM_Node_Var_Init *init) {
    if (var->init_tail) {
        var->init_tail->next = init;
    } else {
        var->init_head = init;
    }

    var->init_tail = init;
}

void llvm_var_init_add_int(LLVM *l, LLVM_Node_Var *var, LLVM_Type type, long n) {
    LLVM_Node_Var_Init *init = arena_alloc(l->arena, sizeof(LLVM_Node_Var_Init));
    init->type = type;
    init->n = n;
    llvm_var_init_push(var, init);
}

void llvm_var_init_add_node(LLVM *l, LLVM_Node_Var *var, LLVM_Node *node) {
    LLVM_Node_Var_Init *init = arena_alloc(l->arena, sizeof(LLVM_Node_Var_Init));
    init->node = node;
    llvm_var_init_push(var, init);
}

LLVM_Node *llvm_build_unary(LLVM *l, LLVM_Unary_Kind kind, LLVM_Type type, LLVM_Node *value) {
    LLVM_Node_Unary *unary = (LLVM_Node_Unary *) llvm_node_build(l, LLVM_NODE_UNARY, type);
    unary->kind = kind;
    unary->value = value;
    return (LLVM_Node *) unary;
}

LLVM_Node *llvm_build_binary(LLVM *l, LLVM_Binary_Kind kind, LLVM_Type type, LLVM_Node *lhs, LLVM_Node *rhs) {
    LLVM_Node_Binary *binary = (LLVM_Node_Binary *) llvm_node_build(l, LLVM_NODE_BINARY, type);
    binary->kind = kind;
    binary->lhs = lhs;
    binary->rhs = rhs;
    return (LLVM_Node *) binary;
}

LLVM_Node *llvm_build_block(LLVM *l, LLVM_Node_Block *block) {
    LLVM_Node *node = (LLVM_Node *) block;
    llvm_nodes_push(&l->fn->body, node);
    return node;
}

LLVM_Node *llvm_build_jump(LLVM *l, LLVM_Node_Block *block) {
    LLVM_Node_Jump *jump = (LLVM_Node_Jump *) llvm_node_build(l, LLVM_NODE_JUMP, llvm_type_basic(LLVM_TYPE_I0));
    jump->block = block;
    return (LLVM_Node *) block;
}

LLVM_Node *
llvm_build_branch(LLVM *l, LLVM_Node *condition, LLVM_Node_Block *consequence, LLVM_Node_Block *antecedence) {
    LLVM_Node_Branch *branch = (LLVM_Node_Branch *) llvm_node_build(l, LLVM_NODE_BRANCH, llvm_type_basic(LLVM_TYPE_I0));
    branch->condition = condition;
    branch->consequence = consequence;
    branch->antecedence = antecedence;
    return (LLVM_Node *) branch;
}

LLVM_Node *llvm_build_return(LLVM *l, LLVM_Node *value) {
    LLVM_Type type;
    if (value) {
        type = value->type;
    } else {
        type = llvm_type_basic(LLVM_TYPE_I0);
    }

    LLVM_Node_Return *returnn = (LLVM_Node_Return *) llvm_node_build(l, LLVM_NODE_RETURN, type);
    returnn->value = value;
    return (LLVM_Node *) returnn;
}

LLVM_Node *llvm_build_load(LLVM *l, LLVM_Node *ptr, LLVM_Type type) {
    LLVM_Node_Load *load = (LLVM_Node_Load *) llvm_node_build(l, LLVM_NODE_LOAD, type);
    load->ptr = ptr;
    return (LLVM_Node *) load;
}

LLVM_Node *llvm_build_store(LLVM *l, LLVM_Node *ptr, LLVM_Node *value) {
    LLVM_Node_Store *store = (LLVM_Node_Store *) llvm_node_build(l, LLVM_NODE_STORE, llvm_type_basic(LLVM_TYPE_I0));
    store->ptr = ptr;
    store->value = value;
    return (LLVM_Node *) store;
}

LLVM_Node *llvm_build_call(LLVM *l, LLVM_Node *fn, LLVM_Node **args, size_t arity) {
    assert(fn->type.kind == LLVM_TYPE_FN);
    LLVM_Node_Call *call = (LLVM_Node_Call *) llvm_node_build(l, LLVM_NODE_CALL, *fn->type.returnn);
    call->fn = fn;
    call->args = args;
    call->arity = arity;
    return (LLVM_Node *) call;
}

LLVM_Node *llvm_build_print(LLVM *l, LLVM_Node *value) {
    LLVM_Node_Print *print = (LLVM_Node_Print *) llvm_node_build(l, LLVM_NODE_PRINT, llvm_type_basic(LLVM_TYPE_I0));
    print->value = value;
    return (LLVM_Node *) print;
}

void llvm_debug_set_file(LLVM *l, const char *path) {
    l->debug_file = arena_alloc(l->arena, sizeof(LLVM_Debug_File));
    l->debug_file->path = path;
}

void llvm_debug_set_pos(LLVM *l, LLVM_Node *n, size_t row, size_t col) {
    n->debug = arena_alloc(l->arena, sizeof(LLVM_Debug_Pos));
    n->debug->row = row;
    n->debug->col = col;
    n->debug->scope = l->fn->debug_scope;

    n->debug->next = l->debug_pos;
    l->debug_pos = n->debug;
}

void llvm_debug_scope_push(LLVM *l, size_t row, size_t col) {
    LLVM_Debug_Scope *scope = arena_alloc(l->arena, sizeof(LLVM_Debug_Scope));
    scope->row = row;
    scope->col = col;

    scope->fn = l->fn;
    scope->outer = l->fn->debug_scope;

    l->fn->debug_scope = scope;
}

void llvm_debug_scope_pop(LLVM *l) {
    assert(l->fn->debug_scope);
    l->fn->debug_scope = l->fn->debug_scope->outer;
}
