#ifndef LLVM_H
#define LLVM_H

#include "basic.h"

typedef struct LLVM_Type LLVM_Type;

typedef struct LLVM_Node       LLVM_Node;
typedef struct LLVM_Node_Fn    LLVM_Node_Fn;
typedef struct LLVM_Node_Var   LLVM_Node_Var;
typedef struct LLVM_Node_Block LLVM_Node_Block;

typedef struct LLVM_Debug_Pos   LLVM_Debug_Pos;
typedef struct LLVM_Debug_File  LLVM_Debug_File;
typedef struct LLVM_Debug_Scope LLVM_Debug_Scope;

typedef struct LLVM_Node_Var_Init LLVM_Node_Var_Init;

typedef struct {
    LLVM_Node *head;
    LLVM_Node *tail;
} LLVM_Nodes;

typedef enum {
    LLVM_TYPE_I0,
    LLVM_TYPE_I1,

    LLVM_TYPE_I8,
    LLVM_TYPE_I16,
    LLVM_TYPE_I32,
    LLVM_TYPE_I64,

    LLVM_TYPE_U8,
    LLVM_TYPE_U16,
    LLVM_TYPE_U32,
    LLVM_TYPE_U64,

    LLVM_TYPE_PTR,
    LLVM_TYPE_FN,
    LLVM_TYPE_STRUCT,
    COUNT_LLVM_TYPES,
} LLVM_Type_Kind;

typedef struct {
    size_t size;
    size_t align;
} LLVM_Type_Info;

typedef struct LLVM_Field      LLVM_Field;
typedef struct LLVM_Field_Info LLVM_Field_Info;

typedef struct {
    SV     name;
    size_t row;

    LLVM_Field      *fields;
    LLVM_Field_Info *fields_infos;
    size_t           fields_count;

    bool           is_info_ready;
    LLVM_Type_Info info;
} LLVM_Type_Struct;

struct LLVM_Type {
    LLVM_Type_Kind kind;
    size_t         debug;

    union {
        // TODO: Make these proper typedef-ed structures
        struct {
            LLVM_Type *type;
        } ptr;

        struct {
            LLVM_Type *args;
            size_t     args_count;
            LLVM_Type *returnn;
        } fn;

        LLVM_Type_Struct structt;
    };
};

struct LLVM_Field {
    SV        name;
    LLVM_Type type;
};

struct LLVM_Field_Info {
    size_t size;
    size_t align;
    size_t offset;
    size_t debug;
};

typedef enum {
    LLVM_UNARY_NOP,
    LLVM_UNARY_NEG,
    LLVM_UNARY_BNOT,
    LLVM_UNARY_LNOT,
    COUNT_LLVM_UNARYS
} LLVM_Unary_Kind;

typedef enum {
    LLVM_BINARY_NOP,

    LLVM_BINARY_ADD,
    LLVM_BINARY_SUB,
    LLVM_BINARY_MUL,
    LLVM_BINARY_DIV,
    LLVM_BINARY_MOD,

    LLVM_BINARY_SHL,
    LLVM_BINARY_SHR,
    LLVM_BINARY_BOR,
    LLVM_BINARY_BAND,

    LLVM_BINARY_GT,
    LLVM_BINARY_GE,
    LLVM_BINARY_LT,
    LLVM_BINARY_LE,
    LLVM_BINARY_EQ,
    LLVM_BINARY_NE,

    COUNT_LLVM_BINARYS
} LLVM_Binary_Kind;

typedef struct {
    Arena *arena;
    SB     sb;

    LLVM_Nodes fns;
    LLVM_Nodes vars;
    Dynamic_Array(LLVM_Type) structs;

    LLVM_Node_Fn *fn;

    // TODO: Temporary solutions to permanent problems
    LLVM_Node_Fn *main_fn;

    size_t iota_local;
    size_t iota_debug;
    size_t basic_type_debugs[COUNT_LLVM_TYPES];

    LLVM_Debug_Pos  *debug_pos;
    LLVM_Debug_File *debug_file;
} LLVM;

void llvm_free(LLVM *l);
void llvm_compile(LLVM *l);

LLVM_Type_Info llvm_type_info(LLVM_Type type);
LLVM_Type      llvm_type_basic(LLVM_Type_Kind kind);
LLVM_Type      llvm_type_ptr(LLVM *l, LLVM_Type type);
LLVM_Type      llvm_type_fn(LLVM *l, LLVM_Type *args, size_t args_count, LLVM_Type returnn);

LLVM_Type llvm_type_struct(LLVM *l, LLVM_Field *fields, size_t fields_count, SV name);

LLVM_Node *llvm_atom_int(LLVM *l, LLVM_Type type, long n);
LLVM_Node *llvm_atom_zero(LLVM *l, LLVM_Type type);

LLVM_Node_Block *llvm_block_new(LLVM *l);

LLVM_Node_Fn *llvm_fn_new(LLVM *l, SV name, LLVM_Type type, bool is_extern);
void          llvm_fn_debug_set_pos(LLVM *l, LLVM_Node_Fn *fn, size_t row, size_t col);

LLVM_Node_Var *llvm_fn_arg_get(LLVM_Node_Fn *fn, size_t index);

LLVM_Node_Var *llvm_var_new(LLVM *l, SV name, LLVM_Type type, bool is_local, bool is_zeroed, bool is_extern);
void           llvm_var_debug_set_pos(LLVM *l, LLVM_Node_Var *var, size_t row, size_t col);
void           llvm_var_set_name(LLVM_Node_Var *var, SV name);

LLVM_Node_Var_Init *llvm_var_init_new_int(LLVM *l, LLVM_Type type, long n);
LLVM_Node_Var_Init *llvm_var_init_new_node(LLVM *l, LLVM_Node *node);
LLVM_Node_Var_Init *llvm_var_init_new_struct(LLVM *l, LLVM_Type type, LLVM_Node_Var_Init **fields, size_t fields_count);
void                llvm_var_set_init(LLVM_Node_Var *var, LLVM_Node_Var_Init *init);

LLVM_Node *llvm_const_new(LLVM *l, SV name, LLVM_Type type, LLVM_Node_Var_Init *value);

LLVM_Node *llvm_build_unary(LLVM *l, LLVM_Unary_Kind kind, LLVM_Type type, LLVM_Node *value);
LLVM_Node *llvm_build_binary(LLVM *l, LLVM_Binary_Kind kind, LLVM_Type type, LLVM_Node *lhs, LLVM_Node *rhs);

LLVM_Node *llvm_build_gep_field(LLVM *l, LLVM_Type final_type, LLVM_Node *base, LLVM_Type base_type, size_t index);
LLVM_Node *llvm_build_gep_index(LLVM *l, LLVM_Type final_type, LLVM_Node *base, LLVM_Type base_type, LLVM_Node *index);

LLVM_Node *llvm_build_load(LLVM *l, LLVM_Node *ptr, LLVM_Type type);
LLVM_Node *llvm_build_store(LLVM *l, LLVM_Node *ptr, LLVM_Node *value);
LLVM_Node *llvm_build_cast(LLVM *l, LLVM_Node *value, LLVM_Type type);

// The parameter `ref` is only valid when the return type is a structure
LLVM_Node *llvm_build_call(LLVM *l, LLVM_Node *fn, LLVM_Node **args, size_t args_count, bool ref);

LLVM_Node *llvm_build_block(LLVM *l, LLVM_Node_Block *block);
LLVM_Node *llvm_build_jump(LLVM *l, LLVM_Node_Block *block);
LLVM_Node *llvm_build_branch(LLVM *l, LLVM_Node *condition, LLVM_Node_Block *consequence, LLVM_Node_Block *antecedence);
LLVM_Node *llvm_build_return(LLVM *l, LLVM_Node *value);

LLVM_Node *llvm_build_print(LLVM *l, LLVM_Node *value);

void llvm_debug_set_file(LLVM *l, const char *path);
void llvm_debug_set_pos(LLVM *l, LLVM_Node *n, size_t row, size_t col);

void llvm_debug_scope_push(LLVM *l, size_t row, size_t col);
void llvm_debug_scope_pop(LLVM *l);

#endif // LLVM_H
