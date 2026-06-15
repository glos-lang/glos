#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"
#include "parser.h"

#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

typedef struct {
    const char **data;
    size_t       count;
    size_t       capacity;

    Arena *arena;
} Link_Flags;

void link_flags_add_libpath(Link_Flags *ls, SV path);
void link_flags_add_libname(Link_Flags *ls, SV name);

typedef struct {
    // These are used only by the analyzer
    Type main_fn_type;
    DA(Type_Struct_Field) struct_fields;

    Type rtti_pointer_type; // This holds `&Type_Info`

    size_t            rtti_variants[COUNT_TYPES];
    const Type_Union *rtti_variants_union;

    HT(Type, LLVMValueRef) rtti_cache;

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

    DA(Node *) defers;
    size_t defers_start;
    size_t loop_defers_start;

    DA(LLVMValueRef) group_values;

    LLVMContextRef       llvm_context;
    LLVMModuleRef        llvm_module;
    LLVMTargetDataRef    llvm_target_data;
    LLVMTargetMachineRef llvm_target_machine;

    LLVMBuilderRef llvm_builder;
    LLVMValueRef   llvm_fn;
    LLVMValueRef   llvm_fn_last_alloca;

    unsigned int llvm_attribute_sret;
    unsigned int llvm_attribute_byval;
    unsigned int llvm_attribute_alwaysinline;

    LLVMBasicBlockRef llvm_loop_break;
    LLVMBasicBlockRef llvm_loop_continue;

    LLVMDIBuilderRef llvm_debug_builder;
    LLVMMetadataRef  llvm_debug_compile_unit;
    LLVMMetadataRef  llvm_debug_scope;

    HT(const char *, LLVMMetadataRef) llvm_debug_files;

    // Compound types like these are the same irrespective of underlying type, therefore don't generate them over and
    // over.
    LLVMTypeRef llvm_slice_type;

    size_t iota_anonymous_fn;
} Compiler;

size_t compile_sizeof(Compiler *c, Type *type);
void   compiler_build(Compiler *c, const char *output_path);

#endif // COMPILER_H
