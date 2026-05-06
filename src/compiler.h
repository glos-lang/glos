#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

typedef Dynamic_Array(const char *) Link_Flags;

typedef struct {
    Scope   globals;
    Context context;

    Cmd        *cmd;
    Link_Flags *link_flags;

    LLVMContextRef llvm_context;
    LLVMModuleRef  llvm_module;
    LLVMBuilderRef llvm_builder;

    // TODO: Temporary solution to permanent problems
    LLVMValueRef llvm_iprint_str;
    LLVMValueRef llvm_uprint_str;

    LLVMTypeRef  llvm_printf_type;
    LLVMValueRef llvm_printf_func;

    // TODO: Replace
    // LLVM_Node_Block *loop_break;
    // LLVM_Node_Block *loop_continue;
    Arena *arena;

    size_t iota_anonymous_fn;
    size_t iota_anonymous_const;
    size_t iota_anonymous_struct;

    // TODO: Temporary solutions to permanent problems
    const char *path;
} Compiler;

size_t compile_sizeof(Compiler *c, AST_Type *type);
void   compiler_build(Compiler *c, const char *output);

#endif // COMPILER_H
