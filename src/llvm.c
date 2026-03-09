#include "llvm.h"
#include "basic.h"
#include <assert.h>
#include <stdbool.h>

typedef enum {
    LLVM_NODE_ATOM,
    LLVM_NODE_UNARY,
    LLVM_NODE_BINARY,

    LLVM_NODE_GEP,
    LLVM_NODE_LOAD,
    LLVM_NODE_STORE,
    LLVM_NODE_CAST,
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
    bool is_casted_to_ptr;
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
    LLVM_Node *base;
    LLVM_Type  base_type;

    bool is_field_index;
    union {
        size_t     field_index;
        LLVM_Node *slice_index;
    };
} LLVM_Node_GEP;

typedef struct {
    LLVM_Node  node;
    LLVM_Node *ptr;

    // Basically don't emit a load instruction when passing structures to functions
    bool is_dead;
} LLVM_Node_Load;

typedef struct {
    LLVM_Node  node;
    LLVM_Node *ptr;
    LLVM_Node *value;
} LLVM_Node_Store;

typedef struct {
    LLVM_Node  node;
    LLVM_Node *value;
} LLVM_Node_Cast;

typedef struct {
    LLVM_Node   node;
    LLVM_Node  *fn;
    LLVM_Node **args;
    size_t      args_count;

    LLVM_Node      *struct_return_memory;
    LLVM_Node_Load *struct_return_load;
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

typedef enum {
    LLVM_ABI_CLASS_VOID,
    LLVM_ABI_CLASS_DIRECT,
    LLVM_ABI_CLASS_INDIRECT,
    LLVM_ABI_CLASS_CONVERTED,
    COUNT_LLVM_ABI_CLASSES
} LLVM_ABI_Class_Kind;

typedef struct {
    LLVM_ABI_Class_Kind kind;

    LLVM_Type_Kind converted_parts[2];
    size_t         converted_parts_count;

    size_t indirect_iotas[2];
} LLVM_ABI_Class;

typedef struct {
    size_t int_registers;
} LLVM_ABI_Classifier;

struct LLVM_Node_Fn {
    LLVM_Node  node;
    LLVM_Nodes vars;

    LLVM_Nodes body;
    bool       is_extern;

    LLVM_Node_Var **args;
    LLVM_ABI_Class *args_classes;
    size_t          args_count;

    size_t indirect_return_iota;

    LLVM_Debug_Pos    debug;
    LLVM_Debug_Scope *debug_scope;
};

typedef enum {
    LLVM_NODE_VAR_INIT_INT,
    LLVM_NODE_VAR_INIT_NODE,
    LLVM_NODE_VAR_INIT_STRUCT,
    COUNT_LLVM_NODE_VAR_INITS,
} LLVM_Node_Var_Init_Kind;

struct LLVM_Node_Var_Init {
    LLVM_Node_Var_Init_Kind kind;

    LLVM_Type type;
    union {
        LLVM_Node *node;
        long       integer;
        struct {
            LLVM_Node_Var_Init **fields;
            size_t               fields_count;
        } structt;
    } as;

    LLVM_Node_Var_Init *next;
};

struct LLVM_Node_Var {
    LLVM_Node node;
    LLVM_Type type;
    SV        name;

    bool   is_arg;
    size_t arg_index;

    bool is_const;
    bool is_zeroed;
    bool is_extern;

    LLVM_Node_Var_Init *init;

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

static_assert(COUNT_LLVM_NODES == 15, "");
static LLVM_Node *llvm_node_alloc(LLVM *l, LLVM_Node_Kind kind, LLVM_Type type) {
    static const size_t sizes[COUNT_LLVM_NODES] = {
        [LLVM_NODE_ATOM] = sizeof(LLVM_Node_Atom), // Prevent clang-format from messing this up
        [LLVM_NODE_UNARY] = sizeof(LLVM_Node_Unary),
        [LLVM_NODE_BINARY] = sizeof(LLVM_Node_Binary),

        [LLVM_NODE_GEP] = sizeof(LLVM_Node_GEP),
        [LLVM_NODE_LOAD] = sizeof(LLVM_Node_Load),
        [LLVM_NODE_STORE] = sizeof(LLVM_Node_Store),
        [LLVM_NODE_CAST] = sizeof(LLVM_Node_Cast),
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

    if (!pos->iota) {
        pos->iota = ++l->iota_debug;
    }
    sb_sprintf(&l->sb, ", !dbg !%zu", pos->iota);
}

static inline void llvm_debug_file_emit(LLVM *l, LLVM_Debug_File *file) {
    file->iota = ++l->iota_debug;
    sb_sprintf(&l->sb, "!%zu = !DIFile(filename: \"%s\", directory: \"\")\n", file->iota, file->path);
}

static_assert(COUNT_LLVM_NODES == 15, "");
static void llvm_node_emit(LLVM *l, const LLVM_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case LLVM_NODE_ATOM: {
        LLVM_Node_Atom *atom = (LLVM_Node_Atom *) n;
        if (atom->is_zeroed) {
            sb_push_cstr(&l->sb, "zeroinitializer");
        } else {
            if (atom->is_casted_to_ptr) {
                sb_sprintf(&l->sb, "inttoptr (i64 %ld to ptr)", atom->as.integer);
            } else {
                sb_sprintf(&l->sb, "%ld", atom->as.integer);
            }
        }
    } break;

    case LLVM_NODE_BLOCK:
        sb_sprintf(&l->sb, "label %%.%zu", llvm_block_iota(l, (LLVM_Node_Block *) n));
        break;

    default:
        if (n->sv.count) {
            sb_sprintf(&l->sb, "@\"" SV_Fmt "\"", SV_Arg(n->sv));
        } else {
            assert(n->iota);
            sb_sprintf(&l->sb, "%%.%zu", n->iota);
        }
        break;
    }
}

static_assert(COUNT_LLVM_TYPES == 13, "");
static void llvm_type_emit(LLVM *l, LLVM_Type type, bool i1_to_i8) {
    switch (type.kind) {
    case LLVM_TYPE_I0:
        sb_push_cstr(&l->sb, "void");
        break;

    case LLVM_TYPE_I1:
        sb_push_cstr(&l->sb, i1_to_i8 ? "i8" : "i1");
        break;

    case LLVM_TYPE_I8:
    case LLVM_TYPE_U8:
        sb_push_cstr(&l->sb, "i8");
        break;

    case LLVM_TYPE_I16:
    case LLVM_TYPE_U16:
        sb_push_cstr(&l->sb, "i16");
        break;

    case LLVM_TYPE_I32:
    case LLVM_TYPE_U32:
        sb_push_cstr(&l->sb, "i32");
        break;

    case LLVM_TYPE_I64:
    case LLVM_TYPE_U64:
        sb_push_cstr(&l->sb, "i64");
        break;

    case LLVM_TYPE_PTR:
    case LLVM_TYPE_FN:
        sb_push_cstr(&l->sb, "ptr");
        break;

    case LLVM_TYPE_STRUCT:
        sb_sprintf(&l->sb, "%%struct." SV_Fmt, SV_Arg(type.structt.name));
        break;

    default:
        unreachable();
    }
}

static_assert(COUNT_LLVM_TYPES == 13, "");
static bool llvm_type_kind_is_signed(LLVM_Type_Kind kind) {
    switch (kind) {
    case LLVM_TYPE_I8:
    case LLVM_TYPE_I16:
    case LLVM_TYPE_I32:
    case LLVM_TYPE_I64:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_LLVM_TYPES == 13, "");
static LLVM_Type_Kind llvm_type_kind_to_signed(LLVM_Type_Kind kind) {
    switch (kind) {
    case LLVM_TYPE_U8:
        return LLVM_TYPE_I8;

    case LLVM_TYPE_U16:
        return LLVM_TYPE_I16;

    case LLVM_TYPE_U32:
        return LLVM_TYPE_I32;

    case LLVM_TYPE_U64:
        return LLVM_TYPE_I64;

    default:
        return kind;
    }
}

static size_t llvm_cast_compile(LLVM *l, const LLVM_Node *n, LLVM_Type_Kind to) {
    const LLVM_Type_Kind from_signed = llvm_type_kind_to_signed(n->type.kind);
    const LLVM_Type_Kind to_signed = llvm_type_kind_to_signed(to);
    if (from_signed == to_signed) {
        return n->iota;
    }

    if (n->kind == LLVM_NODE_ATOM) {
        ((LLVM_Node_Atom *) n)->is_casted_to_ptr = to == LLVM_TYPE_PTR;
        return n->iota;
    }

    if (from_signed == LLVM_TYPE_FN && to_signed == LLVM_TYPE_PTR) {
        return n->iota;
    }

    if (from_signed == LLVM_TYPE_PTR && to_signed == LLVM_TYPE_FN) {
        return n->iota;
    }

    const size_t temp = ++l->iota_local;
    sb_sprintf(&l->sb, "  %%.%zu = ", temp);

    if (n->type.kind == LLVM_TYPE_PTR || n->type.kind == LLVM_TYPE_FN) {
        // Pointer -> Integer
        sb_push_cstr(&l->sb, "ptrtoint");
    } else if (to == LLVM_TYPE_PTR || to == LLVM_TYPE_FN) {
        // Integer -> Pointer
        sb_push_cstr(&l->sb, "inttoptr");
    } else if (from_signed < to_signed) {
        // Lower -> Higher
        if (llvm_type_kind_is_signed(n->type.kind)) {
            sb_push_cstr(&l->sb, "sext");
        } else {
            sb_push_cstr(&l->sb, "zext");
        }
    } else if (from_signed > to_signed) {
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

    return temp;
}

static LLVM_Type_Kind llvm_int_type_kind_from_size(size_t size) {
    switch (size) {
    case 1:
        return LLVM_TYPE_I8;

    case 2:
        return LLVM_TYPE_I16;

    case 3:
    case 4:
        return LLVM_TYPE_I32;

    case 5:
    case 6:
    case 7:
    case 8:
        return LLVM_TYPE_I64;

    default:
        unreachable();
    }
}

static void llvm_abi_x86_64_linux_decide_chunk_sizes(LLVM_Type_Struct spec, size_t chunk_sizes[2], size_t offset) {
    for (size_t i = 0; i < spec.fields_count; i++) {
        const LLVM_Field *it = &spec.fields[i];
        const size_t      it_offset = offset + spec.fields_infos[i].offset;
        if (it->type.kind == LLVM_TYPE_STRUCT) {
            llvm_abi_x86_64_linux_decide_chunk_sizes(it->type.structt, chunk_sizes, it_offset);
        } else {
            const size_t chunk_index = it_offset / 8;
            assert(chunk_index >= 0 && chunk_index < 2);
            chunk_sizes[chunk_index] = chunk_sizes[chunk_index] ? 8 : llvm_type_info(it->type).size;
        }
    }
}

static LLVM_ABI_Class llvm_abi_classify(LLVM_ABI_Classifier *c, LLVM_Type type) {
    LLVM_ABI_Class arg_class = {0};

    const LLVM_Type_Info info = llvm_type_info(type);
    if (info.size == 0) {
        arg_class.kind = LLVM_ABI_CLASS_VOID;
        return arg_class;
    }

    if (type.kind != LLVM_TYPE_STRUCT) {
        c->int_registers++;
        arg_class.kind = LLVM_ABI_CLASS_DIRECT;
        return arg_class;
    }

    if (info.size <= 8) {
        c->int_registers++;
        arg_class.kind = LLVM_ABI_CLASS_CONVERTED;
        arg_class.converted_parts[arg_class.converted_parts_count++] = llvm_int_type_kind_from_size(info.size);
        return arg_class;
    }

#ifdef PLATFORM_X86_64_LINUX
    if (info.size > 8 && info.size <= 16) {
        if (c->int_registers + 2 <= 6) {
            c->int_registers += 2;
            arg_class.kind = LLVM_ABI_CLASS_CONVERTED;

            size_t chunk_sizes[2] = {0};
            llvm_abi_x86_64_linux_decide_chunk_sizes(type.structt, chunk_sizes, 0);
            arg_class.converted_parts[arg_class.converted_parts_count++] = llvm_int_type_kind_from_size(chunk_sizes[0]);
            arg_class.converted_parts[arg_class.converted_parts_count++] = llvm_int_type_kind_from_size(chunk_sizes[1]);

            return arg_class;
        }
    }
#endif // PLATFORM_X86_64_LINUX

    arg_class.kind = LLVM_ABI_CLASS_INDIRECT;
    return arg_class;
}

static void llvm_abi_converted_type_emit(LLVM *l, LLVM_ABI_Class abi_class) {
    if (abi_class.converted_parts_count > 1) {
        sb_push(&l->sb, '{');
    }

    for (size_t j = 0; j < abi_class.converted_parts_count; j++) {
        if (j) {
            sb_push_cstr(&l->sb, ", ");
        }

        llvm_type_emit(l, (LLVM_Type) {.kind = abi_class.converted_parts[j]}, false);
    }

    if (abi_class.converted_parts_count > 1) {
        sb_push(&l->sb, '}');
    }
}

static_assert(COUNT_LLVM_NODES == 15, "");
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

        static_assert(COUNT_LLVM_UNARYS == 4, "");
        switch (unary->kind) {
        case LLVM_UNARY_NEG:
            sb_push_cstr(&l->sb, "sub ");
            llvm_type_emit(l, n->type, false);
            sb_push_cstr(&l->sb, " 0, ");
            llvm_node_emit(l, unary->value);
            break;

        case LLVM_UNARY_BNOT:
            sb_push_cstr(&l->sb, "xor ");
            llvm_type_emit(l, n->type, false);
            sb_push_cstr(&l->sb, " -1, ");
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

        size_t ptrtoint_lhs = 0;
        size_t ptrtoint_rhs = 0;
        bool   is_ptr_arithmetic = false;
        if (n->type.kind == LLVM_TYPE_PTR && (binary->kind == LLVM_BINARY_ADD || binary->kind == LLVM_BINARY_SUB)) {
            is_ptr_arithmetic = true;
            ptrtoint_lhs = llvm_cast_compile(l, binary->lhs, LLVM_TYPE_I64);
            ptrtoint_rhs = llvm_cast_compile(l, binary->rhs, LLVM_TYPE_I64);
        }

        n->iota = ++l->iota_local;
        sb_push_cstr(&l->sb, "  ");
        llvm_node_emit(l, n);
        sb_push_cstr(&l->sb, " = ");

        static_assert(COUNT_LLVM_BINARYS == 16, "");
        typedef struct {
            const char *i;
            const char *u;
        } Op;

        static const Op ops[COUNT_LLVM_BINARYS] = {
            [LLVM_BINARY_ADD] = {.i = "add"},
            [LLVM_BINARY_SUB] = {.i = "sub"},
            [LLVM_BINARY_MUL] = {.i = "mul"},
            [LLVM_BINARY_DIV] = {.i = "sdiv", .u = "udiv"},
            [LLVM_BINARY_MOD] = {.i = "srem", .u = "urem"},

            [LLVM_BINARY_SHL] = {.i = "shl"},
            [LLVM_BINARY_SHR] = {.i = "ashr", .u = "lshr"},
            [LLVM_BINARY_BOR] = {.i = "or"},
            [LLVM_BINARY_BAND] = {.i = "and"},

            [LLVM_BINARY_GT] = {.i = "icmp sgt", .u = "icmp ugt"},
            [LLVM_BINARY_GE] = {.i = "icmp sge", .u = "icmp uge"},
            [LLVM_BINARY_LT] = {.i = "icmp slt", .u = "icmp ult"},
            [LLVM_BINARY_LE] = {.i = "icmp sle", .u = "icmp ule"},
            [LLVM_BINARY_EQ] = {.i = "icmp eq"},
            [LLVM_BINARY_NE] = {.i = "icmp ne"},
        };

        const Op op = ops[binary->kind];
        assert(op.i);

        if (op.u && !llvm_type_kind_is_signed(binary->lhs->type.kind)) {
            sb_sprintf(&l->sb, "%s ", op.u);
        } else {
            sb_sprintf(&l->sb, "%s ", op.i);
        }

        if (is_ptr_arithmetic) {
            sb_push_cstr(&l->sb, "i64");
        } else {
            llvm_type_emit(l, binary->lhs->type, false);
        }
        sb_push(&l->sb, ' ');

        if (ptrtoint_lhs) {
            sb_sprintf(&l->sb, "%%.%zu", ptrtoint_lhs);
        } else {
            llvm_node_emit(l, binary->lhs);
        }

        sb_sprintf(&l->sb, ", ");

        if (ptrtoint_rhs) {
            sb_sprintf(&l->sb, "%%.%zu", ptrtoint_rhs);
        } else {
            llvm_node_emit(l, binary->rhs);
        }

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');

        if (is_ptr_arithmetic) {
            n->type.kind = LLVM_TYPE_I64;
            n->iota = llvm_cast_compile(l, n, LLVM_TYPE_PTR);
            n->type.kind = LLVM_TYPE_PTR;
        }
    } break;

    case LLVM_NODE_GEP: {
        LLVM_Node_GEP *gep = (LLVM_Node_GEP *) n;
        n->iota = ++l->iota_local;

        sb_push_cstr(&l->sb, "  ");
        llvm_node_emit(l, n);
        sb_push_cstr(&l->sb, " = getelementptr inbounds ");
        llvm_type_emit(l, gep->base_type, false);
        sb_push_cstr(&l->sb, ", ptr ");
        llvm_node_emit(l, gep->base);
        if (gep->is_field_index) {
            sb_sprintf(&l->sb, ", i32 0, i32 %zu", gep->field_index);
        } else {
            sb_push_cstr(&l->sb, ", ");
            llvm_type_emit(l, gep->slice_index->type, false);
            sb_push(&l->sb, ' ');
            llvm_node_emit(l, gep->slice_index);
        }

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_LOAD: {
        LLVM_Node_Load *load = (LLVM_Node_Load *) n;
        if (!load->is_dead) {
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
                // `n` is bool. It cannot be a variable. Therefore it is safe to modify
                n->type.kind = LLVM_TYPE_I8;
                n->iota = llvm_cast_compile(l, n, LLVM_TYPE_I1);
                n->type.kind = LLVM_TYPE_I1;
            }
        }
    } break;

    case LLVM_NODE_STORE: {
        LLVM_Node_Store *store = (LLVM_Node_Store *) n;
        if (store->value->type.kind == LLVM_TYPE_I1) {
            // `store->value` is bool. It cannot be a variable. Therefore it is safe to modify
            store->value->iota = llvm_cast_compile(l, store->value, LLVM_TYPE_I8);
            store->value->type.kind = LLVM_TYPE_I8;
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

    case LLVM_NODE_CAST: {
        LLVM_Node_Cast *cast = (LLVM_Node_Cast *) n;
        n->iota = llvm_cast_compile(l, cast->value, n->type.kind);
        if (!n->iota) {
            // It was a variable
            n->sv = cast->value->sv;
        }
    } break;

    case LLVM_NODE_CALL: {
        const LLVM_ABI_Class *checkpoint = temp_alloc(0);
        {
            LLVM_Node_Call *call = (LLVM_Node_Call *) n;
            if (call->struct_return_memory) {
                if (call->struct_return_load) {
                    n->debug = call->struct_return_load->node.debug;
                    call->struct_return_load->node.debug = NULL;
                } else {
                    n->debug = call->struct_return_memory->debug;
                    call->struct_return_memory->debug = NULL;
                }
            }

            LLVM_ABI_Classifier classifier = {0};
            LLVM_ABI_Class      return_class = llvm_abi_classify(&classifier, n->type);

            classifier = (LLVM_ABI_Classifier) {0};
            if (return_class.kind == LLVM_ABI_CLASS_INDIRECT) {
                classifier.int_registers++;
            }

            for (size_t i = 0; i < call->args_count; i++) {
                LLVM_Node     *arg = call->args[i];
                LLVM_ABI_Class arg_class = llvm_abi_classify(&classifier, arg->type);

                if (arg->type.kind == LLVM_TYPE_STRUCT) {
                    if (arg_class.kind == LLVM_ABI_CLASS_CONVERTED) {
                        assert(arg->kind == LLVM_NODE_LOAD);
                        LLVM_Node *arg_ptr = ((LLVM_Node_Load *) arg)->ptr;

                        for (size_t i = 0; i < arg_class.converted_parts_count; i++) {
                            const size_t temp = ++l->iota_local;
                            sb_sprintf(&l->sb, "  %%.%zu = getelementptr inbounds i8, ptr ", temp);
                            llvm_node_emit(l, arg_ptr);
                            sb_sprintf(&l->sb, ", i64 %zu\n", i * 8);

                            arg_class.indirect_iotas[i] = ++l->iota_local;
                            sb_sprintf(&l->sb, "  %%.%zu = load ", arg_class.indirect_iotas[i]);

                            const LLVM_Type type = {.kind = arg_class.converted_parts[i]};
                            llvm_type_emit(l, type, false);
                            sb_sprintf(&l->sb, ", ptr %%.%zu, align %zu\n", temp, llvm_type_info(type).align);
                        }
                    }

                    temp_clone(&arg_class, sizeof(arg_class));
                }
            }

            sb_push_cstr(&l->sb, "  ");
            if (return_class.kind != LLVM_ABI_CLASS_VOID && return_class.kind != LLVM_ABI_CLASS_INDIRECT) {
                n->iota = ++l->iota_local;
                llvm_node_emit(l, n);
                sb_push_cstr(&l->sb, " = ");
            }

            sb_push_cstr(&l->sb, "call ");
            if (call->struct_return_memory) {
                switch (return_class.kind) {
                case LLVM_ABI_CLASS_VOID:
                    sb_push_cstr(&l->sb, "void");
                    break;

                case LLVM_ABI_CLASS_DIRECT:
                    unreachable();

                case LLVM_ABI_CLASS_INDIRECT:
                    sb_push_cstr(&l->sb, "void");
                    break;

                case LLVM_ABI_CLASS_CONVERTED:
                    llvm_abi_converted_type_emit(l, return_class);
                    break;

                default:
                    unreachable();
                }
            } else {
                llvm_type_emit(l, n->type, false);
            }

            sb_push(&l->sb, ' ');
            llvm_node_emit(l, call->fn);
            sb_push(&l->sb, '(');

            if (return_class.kind == LLVM_ABI_CLASS_INDIRECT) {
                sb_push_cstr(&l->sb, "ptr sret(");
                llvm_type_emit(l, n->type, false);
                sb_sprintf(&l->sb, ") align %zu ", llvm_type_info(n->type).align);
                llvm_node_emit(l, call->struct_return_memory);

                if (call->args_count) {
                    sb_push_cstr(&l->sb, ", ");
                }
            }

            size_t arg_class_iota = 0;
            for (size_t i = 0; i < call->args_count; i++) {
                if (i) {
                    sb_push_cstr(&l->sb, ", ");
                }

                LLVM_Node *arg = call->args[i];
                if (arg->type.kind == LLVM_TYPE_STRUCT) {
                    LLVM_ABI_Class arg_class = checkpoint[arg_class_iota++];

                    static_assert(COUNT_LLVM_ABI_CLASSES == 4, "");
                    switch (arg_class.kind) {
                    case LLVM_ABI_CLASS_VOID:
                        unreachable();

                    case LLVM_ABI_CLASS_DIRECT:
                        unreachable();

                    case LLVM_ABI_CLASS_INDIRECT:
                        sb_push_cstr(&l->sb, "ptr noundef byval(");
                        llvm_type_emit(l, arg->type, false);
                        sb_sprintf(&l->sb, ") align %zu ", llvm_type_info(arg->type).align);
                        assert(arg->kind == LLVM_NODE_LOAD);
                        llvm_node_emit(l, ((LLVM_Node_Load *) arg)->ptr);
                        break;

                    case LLVM_ABI_CLASS_CONVERTED:
                        for (size_t i = 0; i < arg_class.converted_parts_count; i++) {
                            if (i) {
                                sb_push_cstr(&l->sb, ", ");
                            }
                            llvm_type_emit(l, (LLVM_Type) {.kind = arg_class.converted_parts[i]}, false);
                            sb_sprintf(&l->sb, " %%.%zu", arg_class.indirect_iotas[i]);
                        }
                        break;

                    default:
                        unreachable();
                    }
                } else {
                    llvm_type_emit(l, arg->type, false);
                    sb_push(&l->sb, ' ');
                    llvm_node_emit(l, arg);
                }
            }
            sb_push(&l->sb, ')');

            llvm_debug_pos_emit(l, n->debug);
            sb_push(&l->sb, '\n');

            if (call->struct_return_memory) {
                switch (return_class.kind) {
                case LLVM_ABI_CLASS_VOID:
                    // Pass
                    break;

                case LLVM_ABI_CLASS_DIRECT:
                    unreachable();

                case LLVM_ABI_CLASS_INDIRECT:
                    // Pass
                    break;

                case LLVM_ABI_CLASS_CONVERTED:
                    if (return_class.converted_parts_count == 1) {
                        const LLVM_Type type = {.kind = return_class.converted_parts[0]};
                        sb_push_cstr(&l->sb, "  store ");
                        llvm_type_emit(l, type, false);
                        sb_push(&l->sb, ' ');
                        llvm_node_emit(l, n);

                        sb_push_cstr(&l->sb, ", ptr ");
                        llvm_node_emit(l, call->struct_return_memory);
                        sb_sprintf(&l->sb, ", align %zu\n", llvm_type_info(type).align);
                    } else {
                        for (size_t i = 0; i < return_class.converted_parts_count; i++) {
                            const size_t dst = ++l->iota_local;
                            sb_sprintf(&l->sb, "  %%.%zu = getelementptr inbounds i8, ptr ", dst);
                            llvm_node_emit(l, call->struct_return_memory);
                            sb_sprintf(&l->sb, ", i64 %zu\n", i * 8);

                            const size_t src = ++l->iota_local;
                            sb_sprintf(&l->sb, "  %%.%zu = extractvalue ", src);
                            llvm_abi_converted_type_emit(l, return_class);
                            sb_push(&l->sb, ' ');
                            llvm_node_emit(l, n);
                            sb_sprintf(&l->sb, ", %zu\n", i);

                            const LLVM_Type type = {.kind = return_class.converted_parts[i]};
                            sb_push_cstr(&l->sb, "  store ");
                            llvm_type_emit(l, type, false);
                            sb_sprintf(
                                &l->sb, " %%.%zu, ptr %%.%zu, align %zu\n", src, dst, llvm_type_info(type).align);
                        }
                    }
                    break;

                default:
                    unreachable();
                }
            }
        }
        temp_reset(checkpoint);
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

        if (n->type.kind == LLVM_TYPE_STRUCT) {
            LLVM_ABI_Classifier classifier = {0};
            LLVM_ABI_Class      return_class = llvm_abi_classify(&classifier, n->type);

            assert(returnn->value);
            assert(returnn->value->kind == LLVM_NODE_LOAD);
            LLVM_Node *return_ptr = ((LLVM_Node_Load *) returnn->value)->ptr;

            static_assert(COUNT_LLVM_ABI_CLASSES == 4, "");
            switch (return_class.kind) {
            case LLVM_ABI_CLASS_VOID:
                unreachable();

            case LLVM_ABI_CLASS_DIRECT:
                unreachable();

            case LLVM_ABI_CLASS_INDIRECT:
                sb_sprintf(&l->sb, "  call void @llvm.memcpy.p0.p0.i64(ptr %%.%zu, ptr ", l->fn->indirect_return_iota);
                llvm_node_emit(l, return_ptr);
                sb_sprintf(
                    &l->sb,
                    ", i64 %zu, i1 false)\n"
                    "  ret void",
                    llvm_type_info(n->type).size);
                break;

            case LLVM_ABI_CLASS_CONVERTED: {
                const size_t temp = ++l->iota_local;
                sb_sprintf(&l->sb, "  %%.%zu = load ", temp);
                llvm_abi_converted_type_emit(l, return_class);
                sb_push_cstr(&l->sb, ", ptr ");
                llvm_node_emit(l, return_ptr);
                sb_sprintf(&l->sb, ", align %zu", llvm_type_info(n->type).align);
                llvm_debug_pos_emit(l, n->debug);
                sb_push(&l->sb, '\n');

                sb_push_cstr(&l->sb, "  ret ");
                llvm_abi_converted_type_emit(l, return_class);
                sb_sprintf(&l->sb, " %%.%zu", temp);
            } break;

            default:
                unreachable();
            }
        } else {
            if (returnn->value) {
                sb_push_cstr(&l->sb, "  ret ");
                llvm_type_emit(l, n->type, false);
                sb_push(&l->sb, ' ');
                llvm_node_emit(l, returnn->value);
            } else {
                sb_push_cstr(&l->sb, "  ret void");
            }
        }

        llvm_debug_pos_emit(l, n->debug);
        sb_push(&l->sb, '\n');
    } break;

    case LLVM_NODE_PRINT: {
        LLVM_Node_Print *print = (LLVM_Node_Print *) n;

        const bool   is_signed = llvm_type_kind_is_signed(print->value->type.kind);
        const size_t value = llvm_cast_compile(l, print->value, is_signed ? LLVM_TYPE_I64 : LLVM_TYPE_U64);

        n->iota = ++l->iota_local;
        sb_push_cstr(&l->sb, "  ");
        llvm_node_emit(l, n);
        sb_sprintf(&l->sb, " = call i32 (ptr, ...) @printf(ptr @.%cprint, i64 ", is_signed ? 'i' : 'u');

        if (value) {
            sb_sprintf(&l->sb, "%%.%zu", value);
        } else {
            // `print->value` is an atom
            llvm_node_emit(l, print->value);
        }

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
    da_free(&l->structs);
}

static size_t llvm_type_debug_compile(LLVM *l, LLVM_Type *type);

static size_t llvm_type_fn_debug_compile(LLVM *l, LLVM_Type *type) {
    assert(type->kind == LLVM_TYPE_FN);

    if (type->fn.returnn->kind != LLVM_TYPE_I0) {
        llvm_type_debug_compile(l, type->fn.returnn);
    }

    for (size_t i = 0; i < type->fn.args_count; i++) {
        llvm_type_debug_compile(l, &type->fn.args[i]);
    }

    type->debug = ++l->iota_debug;
    sb_sprintf(&l->sb, "!%zu = !DISubroutineType(types: !{", type->debug);

    if (type->fn.returnn->kind != LLVM_TYPE_I0) {
        sb_sprintf(&l->sb, "!%zu", type->fn.returnn->debug);
        if (type->fn.args_count) {
            sb_push_cstr(&l->sb, ", ");
        }
    }

    for (size_t i = 0; i < type->fn.args_count; i++) {
        if (i) {
            sb_push_cstr(&l->sb, ", ");
        }
        sb_sprintf(&l->sb, "!%zu", type->fn.args[i].debug);
    }

    sb_push_cstr(&l->sb, "})\n");
    return type->debug;
}

static_assert(COUNT_LLVM_TYPES == 13, "");
static size_t llvm_type_debug_compile(LLVM *l, LLVM_Type *type) {
    switch (type->kind) {
    case LLVM_TYPE_I0:
        unreachable();
        break;

    case LLVM_TYPE_I1:
    case LLVM_TYPE_I8:
    case LLVM_TYPE_I16:
    case LLVM_TYPE_I32:
    case LLVM_TYPE_I64:
    case LLVM_TYPE_U8:
    case LLVM_TYPE_U16:
    case LLVM_TYPE_U32:
    case LLVM_TYPE_U64: {
        size_t *debug = &l->basic_type_debugs[type->kind];
        if (!*debug) {
            *debug = ++l->iota_debug;

            typedef struct {
                const char *name;
                const char *encoding;
            } Desc;

            static const Desc descs[COUNT_LLVM_TYPES] = {
                [LLVM_TYPE_I1] = {.name = "bool", .encoding = "DW_ATE_boolean"},

                [LLVM_TYPE_I8] = {.name = "i8", .encoding = "DW_ATE_signed"},
                [LLVM_TYPE_I16] = {.name = "i16", .encoding = "DW_ATE_signed"},
                [LLVM_TYPE_I32] = {.name = "i32", .encoding = "DW_ATE_signed"},
                [LLVM_TYPE_I64] = {.name = "i64", .encoding = "DW_ATE_signed"},

                [LLVM_TYPE_U8] = {.name = "u8", .encoding = "DW_ATE_unsigned"},
                [LLVM_TYPE_U16] = {.name = "u16", .encoding = "DW_ATE_unsigned"},
                [LLVM_TYPE_U32] = {.name = "u32", .encoding = "DW_ATE_unsigned"},
                [LLVM_TYPE_U64] = {.name = "u64", .encoding = "DW_ATE_unsigned"},
            };

            const Desc desc = descs[type->kind];
            assert(desc.name);

            sb_sprintf(
                &l->sb,
                "!%zu = !DIBasicType(name: \"%s\", size: %zu, encoding: %s)\n",
                *debug,
                desc.name,
                llvm_type_info(*type).size * 8,
                desc.encoding);
        }
        type->debug = *debug;
    } break;

    case LLVM_TYPE_PTR:
        if (type->ptr.type) {
            llvm_type_debug_compile(l, type->ptr.type);
            type->debug = ++l->iota_debug;
            sb_sprintf(
                &l->sb,
                "!%zu = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !%zu)\n",
                type->debug,
                type->ptr.type->debug);
        } else {
            type->debug = ++l->iota_debug;
            sb_sprintf(&l->sb, "!%zu = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null)\n", type->debug);
        }
        break;

    case LLVM_TYPE_FN: {
        llvm_type_fn_debug_compile(l, type);
        const size_t debug = ++l->iota_debug;
        sb_sprintf(&l->sb, "!%zu = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !%zu)\n", debug, type->debug);
        type->debug = debug;
    } break;

    case LLVM_TYPE_STRUCT: {
        type->debug = ++l->iota_debug;

        const LLVM_Type_Info   info = llvm_type_info(*type); // Pre-calculate offsets
        const LLVM_Type_Struct spec = type->structt;
        for (size_t i = 0; i < spec.fields_count; i++) {
            LLVM_Field      *field = &spec.fields[i];
            LLVM_Field_Info *field_info = &spec.fields_infos[i];
            llvm_type_debug_compile(l, &field->type);

            field_info->debug = ++l->iota_debug;
            sb_sprintf(
                &l->sb,
                "!%zu = !DIDerivedType("
                "tag: DW_TAG_member, "
                "name: \"" SV_Fmt "\", "
                "scope: !%zu, "
                "file: !%zu, "
                "baseType: !%zu, "
                "size: %zu, "
                "offset: %zu)\n",
                field_info->debug,
                SV_Arg(field->name),
                type->debug,
                l->debug_file->iota,
                field->type.debug,
                field_info->size * 8,
                field_info->offset * 8);
        }

        sb_sprintf(
            &l->sb,
            "!%zu = !DICompositeType("
            "tag: DW_TAG_structure_type, "
            "file: !%zu, "
            "size: %zu, "
            "elements: !{",
            type->debug,
            l->debug_file->iota,
            info.size * 8);

        for (size_t i = 0; i < spec.fields_count; i++) {
            if (i) {
                sb_push_cstr(&l->sb, ", ");
            }
            sb_sprintf(&l->sb, "!%zu", spec.fields_infos[i].debug);
        }
        sb_sprintf(&l->sb, "})\n");
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
    if (!pos->iota) {
        return;
    }

    llvm_debug_scope_compile(l, pos->scope);
    sb_sprintf(
        &l->sb,
        "!%zu = !DILocation(line: %zu, column: %zu, scope: !%zu)\n",
        pos->iota,
        pos->row + 1,
        pos->col + 1,
        pos->scope->iota);
}

static_assert(COUNT_LLVM_NODE_VAR_INITS == 3, "");
static void llvm_var_init_emit(LLVM *l, LLVM_Node_Var_Init *init) {
    switch (init->kind) {
    case LLVM_NODE_VAR_INIT_INT:
        llvm_type_emit(l, init->type, true);
        if (init->type.kind == LLVM_TYPE_PTR) {
            sb_sprintf(&l->sb, " inttoptr (i64 %ld to ptr)", init->as.integer);
        } else {
            sb_sprintf(&l->sb, " %ld", init->as.integer);
        }
        break;

    case LLVM_NODE_VAR_INIT_NODE:
        sb_push_cstr(&l->sb, "ptr ");
        llvm_node_emit(l, init->as.node);
        break;

    case LLVM_NODE_VAR_INIT_STRUCT:
        llvm_type_emit(l, init->type, true);

        sb_push_cstr(&l->sb, " {");
        for (size_t i = 0; i < init->as.structt.fields_count; i++) {
            if (i) {
                sb_push_cstr(&l->sb, ", ");
            }
            llvm_var_init_emit(l, init->as.structt.fields[i]);
        }
        sb_push(&l->sb, '}');
        break;

    default:
        unreachable();
        break;
    }
}

void llvm_compile(LLVM *l) {
    assert(l->main_fn);
    const size_t debug_compilation_unit = ++l->iota_debug;

    llvm_debug_file_emit(l, l->debug_file);
    sb_push_cstr(
        &l->sb,
        "\n"
        "@.iprint = private unnamed_addr constant [5 x i8] c\"%ld\\0A\\00\", align 1\n"
        "@.uprint = private unnamed_addr constant [5 x i8] c\"%zu\\0A\\00\", align 1\n"
        "declare i32 @printf(ptr, ...)\n");

    if (l->structs.count) {
        sb_push(&l->sb, '\n');
        for (size_t i = 0; i < l->structs.count; i++) {
            LLVM_Type_Struct it = l->structs.data[i].structt;
            sb_sprintf(&l->sb, "%%struct." SV_Fmt " = type {", SV_Arg(it.name));
            for (size_t j = 0; j < it.fields_count; j++) {
                if (j) {
                    sb_push_cstr(&l->sb, ", ");
                }
                llvm_type_emit(l, it.fields[j].type, true);
            }
            sb_push_cstr(&l->sb, "}\n");
        }
    }

    if (l->vars.head) {
        sb_push(&l->sb, '\n');
        for (LLVM_Node *it = l->vars.head; it; it = it->next) {
            LLVM_Node_Var *var = (LLVM_Node_Var *) it;

            sb_sprintf(&l->sb, "@\"" SV_Fmt "\" = ", SV_Arg(it->sv));
            if (var->is_const) {
                sb_push_cstr(&l->sb, "private unnamed_addr constant ");
            } else if (var->is_extern) {
                sb_push_cstr(&l->sb, "external global ");
            } else {
                sb_push_cstr(&l->sb, "global ");
            }

            if (var->init) {
                llvm_var_init_emit(l, var->init);
            } else {
                llvm_type_emit(l, var->type, true);
                if (!var->is_extern) {
                    sb_sprintf(&l->sb, " zeroinitializer");
                }
            }
            sb_sprintf(&l->sb, ", align %zu", llvm_type_info(var->type).align);

            llvm_debug_pos_emit(l, it->debug);
            sb_push(&l->sb, '\n');
        }
    }

    for (LLVM_Node *it = l->fns.head; it; it = it->next) {
        LLVM_Node_Fn *fn = (LLVM_Node_Fn *) it;
        l->fn = fn;

        fn->debug.iota = ++l->iota_debug;
        l->iota_local = 0;

        if (fn->is_extern) {
            sb_push_cstr(&l->sb, "\ndeclare ");
        } else {
            sb_push_cstr(&l->sb, "\ndefine ");
        }

        LLVM_ABI_Classifier classifier = {0};

        LLVM_Type      return_type = *fn->node.type.fn.returnn;
        LLVM_ABI_Class return_class = llvm_abi_classify(&classifier, return_type);

        static_assert(COUNT_LLVM_ABI_CLASSES == 4, "");
        switch (return_class.kind) {
        case LLVM_ABI_CLASS_VOID:
            sb_push_cstr(&l->sb, "void");
            break;

        case LLVM_ABI_CLASS_DIRECT:
            llvm_type_emit(l, return_type, false);
            break;

        case LLVM_ABI_CLASS_INDIRECT:
            sb_push_cstr(&l->sb, "void");
            break;

        case LLVM_ABI_CLASS_CONVERTED:
            llvm_abi_converted_type_emit(l, return_class);
            break;

        default:
            unreachable();
            break;
        }

        sb_sprintf(&l->sb, " @\"" SV_Fmt "\"(", SV_Arg(it->sv));

        classifier = (LLVM_ABI_Classifier) {0};
        if (return_class.kind == LLVM_ABI_CLASS_INDIRECT) {
            sb_push_cstr(&l->sb, "ptr sret(");
            llvm_type_emit(l, return_type, false);
            sb_sprintf(&l->sb, ") align %zu", llvm_type_info(return_type).align);
            if (!fn->is_extern) {
                fn->indirect_return_iota = ++l->iota_local;
                sb_sprintf(&l->sb, " %%.%zu", fn->indirect_return_iota);
            }

            if (fn->args_count) {
                sb_push_cstr(&l->sb, ", ");
            }

            classifier.int_registers++;
        }

        for (size_t i = 0; i < fn->args_count; i++) {
            if (i > 0) {
                sb_push_cstr(&l->sb, ", ");
            }

            LLVM_Type      arg_type = fn->node.type.fn.args[i];
            LLVM_ABI_Class arg_class = llvm_abi_classify(&classifier, arg_type);

            static_assert(COUNT_LLVM_ABI_CLASSES == 4, "");
            switch (arg_class.kind) {
            case LLVM_ABI_CLASS_VOID:
                unreachable();

            case LLVM_ABI_CLASS_DIRECT:
                llvm_type_emit(l, arg_type, false);
                if (!fn->is_extern) {
                    sb_sprintf(&l->sb, " %%a%zu", i);
                }
                break;

            case LLVM_ABI_CLASS_INDIRECT:
                sb_push_cstr(&l->sb, "ptr noundef byval(");
                llvm_type_emit(l, arg_type, false);
                sb_sprintf(&l->sb, ") align %zu", llvm_type_info(arg_type).align);
                if (!fn->is_extern) {
                    arg_class.indirect_iotas[0] = ++l->iota_local;
                    sb_sprintf(&l->sb, " %%.%zu", arg_class.indirect_iotas[0]);
                }
                break;

            case LLVM_ABI_CLASS_CONVERTED:
                for (size_t j = 0; j < arg_class.converted_parts_count; j++) {
                    if (j) {
                        sb_push_cstr(&l->sb, ", ");
                    }

                    llvm_type_emit(l, (LLVM_Type) {.kind = arg_class.converted_parts[j]}, false);
                    if (!fn->is_extern) {
                        sb_sprintf(&l->sb, " %%a%zu_%zu", i, j);
                    }
                }
                break;

            default:
                unreachable();
            }

            fn->args_classes[i] = arg_class;
        }

        sb_push(&l->sb, ')');
        if (fn->is_extern) {
            sb_push(&l->sb, '\n');
            continue;
        }

        if (fn != l->main_fn) {
            sb_sprintf(&l->sb, " #0 !dbg !%zu", fn->debug.iota);
        }
        sb_push_cstr(&l->sb, " {\n");

        for (LLVM_Node *n = fn->vars.head; n; n = n->next) {
            LLVM_Node_Var *var = (LLVM_Node_Var *) n;

            if (var->is_arg && fn->args_classes[var->arg_index].kind == LLVM_ABI_CLASS_INDIRECT) {
                n->iota = fn->args_classes[var->arg_index].indirect_iotas[0];
            } else {
                n->iota = ++l->iota_local;
                sb_push_cstr(&l->sb, "  ");
                llvm_node_emit(l, n);
                sb_push_cstr(&l->sb, " = alloca ");
                llvm_type_emit(l, var->type, true);
                sb_sprintf(&l->sb, ", align %zu\n", llvm_type_info(var->type).align);
            }

            if (var->is_arg) {
                if (var->type.kind == LLVM_TYPE_I1) {
                    const size_t temp = ++l->iota_local;
                    sb_sprintf(&l->sb, "  %%.%zu = zext i1 %%a%zu to i8\n", temp, var->arg_index);
                    sb_sprintf(&l->sb, "  store i8 %%.%zu, ptr ", temp);
                    llvm_node_emit(l, n);
                    sb_push_cstr(&l->sb, ", align 1\n");
                } else {
                    const LLVM_ABI_Class arg_class = fn->args_classes[var->arg_index];

                    static_assert(COUNT_LLVM_ABI_CLASSES == 4, "");
                    switch (arg_class.kind) {
                    case LLVM_ABI_CLASS_VOID:
                        todo(); // TODO: What to do for void parameters
                        break;

                    case LLVM_ABI_CLASS_DIRECT:
                        sb_push_cstr(&l->sb, "  store ");
                        llvm_type_emit(l, var->type, false);
                        sb_sprintf(&l->sb, " %%a%zu, ptr ", var->arg_index);
                        llvm_node_emit(l, n);
                        sb_push(&l->sb, '\n');
                        break;

                    case LLVM_ABI_CLASS_INDIRECT:
                        // Pass
                        break;

                    case LLVM_ABI_CLASS_CONVERTED:
                        for (size_t i = 0; i < arg_class.converted_parts_count; i++) {
                            const size_t temp = ++l->iota_local;
                            sb_sprintf(
                                &l->sb,
                                "  %%.%zu = getelementptr inbounds i8, ptr %%.%zu, i64 %zu\n",
                                temp,
                                n->iota,
                                i * 8);

                            const LLVM_Type type = {.kind = arg_class.converted_parts[i]};
                            sb_push_cstr(&l->sb, "  store ");
                            llvm_type_emit(l, type, false);
                            sb_sprintf(
                                &l->sb,
                                " %%a%zu_%zu, ptr %%.%zu, align %zu\n",
                                var->arg_index,
                                i,
                                temp,
                                llvm_type_info(type).align);
                        }
                        break;

                    default:
                        unreachable();
                    }
                }
            } else if (var->is_zeroed) {
                sb_push_cstr(&l->sb, "  store ");
                llvm_type_emit(l, var->type, true);
                sb_push_cstr(&l->sb, " zeroinitializer, ptr ");
                llvm_node_emit(l, n);
                sb_push(&l->sb, '\n');
            }

            if (var->debug.scope) {
                var->debug_local = ++l->iota_debug;
                var->debug.iota = ++l->iota_debug;
                sb_push_cstr(&l->sb, "  call void @llvm.dbg.declare(metadata ptr ");
                llvm_node_emit(l, n);
                sb_sprintf(&l->sb, ", metadata !%zu, metadata !DIExpression())", var->debug_local);
                llvm_debug_pos_emit(l, n->debug);
                sb_push(&l->sb, '\n');
            }
        }

        size_t first_row = 0;
        bool   first_row_set = false;
        bool   skip = false;
        for (LLVM_Node *n = fn->body.head; n; n = n->next) {
            if (n->debug && !first_row_set) {
                first_row = n->debug->row;
                first_row_set = true;
            }

            if (n->kind == LLVM_NODE_BLOCK) {
                skip = false;
            }

            if (!skip) {
                llvm_node_compile(l, n);
            }

            if (n->kind == LLVM_NODE_JUMP || n->kind == LLVM_NODE_BRANCH || n->kind == LLVM_NODE_RETURN) {
                skip = true;
            }
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
            LLVM_Node_Var *var = (LLVM_Node_Var *) n;
            if (!var->debug.scope) {
                continue;
            }

            const size_t debug_type = llvm_type_debug_compile(l, &var->type);
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

    for (LLVM_Node *it = l->vars.head; it; it = it->next) {
        LLVM_Node_Var *var = (LLVM_Node_Var *) it;
        if (var->is_const) {
            continue;
        }

        const size_t debug_var = ++l->iota_debug;
        sb_sprintf(
            &l->sb,
            "!%zu = !DIGlobalVariableExpression(var: !%zu, expr: !DIExpression())\n",
            it->debug->iota,
            debug_var);

        const size_t debug_type = llvm_type_debug_compile(l, &var->type);
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
        if (((LLVM_Node_Var *) it)->is_const) {
            continue;
        }

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

static_assert(COUNT_LLVM_TYPES == 13, "");
LLVM_Type_Info llvm_type_info(LLVM_Type type) {
    switch (type.kind) {
    case LLVM_TYPE_I0:
        return (LLVM_Type_Info) {.size = 0, .align = 0};

    case LLVM_TYPE_I1:
    case LLVM_TYPE_I8:
    case LLVM_TYPE_U8:
        return (LLVM_Type_Info) {.size = 1, .align = 1};

    case LLVM_TYPE_I16:
    case LLVM_TYPE_U16:
        return (LLVM_Type_Info) {.size = 2, .align = 2};

    case LLVM_TYPE_I32:
    case LLVM_TYPE_U32:
        return (LLVM_Type_Info) {.size = 4, .align = 4};

    case LLVM_TYPE_I64:
    case LLVM_TYPE_U64:
        return (LLVM_Type_Info) {.size = 8, .align = 8};

    case LLVM_TYPE_PTR:
    case LLVM_TYPE_FN:
        return (LLVM_Type_Info) {.size = 8, .align = 8};

    case LLVM_TYPE_STRUCT: {
        LLVM_Type_Struct *spec = &type.structt;
        if (!spec->is_info_ready) {
            spec->info.size = 0;
            spec->info.align = 1;

            for (size_t i = 0; i < spec->fields_count; i++) {
                LLVM_Field       field = spec->fields[i];
                LLVM_Field_Info *field_info = &spec->fields_infos[i];

                const LLVM_Type_Info type_info = llvm_type_info(field.type);
                field_info->size = type_info.size;
                field_info->align = type_info.align;
                // if (!packed) {
                spec->info.align = max(spec->info.align, field_info->align);
                // }
            }

            size_t offset = 0;
            for (size_t i = 0; i < spec->fields_count; i++) {
                LLVM_Field_Info *it = &spec->fields_infos[i];

                // if (!packed) {
                offset += (it->align - (offset % it->align)) % it->align;
                // }

                it->offset = offset;
                offset += it->size;
            }

            // if (!packed) {
            offset += (spec->info.align - (offset % spec->info.align)) % spec->info.align;
            // }

            spec->info.size = offset;
            spec->is_info_ready = true;
        }

        return type.structt.info;
    }

    default:
        unreachable();
        break;
    }
}

LLVM_Type llvm_type_basic(LLVM_Type_Kind kind) {
    return (LLVM_Type) {.kind = kind};
}

LLVM_Type llvm_type_ptr(LLVM *l, LLVM_Type type) {
    return (LLVM_Type) {
        .kind = LLVM_TYPE_PTR,
        .ptr.type = arena_clone(l->arena, &type, sizeof(type)),
    };
}

LLVM_Type llvm_type_fn(LLVM *l, LLVM_Type *args, size_t args_count, LLVM_Type returnn) {
    return (LLVM_Type) {
        .kind = LLVM_TYPE_FN,
        .fn.args = args,
        .fn.args_count = args_count,
        .fn.returnn = arena_clone(l->arena, &returnn, sizeof(returnn)),
    };
}

LLVM_Type llvm_type_struct(LLVM *l, LLVM_Field *fields, size_t fields_count, SV name) {
    LLVM_Type type = {0};
    type.kind = LLVM_TYPE_STRUCT;
    type.structt.name = name;
    type.structt.fields = fields;
    type.structt.fields_infos = arena_alloc(l->arena, fields_count * sizeof(*type.structt.fields_infos));
    type.structt.fields_count = fields_count;
    da_push(&l->structs, type);
    return type;
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

LLVM_Node_Fn *llvm_fn_new(LLVM *l, SV name, LLVM_Type type, bool is_extern) {
    LLVM_Node_Fn *fn = (LLVM_Node_Fn *) llvm_node_alloc(l, LLVM_NODE_FN, type);
    fn->node.sv = name;
    fn->node.debug = &fn->debug;
    fn->is_extern = is_extern;

    fn->args = arena_alloc(l->arena, type.fn.args_count * sizeof(*fn->args));
    fn->args_classes = arena_alloc(l->arena, type.fn.args_count * sizeof(*fn->args_classes));
    fn->args_count = type.fn.args_count;

    LLVM_Node_Fn *fn_save = l->fn;
    l->fn = fn;
    for (size_t i = 0; i < fn->args_count; i++) {
        LLVM_Node_Var *var = llvm_var_new(l, (SV) {0}, type.fn.args[i], true, false, false);
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
    assert(index < fn->node.type.fn.args_count);
    return fn->args[index];
}

LLVM_Node_Var *llvm_var_new(LLVM *l, SV name, LLVM_Type type, bool is_local, bool is_zeroed, bool is_extern) {
    LLVM_Node_Var *var = (LLVM_Node_Var *) llvm_node_alloc(l, LLVM_NODE_VAR, llvm_type_basic(LLVM_TYPE_PTR));
    var->node.debug = &var->debug;
    var->type = type;
    var->name = name;
    var->is_zeroed = is_zeroed;
    var->is_extern = is_extern;
    if (is_local && !is_extern) {
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

LLVM_Node_Var_Init *llvm_var_init_new_int(LLVM *l, LLVM_Type type, long n) {
    LLVM_Node_Var_Init *init = arena_alloc(l->arena, sizeof(LLVM_Node_Var_Init));
    init->kind = LLVM_NODE_VAR_INIT_INT;
    init->type = type;
    init->as.integer = n;
    return init;
}

LLVM_Node_Var_Init *llvm_var_init_new_node(LLVM *l, LLVM_Node *node) {
    LLVM_Node_Var_Init *init = arena_alloc(l->arena, sizeof(LLVM_Node_Var_Init));
    init->kind = LLVM_NODE_VAR_INIT_NODE;
    init->as.node = node;
    return init;
}

LLVM_Node_Var_Init *
llvm_var_init_new_struct(LLVM *l, LLVM_Type type, LLVM_Node_Var_Init **fields, size_t fields_count) {
    LLVM_Node_Var_Init *init = arena_alloc(l->arena, sizeof(LLVM_Node_Var_Init));
    init->kind = LLVM_NODE_VAR_INIT_STRUCT;
    init->type = type;
    init->as.structt.fields = fields;
    init->as.structt.fields_count = fields_count;
    return init;
}

void llvm_var_set_init(LLVM_Node_Var *var, LLVM_Node_Var_Init *init) {
    var->init = init;
}

LLVM_Node *llvm_const_new(LLVM *l, SV name, LLVM_Type type, LLVM_Node_Var_Init *value) {
    LLVM_Node_Var *var = (LLVM_Node_Var *) llvm_node_alloc(l, LLVM_NODE_VAR, llvm_type_basic(LLVM_TYPE_PTR));
    var->type = type;
    var->name = name;
    var->node.sv = name;
    var->is_const = true;
    var->init = value;
    llvm_nodes_push(&l->vars, (LLVM_Node *) var);
    return (LLVM_Node *) var;
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
        if (value->type.kind == LLVM_TYPE_STRUCT) {
            if (value->kind == LLVM_NODE_LOAD) {
                ((LLVM_Node_Load *) value)->is_dead = true;
            }
        }
        type = value->type;
    } else {
        type = llvm_type_basic(LLVM_TYPE_I0);
    }

    LLVM_Node_Return *returnn = (LLVM_Node_Return *) llvm_node_build(l, LLVM_NODE_RETURN, type);
    returnn->value = value;
    return (LLVM_Node *) returnn;
}

LLVM_Node *llvm_build_gep_field(LLVM *l, LLVM_Type final_type, LLVM_Node *base, LLVM_Type base_type, size_t index) {
    LLVM_Node_GEP *gep = (LLVM_Node_GEP *) llvm_node_build(l, LLVM_NODE_GEP, final_type);
    gep->base = base;
    gep->base_type = base_type;
    gep->is_field_index = true;
    gep->field_index = index;
    return (LLVM_Node *) gep;
}

LLVM_Node *llvm_build_gep_index(LLVM *l, LLVM_Type final_type, LLVM_Node *base, LLVM_Type base_type, LLVM_Node *index) {
    LLVM_Node_GEP *gep = (LLVM_Node_GEP *) llvm_node_build(l, LLVM_NODE_GEP, final_type);
    gep->base = base;
    gep->base_type = base_type;
    gep->slice_index = index;
    return (LLVM_Node *) gep;
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

LLVM_Node *llvm_build_cast(LLVM *l, LLVM_Node *value, LLVM_Type type) {
    if (value->kind == LLVM_NODE_ATOM) {
        value->type = type;
        ((LLVM_Node_Atom *) value)->is_casted_to_ptr = type.kind == LLVM_TYPE_PTR;
        return value;
    }

    LLVM_Node_Cast *cast = (LLVM_Node_Cast *) llvm_node_build(l, LLVM_NODE_CAST, type);
    cast->value = value;
    return (LLVM_Node *) cast;
}

LLVM_Node *llvm_build_call(LLVM *l, LLVM_Node *fn, LLVM_Node **args, size_t args_count, bool ref) {
    assert(fn->type.kind == LLVM_TYPE_FN);
    LLVM_Node_Call *call = (LLVM_Node_Call *) llvm_node_build(l, LLVM_NODE_CALL, *fn->type.fn.returnn);
    call->fn = fn;

    for (size_t i = 0; i < args_count; i++) {
        LLVM_Node *arg = args[i];
        if (arg->type.kind == LLVM_TYPE_STRUCT) {
            assert(arg->kind == LLVM_NODE_LOAD);
            ((LLVM_Node_Load *) arg)->is_dead = true;
        }
    }

    call->args = args;
    call->args_count = args_count;

    if (call->node.type.kind == LLVM_TYPE_STRUCT) {
        call->struct_return_memory = (LLVM_Node *) llvm_var_new(l, (SV) {0}, call->node.type, true, false, false);
        if (ref) {
            return call->struct_return_memory;
        }

        call->struct_return_load = (LLVM_Node_Load *) llvm_build_load(l, call->struct_return_memory, call->node.type);
        return (LLVM_Node *) call->struct_return_load;
    } else {
        assert(!ref);
    }

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

// TODO: Check whether external variables work once imports are implemented. It works on x86_64 Linux so far...
