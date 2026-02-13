#ifndef LLVM_H
#define LLVM_H

#include "basic.h"

typedef struct LLVM_Node       LLVM_Node;
typedef struct LLVM_Node_Fn    LLVM_Node_Fn;
typedef struct LLVM_Node_Var   LLVM_Node_Var;
typedef struct LLVM_Node_Block LLVM_Node_Block;

typedef struct LLVM_Debug_Pos  LLVM_Debug_Pos;
typedef struct LLVM_Debug_File LLVM_Debug_File;

typedef struct {
    LLVM_Node *head;
    LLVM_Node *tail;
} LLVM_Nodes;

typedef enum {
    LLVM_TYPE_I0,
    LLVM_TYPE_I1,
    LLVM_TYPE_I8,
    LLVM_TYPE_I64,

    LLVM_TYPE_FN,
    COUNT_LLVM_TYPES,
} LLVM_Type_Kind;

typedef struct {
    LLVM_Type_Kind kind;
} LLVM_Type;

typedef struct {
    size_t align;
    size_t size;
} LLVM_Type_Info;

typedef enum {
    LLVM_UNARY_NOP,
    LLVM_UNARY_NEG,
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

    LLVM_Node_Fn *fn;
    LLVM_Node_Fn *main_fn;

    size_t iota_local;
    size_t iota_debug;

    // TODO: Temporary solutions to permanent problems
    size_t debug_bool_type;
    size_t debug_i8_type;
    size_t debug_i32_type;
    size_t debug_i64_type;
    size_t debug_fn_type;
    size_t debug_main_fn_type;

    LLVM_Debug_Pos  *debug_pos;
    LLVM_Debug_File *debug_file;
} LLVM;

void llvm_free(LLVM *l);
void llvm_compile(LLVM *l);

LLVM_Type      llvm_type_basic(LLVM_Type_Kind kind);
LLVM_Type_Info llvm_type_info(LLVM_Type type);

LLVM_Node *llvm_atom_int(LLVM *l, LLVM_Type type, long n);

LLVM_Node_Block *llvm_block_new(LLVM *l);

LLVM_Node_Fn *llvm_fn_new(LLVM *l, SV name);
void          llvm_fn_debug_set_start_pos(LLVM *l, LLVM_Node_Fn *fn, size_t row, size_t col);
void          llvm_fn_debug_set_return_pos(LLVM *l, LLVM_Node_Fn *fn, size_t row, size_t col);

LLVM_Node_Var *llvm_var_new(LLVM *l, SV name, LLVM_Type type);
void           llvm_var_debug_set_pos(LLVM *l, LLVM_Node_Var *var, size_t row, size_t col);

LLVM_Node *llvm_build_unary(LLVM *l, LLVM_Unary_Kind kind, LLVM_Type type, LLVM_Node *value);
LLVM_Node *llvm_build_binary(LLVM *l, LLVM_Binary_Kind kind, LLVM_Type type, LLVM_Node *lhs, LLVM_Node *rhs);

LLVM_Node *llvm_build_load(LLVM *l, LLVM_Node *ptr, LLVM_Type type);
LLVM_Node *llvm_build_store(LLVM *l, LLVM_Node *ptr, LLVM_Node *value);

LLVM_Node *llvm_build_call(LLVM *l, LLVM_Node *fn);

LLVM_Node *llvm_build_block(LLVM *l, LLVM_Node_Block *block);
LLVM_Node *llvm_build_jump(LLVM *l, LLVM_Node_Block *block);
LLVM_Node *llvm_build_branch(LLVM *l, LLVM_Node *condition, LLVM_Node_Block *consequence, LLVM_Node_Block *antecedence);

LLVM_Node *llvm_build_print(LLVM *l, LLVM_Node *value);

void llvm_debug_set_file(LLVM *l, const char *path);
void llvm_debug_set_pos(LLVM *l, LLVM_Node *n, size_t row, size_t col);

#endif // LLVM_H
