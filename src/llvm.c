#include "llvm.h"

typedef enum {
    LLVM_NODE_ATOM,
    LLVM_NODE_UNARY,
    LLVM_NODE_BINARY,

    LLVM_NODE_LOAD,
    LLVM_NODE_STORE,

    LLVM_NODE_BLOCK,
    LLVM_NODE_JUMP,
    LLVM_NODE_BRANCH,

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

    LLVM_Debug_Pos *next;
};

struct LLVM_Debug_File {
    size_t      iota;
    const char *path;
};

typedef struct {
    LLVM_Node node;
    union {
        size_t integer;
    } as;
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

struct LLVM_Node_Var {
    LLVM_Node      node;
    LLVM_Debug_Pos debug;
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

static_assert(COUNT_LLVM_NODES == 10, "");
static LLVM_Node *llvm_node_alloc(LLVM *l, LLVM_Node_Kind kind, LLVM_Type type) {
    static const size_t sizes[COUNT_LLVM_NODES] = {
        [LLVM_NODE_ATOM] = sizeof(LLVM_Node_Atom), // Prevent clang-format from messing this up
        [LLVM_NODE_UNARY] = sizeof(LLVM_Node_Unary),
        [LLVM_NODE_BINARY] = sizeof(LLVM_Node_Binary),

        [LLVM_NODE_LOAD] = sizeof(LLVM_Node_Load),
        [LLVM_NODE_STORE] = sizeof(LLVM_Node_Store),

        [LLVM_NODE_BLOCK] = sizeof(LLVM_Node_Block),
        [LLVM_NODE_JUMP] = sizeof(LLVM_Node_Jump),
        [LLVM_NODE_BRANCH] = sizeof(LLVM_Node_Branch),

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
    llvm_nodes_push(&l->body, node);
    return node;
}

static inline size_t llvm_block_iota(LLVM *l, LLVM_Node_Block *block) {
    if (!block->node.iota) {
        block->node.iota = ++l->iota_local;
    }

    return block->node.iota;
}

static inline void llvm_debug_pos_emit(LLVM *l, LLVM_Debug_Pos *pos) {
    pos->iota = ++l->iota_debug;
    sb_sprintf(&l->sb, ", !dbg !%zu", pos->iota);
}

static inline void llvm_debug_file_emit(LLVM *l, LLVM_Debug_File *file) {
    file->iota = ++l->iota_debug;
    sb_sprintf(&l->sb, "!%zu = !DIFile(filename: \"%s\", directory: \"\")\n", file->iota, file->path);
}

static_assert(COUNT_LLVM_NODES == 10, "");
static void llvm_node_emit(LLVM *l, LLVM_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case LLVM_NODE_ATOM: {
        LLVM_Node_Atom *atom = (LLVM_Node_Atom *) n;
        sb_sprintf(&l->sb, "%zu", atom->as.integer);
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

static_assert(COUNT_LLVM_TYPES == 3, "");
static void llvm_type_emit(LLVM *l, LLVM_Type type) {
    switch (type.kind) {
    case LLVM_TYPE_I0:
        sb_sprintf(&l->sb, "void");
        break;

    case LLVM_TYPE_I1:
        sb_sprintf(&l->sb, "i1");
        break;

    case LLVM_TYPE_I64:
        sb_sprintf(&l->sb, "i64");
        break;

    default:
        unreachable();
    }
}

static_assert(COUNT_LLVM_NODES == 10, "");
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
            llvm_type_emit(l, n->type);
            sb_push_cstr(&l->sb, " 0, ");
            llvm_node_emit(l, unary->value);
            break;

        case LLVM_UNARY_LNOT:
            sb_push_cstr(&l->sb, "icmp eq ");
            llvm_type_emit(l, n->type);
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
        llvm_type_emit(l, binary->lhs->type);
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
        llvm_type_emit(l, n->type);
        sb_push_cstr(&l->sb, ", ptr ");
        llvm_node_emit(l, load->ptr);

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
        // TODO: Align
    } break;

    case LLVM_NODE_STORE: {
        LLVM_Node_Store *store = (LLVM_Node_Store *) n;
        sb_push_cstr(&l->sb, "  store ");
        llvm_type_emit(l, store->value->type);
        sb_push(&l->sb, ' ');
        llvm_node_emit(l, store->value);
        sb_push_cstr(&l->sb, ", ptr ");
        llvm_node_emit(l, store->ptr);

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
        // TODO: Align
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
        llvm_type_emit(l, branch->condition->type);
        sb_push(&l->sb, ' ');
        llvm_node_emit(l, branch->condition);

        sb_push_cstr(&l->sb, ", ");
        llvm_node_emit(l, (LLVM_Node *) branch->consequence);

        sb_push_cstr(&l->sb, ", ");
        llvm_node_emit(l, (LLVM_Node *) branch->antecedence);

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_PRINT: {
        LLVM_Node_Print *print = (LLVM_Node_Print *) n;
        if (print->value->type.kind != LLVM_TYPE_I64) {
            sb_push_cstr(&l->sb, "  ");
            n->iota = ++l->iota_local;
            llvm_node_emit(l, n);
            sb_push_cstr(&l->sb, " = zext ");
            llvm_type_emit(l, print->value->type);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, print->value);
            sb_push_cstr(&l->sb, " to i64\n");
            print->value->iota = n->iota;
        }

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

void llvm_compile(LLVM *l) {
    l->debug_i32_type = ++l->iota_debug;
    l->debug_i64_type = ++l->iota_debug;
    l->debug_bool_type = ++l->iota_debug;

    sb_push_cstr(
        &l->sb,
        "@.iprint = private unnamed_addr constant [5 x i8] c\"%ld\\0A\\00\", align 1\n"
        "declare i32 @printf(ptr, ...)\n");

    if (l->vars.head) {
        sb_push(&l->sb, '\n');
        for (LLVM_Node *it = l->vars.head; it; it = it->next) {
            sb_sprintf(&l->sb, "@" SV_Fmt " = global ", SV_Arg(it->sv));
            llvm_type_emit(l, it->type);
            sb_sprintf(&l->sb, " zeroinitializer");

            llvm_debug_pos_emit(l, it->debug);
            sb_push(&l->sb, '\n');
            // TODO: Align
            // TODO: Booleans should be stored as `i8`
        }
    }

    l->debug_main_fn = ++l->iota_debug;
    sb_sprintf(&l->sb, "\ndefine i32 @main() !dbg !%zu {\n", l->debug_main_fn);
    for (LLVM_Node *it = l->body.head; it; it = it->next) {
        llvm_node_compile(l, it);
    }
    sb_push_cstr(&l->sb, "  ret i32 0\n}\n");

    sb_push(&l->sb, '\n');
    llvm_debug_file_emit(l, l->debug_file);

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
    const size_t debug_compilation_unit = ++l->iota_debug;
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

    const size_t debug_main_fn_type = ++l->iota_debug;
    sb_sprintf(
        &l->sb,
        "!%zu = distinct !DISubprogram("
        "name: \"main\", "
        "scope: !%zu, "
        "file: !%zu, "
        "line: %zu, "
        "scopeLine: %zu, "
        "type: !%zu, "
        "flags: DIFlagPrototyped, "
        "spFlags: DISPFlagDefinition, "
        "unit: !%zu)\n",
        l->debug_main_fn,
        l->debug_file->iota,
        l->debug_file->iota,
        (size_t) 1,
        (size_t) 1,
        debug_main_fn_type,
        debug_compilation_unit);

    sb_sprintf(&l->sb, "!%zu = !DIBasicType(name: \"bool\", size: 8, encoding: DW_ATE_boolean)\n", l->debug_bool_type);
    sb_sprintf(&l->sb, "!%zu = !DIBasicType(name: \"i32\", size: 32, encoding: DW_ATE_signed)\n", l->debug_i32_type);
    sb_sprintf(&l->sb, "!%zu = !DIBasicType(name: \"i64\", size: 64, encoding: DW_ATE_signed)\n", l->debug_i64_type);
    sb_sprintf(&l->sb, "!%zu = !DISubroutineType(types: !{!%zu})\n", debug_main_fn_type, l->debug_i32_type);

    for (LLVM_Node *it = l->vars.head; it; it = it->next) {
        const size_t info = ++l->iota_debug;
        sb_sprintf(
            &l->sb, "!%zu = !DIGlobalVariableExpression(var: !%zu, expr: !DIExpression())\n", it->debug->iota, info);

        size_t debug_type = 0;
        static_assert(COUNT_LLVM_TYPES == 3, "");
        switch (it->type.kind) {
        case LLVM_TYPE_I0:
            unreachable();
            break;

        case LLVM_TYPE_I1:
            debug_type = l->debug_bool_type;
            break;

        case LLVM_TYPE_I64:
            debug_type = l->debug_i64_type;
            break;

        default:
            unreachable();
            break;
        }

        sb_sprintf(
            &l->sb,
            "!%zu = distinct !DIGlobalVariable(name: \"" SV_Fmt "\", "
            "scope: !%zu, "
            "file: !%zu, "
            "line: %zu, "
            "type: !%zu, "
            "isLocal: false, "
            "isDefinition: true)\n",
            info,
            SV_Arg(it->sv),
            debug_compilation_unit,
            l->debug_file->iota,
            it->debug->row,
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
        sb_sprintf(
            &l->sb,
            "!%zu = !DILocation(line: %zu, column: %zu, scope: !%zu)\n",
            it->iota,
            it->row + 1,
            it->col + 1,
            l->debug_main_fn);
    }
}

LLVM_Type llvm_type_basic(LLVM_Type_Kind kind) {
    return (LLVM_Type) {.kind = kind};
}

LLVM_Node *llvm_atom_int(LLVM *l, LLVM_Type type, size_t n) {
    LLVM_Node_Atom *atom = (LLVM_Node_Atom *) llvm_node_alloc(l, LLVM_NODE_ATOM, type);
    atom->as.integer = n;
    return (LLVM_Node *) atom;
}

LLVM_Node_Block *llvm_block_new(LLVM *l) {
    return (LLVM_Node_Block *) llvm_node_alloc(l, LLVM_NODE_BLOCK, llvm_type_basic(LLVM_TYPE_I0));
}

LLVM_Node_Var *llvm_var_new(LLVM *l, SV name, LLVM_Type type) {
    LLVM_Node_Var *var = (LLVM_Node_Var *) llvm_node_alloc(l, LLVM_NODE_VAR, type);
    var->node.sv = name;
    var->node.debug = &var->debug;
    llvm_nodes_push(&l->vars, (LLVM_Node *) var);
    return var;
}

void llvm_var_debug_set_pos(LLVM *l, LLVM_Node_Var *var, size_t row, size_t col) {
    unused(l); // For symmetry
    var->debug.row = row;
    var->debug.col = col;
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
    llvm_nodes_push(&l->body, node);
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

    n->debug->next = l->debug_pos;
    l->debug_pos = n->debug;
}
