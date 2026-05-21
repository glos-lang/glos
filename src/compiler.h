#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"
#include "parser.h"

#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

typedef Dynamic_Array(const char *) Link_Flags;

typedef struct {
    // These are used only by the analyzer
    Type main_fn_type;

    // These are used both by the analyzer and the compiler
    Arena  *arena;
    Context context;

    Parser  *parser;
    Modules *modules;
    Node_Fn *main_fn;

    Module *main_module;
    Module *builtin_module;

    // Rest all are only used by compiler
    Cmd        *cmd;
    Link_Flags *link_flags;

    Dynamic_Array(Node *) defers;
    size_t defers_start;
    size_t loop_defers_start;

    LLVMContextRef       llvm_context;
    LLVMModuleRef        llvm_module;
    LLVMTargetDataRef    llvm_target_data;
    LLVMTargetMachineRef llvm_target_machine;

    LLVMBuilderRef llvm_builder;
    LLVMValueRef   llvm_fn;
    LLVMValueRef   llvm_fn_last_alloca;

    unsigned int llvm_attribute_sret;
    unsigned int llvm_attribute_byval;

    LLVMBasicBlockRef llvm_loop_break;
    LLVMBasicBlockRef llvm_loop_continue;

    LLVMDIBuilderRef llvm_debug_builder;
    LLVMMetadataRef  llvm_debug_compile_unit;
    LLVMMetadataRef  llvm_debug_scope;

    struct {
        const char     *key;
        LLVMMetadataRef value;
    } *llvm_debug_files;

    // For string comparisons
    LLVMTypeRef  llvm_memcmp_type;
    LLVMValueRef llvm_memcmp_func;

    // Compound types like these are the same irrespective of underlying type, therefore don't generate them over and
    // over.
    LLVMTypeRef llvm_slice_type;

    size_t iota_anonymous_fn;
    size_t iota_anonymous_const;
    size_t iota_anonymous_struct;
} Compiler;

size_t compile_sizeof(Compiler *c, Type *type);
void   compiler_build(Compiler *c, const char *output_path);

#endif // COMPILER_H
