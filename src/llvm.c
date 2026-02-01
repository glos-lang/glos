#include "llvm.h"

typedef enum {
    LLVM_NODE_ATOM,
    LLVM_NODE_UNARY,
    LLVM_NODE_BINARY,

    LLVM_NODE_PRINT,
    COUNT_LLVM_NODES
} LLVM_Node_Kind;

struct LLVM_Node {
    LLVM_Node_Kind kind;

    LLVM_Type type;
    size_t    iota;

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

static_assert(COUNT_LLVM_NODES == 4, "");
static LLVM_Node *llvm_node_alloc(LLVM *l, LLVM_Node_Kind kind, LLVM_Type type) {
    static const size_t sizes[COUNT_LLVM_NODES] = {
        [LLVM_NODE_ATOM] = sizeof(LLVM_Node_Atom), // Prevent clang-format from messing this up
        [LLVM_NODE_UNARY] = sizeof(LLVM_Node_Unary),
        [LLVM_NODE_BINARY] = sizeof(LLVM_Node_Binary),

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

static void llvm_debug_pos_emit(LLVM *l, LLVM_Debug_Pos *pos) {
    pos->iota = l->iota_debug++;
    sb_sprintf(&l->sb, ", !dbg !%zu", pos->iota);
}

static void llvm_debug_file_emit(LLVM *l, LLVM_Debug_File *file) {
    file->iota = l->iota_debug++;
    sb_sprintf(&l->sb, "!%zu = !DIFile(filename: \"%s\", directory: \"\")\n", file->iota, file->path);
}

static_assert(COUNT_LLVM_NODES == 4, "");
static void llvm_node_emit(LLVM *l, LLVM_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case LLVM_NODE_ATOM: {
        LLVM_Node_Atom *atom = (LLVM_Node_Atom *) n;
        sb_sprintf(&l->sb, "%zu", atom->as.integer);
    } break;

    default:
        assert(n->iota);
        sb_sprintf(&l->sb, "%%%zu", n->iota);
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

static_assert(COUNT_LLVM_NODES == 4, "");
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

        static_assert(COUNT_LLVM_UNARYS == 2, "");
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

        static_assert(COUNT_LLVM_BINARYS == 5, "");
        switch (binary->kind) {
        case LLVM_BINARY_ADD:
            sb_push_cstr(&l->sb, "add ");
            llvm_type_emit(l, n->type);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, binary->lhs);
            sb_sprintf(&l->sb, ", ");
            llvm_node_emit(l, binary->rhs);
            break;

        case LLVM_BINARY_SUB:
            sb_push_cstr(&l->sb, "sub ");
            llvm_type_emit(l, n->type);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, binary->lhs);
            sb_sprintf(&l->sb, ", ");
            llvm_node_emit(l, binary->rhs);
            break;

        case LLVM_BINARY_MUL:
            sb_push_cstr(&l->sb, "mul ");
            llvm_type_emit(l, n->type);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, binary->lhs);
            sb_sprintf(&l->sb, ", ");
            llvm_node_emit(l, binary->rhs);
            break;

        case LLVM_BINARY_DIV:
            sb_push_cstr(&l->sb, "sdiv ");
            llvm_type_emit(l, n->type);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, binary->lhs);
            sb_sprintf(&l->sb, ", ");
            llvm_node_emit(l, binary->rhs);
            break;

        case LLVM_BINARY_MOD:
            sb_push_cstr(&l->sb, "srem ");
            llvm_type_emit(l, n->type);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, binary->lhs);
            sb_sprintf(&l->sb, ", ");
            llvm_node_emit(l, binary->rhs);
            break;

        default:
            unreachable();
            break;
        }

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

    case COUNT_LLVM_NODES:
        unreachable();
    }
}

void llvm_free(LLVM *l) {
    sb_free(&l->sb);
}

void llvm_compile(LLVM *l) {
    sb_push_cstr(
        &l->sb,
        "@.iprint = private unnamed_addr constant [5 x i8] c\"%ld\\0A\\00\", align 1\n"
        "declare i32 @printf(ptr, ...)\n");

    {
        sb_push(&l->sb, '\n');
        llvm_debug_file_emit(l, l->debug_file);
        sb_push(&l->sb, '\n');

        const size_t debug_version = l->iota_debug++;
        const size_t debug_info_version = l->iota_debug++;
        sb_sprintf(&l->sb, "!llvm.module.flags = !{!%zu, !%zu}\n", debug_version, debug_info_version);

#ifdef PLATFORM_X86_64_WINDOWS
        sb_sprintf(&l->sb, "!%zu = !{i32 2, !\"CodeView\", i32 1}\n", debug_version);
#else
        sb_sprintf(&l->sb, "!%zu = !{i32 7, !\"Dwarf Version\", i32 5}\n", debug_version);
#endif // PLATFORM_X86_64_WINDOWS
        sb_sprintf(&l->sb, "!%zu = !{i32 2, !\"Debug Info Version\", i32 3}\n", debug_info_version);

        const size_t debug_compilation_unit = l->iota_debug++;
        sb_push(&l->sb, '\n');
        sb_sprintf(&l->sb, "!llvm.dbg.cu = !{!%zu}\n", debug_compilation_unit);
        sb_sprintf(
            &l->sb,
            "!%zu = distinct !DICompileUnit("
            "language: DW_LANG_C11, "
            "file: !%zu, "
            "producer: \"glos\", "
            "isOptimized: false, "
            "runtimeVersion: 0, "
            "emissionKind: FullDebug, "
            "globals: !{}, "
            "splitDebugInlining: false, "
            "nameTableKind: None)\n",
            debug_compilation_unit,
            l->debug_file->iota);

        l->debug_main_fn = l->iota_debug++;
        const size_t debug_main_fn_type = l->iota_debug++;
        sb_sprintf(
            &l->sb,
            "\n"
            "!%zu = distinct !DISubprogram("
            "name: \"main\", "
            "scope: !%zu, "
            "file: !%zu, "
            "line: 1, "
            "type: !%zu, "
            "scopeLine: 1, "
            "flags: DIFlagPrototyped, "
            "spFlags: DISPFlagDefinition, "
            "unit: !%zu)\n",
            l->debug_main_fn,
            l->debug_file->iota,
            l->debug_file->iota,
            debug_main_fn_type,
            debug_compilation_unit);

        const size_t debug_i32_type = l->iota_debug++;
        sb_sprintf(&l->sb, "!%zu = !DISubroutineType(types: !{!%zu})\n", debug_main_fn_type, debug_i32_type);
        sb_sprintf(&l->sb, "!%zu = !DIBasicType(name: \"i32\", size: 32, encoding: DW_ATE_signed)\n", debug_i32_type);
    }

    sb_sprintf(&l->sb, "\ndefine i32 @main() !dbg !%zu {\n", l->debug_main_fn);
    for (LLVM_Node *it = l->body.head; it; it = it->next) {
        llvm_node_compile(l, it);
    }

    sb_push_cstr(
        &l->sb,
        "  ret i32 0\n"
        "}\n");

    if (l->debug_pos) {
        sb_push(&l->sb, '\n');
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
}

LLVM_Type llvm_type_basic(LLVM_Type_Kind kind) {
    return (LLVM_Type) {.kind = kind};
}

LLVM_Node *llvm_atom_int(LLVM *l, LLVM_Type type, size_t n) {
    LLVM_Node_Atom *atom = (LLVM_Node_Atom *) llvm_node_alloc(l, LLVM_NODE_ATOM, type);
    atom->as.integer = n;
    return (LLVM_Node *) atom;
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
