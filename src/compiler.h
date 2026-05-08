#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"
#include <llvm-c/Target.h>

typedef Dynamic_Array(const char *) Link_Flags;

typedef struct {
    Scope   globals;
    Context context;

    Cmd        *cmd;
    Link_Flags *link_flags;

    LLVMContextRef    llvm_context;
    LLVMModuleRef     llvm_module;
    LLVMTargetDataRef llvm_target_data;

    LLVMBuilderRef llvm_builder;
    LLVMValueRef   llvm_fn;
    LLVMValueRef   llvm_fn_last_alloca;

    LLVMBasicBlockRef llvm_loop_break;
    LLVMBasicBlockRef llvm_loop_continue;

    LLVMDIBuilderRef llvm_debug_builder;
    LLVMMetadataRef  llvm_debug_compile_unit;
    LLVMMetadataRef  llvm_debug_file;
    LLVMMetadataRef  llvm_debug_scope;

    // TODO: Temporary solution to permanent problems
    LLVMValueRef llvm_iprint_str;
    LLVMValueRef llvm_uprint_str;

    LLVMTypeRef  llvm_printf_type;
    LLVMValueRef llvm_printf_func;

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
