#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"
#include "parser.h"

#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

typedef Dynamic_Array(const char *) Link_Flags;

typedef struct {
    // These are used both by the analyzer and the compiler
    Arena  *arena;
    Context context;

    Parser  *parser;
    Modules *modules;

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

    // TODO: Temporary solution to permanent problems
    LLVMValueRef llvm_iprint_str;
    LLVMValueRef llvm_uprint_str;
    LLVMTypeRef  llvm_printf_type;
    LLVMValueRef llvm_printf_func;

    // For panic messages
    LLVMTypeRef  llvm_fprintf_type;
    LLVMValueRef llvm_fprintf_func;

    LLVMTypeRef  llvm_fflush_type;
    LLVMValueRef llvm_fflush_func;

    LLVMTypeRef  llvm_abort_type;
    LLVMValueRef llvm_abort_func;

    // For string comparisons
    LLVMTypeRef  llvm_memcmp_type;
    LLVMValueRef llvm_memcmp_func;

#ifdef PLATFORM_X86_64_WINDOWS
    LLVMTypeRef  llvm_iob_type;
    LLVMValueRef llvm_iob_func;
#else
    LLVMValueRef llvm_stdout_value;
    LLVMValueRef llvm_stderr_value;
#endif // PLATFORM_X86_64_WINDOWS

    // Compound types like these are the same irrespective of underlying type, therefore don't generate them over and
    // over.
    LLVMTypeRef llvm_slice_type;

    size_t iota_anonymous_fn;
    size_t iota_anonymous_const;
    size_t iota_anonymous_struct;
} Compiler;

size_t compile_sizeof(Compiler *c, Type *type);
void   compiler_build(Compiler *c, Module *main_module, const char *output_path);

#endif // COMPILER_H
