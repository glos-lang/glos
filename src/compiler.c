#include "compiler.h"
#include "basic.h"
#include "checker.h"
#include "dwarf.h"
#include "node.h"
#include "token.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

static_assert(COUNT_TYPES == 21, "");
static void compile_type(Compiler *c, Type *type) {
    if (!type || type->llvm) {
        return;
    }

    assert(type->kind != TYPE_MODULE);
    assert(type->kind != TYPE_UNKNOWN_ENUM);

    // NOTE: Do not use `type*` functions because this function should not care whether a type is a metatype or not.
    if (type->ref || type->kind == TYPE_RAWPTR || type->kind == TYPE_FN) {
        type->llvm = LLVMPointerTypeInContext(c->llvm_context, 0);
        return;
    }

    switch (type->kind) {
    case TYPE_UNIT:
        type->llvm = LLVMVoidTypeInContext(c->llvm_context);
        break;

    case TYPE_BOOL:
        type->llvm = LLVMInt1TypeInContext(c->llvm_context);
        break;

    case TYPE_I8:
    case TYPE_U8:
    case TYPE_CHAR:
        type->llvm = LLVMInt8TypeInContext(c->llvm_context);
        break;

    case TYPE_I16:
    case TYPE_U16:
        type->llvm = LLVMInt16TypeInContext(c->llvm_context);
        break;

    case TYPE_I32:
    case TYPE_U32:
        type->llvm = LLVMInt32TypeInContext(c->llvm_context);
        break;

    case TYPE_I64:
    case TYPE_U64:
    case TYPE_INT:
        type->llvm = LLVMInt64TypeInContext(c->llvm_context);
        break;

    case TYPE_FN:
    case TYPE_RAWPTR:
        unreachable();
        break;

    case TYPE_ENUM: {
        Node_Enum *definition = type->spec.enumm.definition;
        if (!definition->llvm) {
            Type stub = {.kind = type->spec.enumm.underlying};
            compile_type(c, &stub);
            definition->llvm = stub.llvm;
        }
        type->llvm = definition->llvm;
    } break;

    case TYPE_STRUCT: {
        assert(type->spec.structt);

        Type_Struct *spec = type->spec.structt;
        if (!spec->llvm) {

            LLVMTypeRef *fields = temp_alloc(spec->fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];
                compile_type(c, &it->type);
                fields[i] = it->type.llvm;
            }

            spec->llvm = LLVMStructTypeInContext(c->llvm_context, fields, spec->fields_count, false);
            temp_reset(fields);
        }
        type->llvm = spec->llvm;
    } break;

    case TYPE_SLICE:
    case TYPE_STRING:
        if (!c->llvm_slice_type) {
            LLVMTypeRef fields[] = {
                LLVMPointerTypeInContext(c->llvm_context, 0),
                LLVMInt64TypeInContext(c->llvm_context),
            };
            c->llvm_slice_type = LLVMStructTypeInContext(c->llvm_context, fields, len(fields), false);
        }

        type->llvm = c->llvm_slice_type;
        break;

    case TYPE_GROUP: {
        Type_Group *spec = &type->spec.group;
        if (!spec->llvm) {
            LLVMTypeRef *fields = temp_alloc(spec->count * sizeof(*fields));
            for (size_t i = 0; i < spec->count; i++) {
                Type *it = &spec->data[i];
                compile_type(c, it);
                fields[i] = it->llvm;
            }

            spec->llvm = LLVMStructTypeInContext(c->llvm_context, fields, spec->count, false);
            temp_reset(fields);
        }
        type->llvm = spec->llvm;
    } break;

    default:
        unreachable();
        break;
    }
}

static LLVMValueRef compile_alloca(Compiler *c, LLVMTypeRef type) {
    LLVMBasicBlockRef llvm_current_block_save = LLVMGetInsertBlock(c->llvm_builder);
    if (c->llvm_fn_last_alloca) {
        LLVMValueRef next_inst = LLVMGetNextInstruction(c->llvm_fn_last_alloca);
        if (next_inst) {
            LLVMPositionBuilderBefore(c->llvm_builder, next_inst);
        } else {
            LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMGetFirstBasicBlock(c->llvm_fn));
        }
    } else {
        LLVMBasicBlockRef first_block = LLVMGetFirstBasicBlock(c->llvm_fn);
        LLVMValueRef      first_inst = LLVMGetFirstInstruction(first_block);
        if (first_inst) {
            LLVMPositionBuilderBefore(c->llvm_builder, first_inst);
        } else {
            LLVMPositionBuilderAtEnd(c->llvm_builder, first_block);
        }
    }

    LLVMValueRef alloca = LLVMBuildAlloca(c->llvm_builder, type, "");
    LLVMSetAlignment(alloca, LLVMABIAlignmentOfType(c->llvm_target_data, type));
    c->llvm_fn_last_alloca = alloca;
    LLVMPositionBuilderAtEnd(c->llvm_builder, llvm_current_block_save);
    return alloca;
}

#define ABI_DIRECT_TYPES_MAX 2

typedef struct {
    LLVMTypeRef type; // The type that this info is for

    LLVMTypeRef direct_types[ABI_DIRECT_TYPES_MAX];
    size_t      direct_types_count;
} ABI_Info;

static_assert(COUNT_TYPES == 21, "");
static bool type_is_compound(Type type) {
    if (type.ref) {
        return false;
    }

    switch (type.kind) {
    case TYPE_STRUCT:
    case TYPE_SLICE:
    case TYPE_STRING:
    case TYPE_GROUP:
        return true;

    default:
        return false;
    }
}

static ABI_Info get_abi_info_for_type(Compiler *c, Type *type) {
    ABI_Info info = {0};
    size_t   size = compile_sizeof(c, type);

    info.type = type->llvm;
    if (type->ref) {
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;
    }

    static_assert(COUNT_TYPES == 21, "");
    switch (type->kind) {
    case TYPE_UNIT:
        info.direct_types[info.direct_types_count++] = LLVMVoidTypeInContext(c->llvm_context);
        return info;

    case TYPE_BOOL:
        info.direct_types[info.direct_types_count++] = LLVMInt1TypeInContext(c->llvm_context);
        return info;

    case TYPE_RAWPTR:
    case TYPE_FN:
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;

    default:
        // Pass
        break;
    }

    if (size <= 8) {
#ifdef PLATFORM_ARM64_MACOS
        if (type_is_compound(*type)) {
            size = 8;
        }
#endif // PLATFORM_ARM64_MACOS

        info.direct_types[info.direct_types_count++] = LLVMIntTypeInContext(c->llvm_context, size * 8);
        return info;
    }

#ifdef PLATFORM_X86_64_LINUX
    if (size <= 16) {
        // TODO(@abi): The sizes of the integers depend on the elements present in that portion
        info.direct_types[info.direct_types_count++] = LLVMIntTypeInContext(c->llvm_context, 64);
        info.direct_types[info.direct_types_count++] = LLVMIntTypeInContext(c->llvm_context, size * 8 - 64);
        return info;
    }
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    if (size <= 16) {
        info.direct_types[info.direct_types_count++] = LLVMArrayType(LLVMInt64TypeInContext(c->llvm_context), 2);
        return info;
    }
#endif // PLATFORM_ARM64_MACOS

    return info;
}

typedef struct {
    ABI_Info *args;
    size_t    args_count;

    ABI_Info return_abi;
    Type    *return_type;

    LLVMTypeRef *actual_args;
    size_t       actual_args_count;

    bool   is_variadic;
    size_t actual_args_variadics_start;
} ABI;

static void abi_set_return_type(Compiler *c, ABI *abi, Type *type) {
    assert(abi->actual_args_count == 0);
    abi->return_abi = get_abi_info_for_type(c, type);
    abi->return_type = type;
    if (!abi->return_abi.direct_types_count) {
        abi->actual_args_count++;
    }
}

static void abi_set_argument_type(Compiler *c, ABI *abi, size_t index, Type *type) {
    assert(index < abi->args_count);
    ABI_Info *it = &abi->args[index];
    *it = get_abi_info_for_type(c, type);
    if (it->direct_types_count) {
        abi->actual_args_count += it->direct_types_count;
    } else {
        abi->actual_args_count++;
    }
}

static void abi_set_variadic_at(ABI *abi, size_t index) {
    assert(!abi->is_variadic);
    abi->is_variadic = true;
    abi->actual_args_variadics_start = index;
}

static LLVMTypeRef abi_finalize(Compiler *c, ABI *abi) {
    size_t args_iota = 0;
    if (!abi->return_abi.direct_types_count) {
        abi->actual_args[args_iota++] = LLVMPointerTypeInContext(c->llvm_context, 0);
    }

    for (size_t i = 0; i < abi->args_count; i++) {
        const ABI_Info it_abi = abi->args[i];
        if (it_abi.direct_types_count) {
            for (size_t j = 0; j < it_abi.direct_types_count; j++) {
                abi->actual_args[args_iota++] = it_abi.direct_types[j];
            }
        } else {
            abi->actual_args[args_iota++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        }
    }

    LLVMTypeRef return_type = NULL;

    static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
    switch (abi->return_abi.direct_types_count) {
    case 0:
        return_type = LLVMVoidTypeInContext(c->llvm_context);
        break;

    case 1:
        return_type = abi->return_abi.direct_types[0];
        break;

    case 2:
        return_type = LLVMStructTypeInContext(
            c->llvm_context, abi->return_abi.direct_types, abi->return_abi.direct_types_count, false);
        break;

    default:
        unreachable();
    }

    return LLVMFunctionType(
        return_type,
        abi->actual_args,
        abi->is_variadic ? abi->actual_args_variadics_start : abi->actual_args_count,
        abi->is_variadic);
}

static LLVMValueRef undo_load(LLVMValueRef value) {
    assert(LLVMGetInstructionOpcode(value) == LLVMLoad);
    assert(LLVMGetFirstUse(value) == NULL);
    LLVMValueRef ptr = LLVMGetOperand(value, 0);
    LLVMInstructionEraseFromParent(value);
    return ptr;
}

typedef struct {
    ABI abi;

    LLVMTypeRef  fn_type;
    LLVMValueRef fn_value;

    LLVMValueRef *args;
    size_t        args_iota;
    size_t        args_abi_iota;
} ABI_Call;

// Call after `abi` has been finalized
// NOTE: This allocates memory using the temporary allocator which is then freed by `abi_finalize()`
static ABI_Call abi_call_create(Compiler *c, ABI abi, LLVMValueRef fn_value, LLVMTypeRef fn_type) {
    ABI_Call call = {0};
    call.abi = abi;
    call.fn_type = fn_type;
    call.fn_value = fn_value;

    call.args = temp_alloc(abi.actual_args_count * sizeof(*call.args));
    if (!abi.return_abi.direct_types_count) {
        call.args[call.args_iota++] = compile_alloca(c, abi.return_type->llvm);
    }

    return call;
}

static void abi_call_add_arg(Compiler *c, ABI_Call *call, LLVMValueRef expr, Type type) {
    const ABI_Info arg_abi = call->abi.args[call->args_abi_iota++];

    static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
    switch (arg_abi.direct_types_count) {
    case 0: {
#ifdef PLATFORM_X86_64_LINUX
        expr = undo_load(expr);
#else
        LLVMValueRef temp = compile_alloca(c, type.llvm);
        LLVMBuildStore(c->llvm_builder, expr, temp);
        expr = temp;
#endif // PLATFORM_X86_64_LINUX

        call->args[call->args_iota++] = expr;
    } break;

    case 1: {
        if (type_is_compound(type)) {
            expr = undo_load(expr);
            expr = LLVMBuildLoad2(c->llvm_builder, arg_abi.direct_types[0], expr, "");
        } else {
            // TODO(@variadics): Promotion
        }
        call->args[call->args_iota++] = expr;
    } break;

    case 2: {
        // First half
        LLVMValueRef first = undo_load(expr);
        call->args[call->args_iota++] = LLVMBuildLoad2(c->llvm_builder, arg_abi.direct_types[0], first, "");

        // Second half
        LLVMTypeRef  llvm_i8_type = LLVMInt8TypeInContext(c->llvm_context);
        LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 8, false)};
        LLVMValueRef second = LLVMBuildGEP2(c->llvm_builder, llvm_i8_type, first, indices, len(indices), "");
        call->args[call->args_iota++] = LLVMBuildLoad2(c->llvm_builder, arg_abi.direct_types[1], second, "");
    } break;

    default:
        unreachable();
    }
}

static LLVMValueRef abi_call_finalize(Compiler *c, ABI_Call *call, bool ref) {
    assert(call->args_iota == call->abi.actual_args_count);

    LLVMValueRef result =
        LLVMBuildCall2(c->llvm_builder, call->fn_type, call->fn_value, call->args, call->abi.actual_args_count, "");
    LLVMValueRef memory = NULL;

    size_t args_iota = 0;
    if (type_is_compound(*call->abi.return_type)) {
        static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
        switch (call->abi.return_abi.direct_types_count) {
        case 0: {
            memory = call->args[args_iota++];
            LLVMAttributeRef sret =
                LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_sret, call->abi.return_type->llvm);
            LLVMAddCallSiteAttribute(result, args_iota, sret);
        } break;

        case 1:
            memory = compile_alloca(c, call->abi.return_type->llvm);
            LLVMBuildStore(c->llvm_builder, result, memory);
            break;

        case 2: {
            memory = compile_alloca(c, call->abi.return_type->llvm);

            // First half
            LLVMValueRef first = memory;
            LLVMBuildStore(c->llvm_builder, LLVMBuildExtractValue(c->llvm_builder, result, 0, ""), first);

            // Second half
            LLVMTypeRef  llvm_i8_type = LLVMInt8TypeInContext(c->llvm_context);
            LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 8, false)};
            LLVMValueRef second = LLVMBuildGEP2(c->llvm_builder, llvm_i8_type, memory, indices, len(indices), "");
            LLVMBuildStore(c->llvm_builder, LLVMBuildExtractValue(c->llvm_builder, result, 1, ""), second);
        } break;

        default:
            unreachable();
        }
    }

#ifdef PLATFORM_X86_64_LINUX
    for (size_t i = 0; i < call->abi.args_count; i++) {
        const ABI_Info it_abi = call->abi.args[i];
        if (it_abi.direct_types_count) {
            args_iota += it_abi.direct_types_count;
        } else {
            args_iota++;

            LLVMTypeRef it_type = it_abi.type;
            assert(it_type);

            LLVMAttributeRef byval = LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_byval, it_type);
            LLVMAddCallSiteAttribute(result, args_iota, byval);
        }
    }
    assert(args_iota == call->abi.actual_args_count);
#endif // PLATFORM_X86_64_LINUX

    temp_reset(call->args);
    if (memory) {
        if (ref) {
            return memory;
        }

        return LLVMBuildLoad2(c->llvm_builder, call->abi.return_type->llvm, memory, "");
    }
    return result;
}

static LLVMTypeRef compile_fn_type(Compiler *c, Type type, ABI *abi) {
    const void *checkpoint = temp_alloc(0);

    assert(!type.ref && type.kind == TYPE_FN);
    Type_Fn *spec = &type.spec.fn;

    abi_set_return_type(c, abi, spec->return_type);
    for (size_t i = 0; i < spec->args_count; i++) {
        abi_set_argument_type(c, abi, i, &spec->args[i].type);
    }

    if (spec->is_variadic) {
        abi_set_variadic_at(abi, spec->args_count);
    }

    abi->actual_args = temp_alloc(abi->actual_args_count * sizeof(*abi->actual_args));
    LLVMTypeRef fn_type = abi_finalize(c, abi);
    temp_reset(checkpoint);
    return fn_type;
}

static LLVMValueRef compile_fn(Compiler *c, Node_Fn *fn);
static LLVMValueRef compile_expr(Compiler *c, Node *n, bool ref);
static void         compile_stmt(Compiler *c, Node *n);

static const char *temp_emit_nested_fn_name(Compiler *c, Node_Fn *fn, Module *module) {
    if (!fn) {
        return temp_sprintf("%s", module->name);
    }

    const char *name = temp_emit_nested_fn_name(c, fn->outer_fn, module);
    temp_remove_null();

    if (fn->defined_as) {
        temp_sprintf("." SV_Fmt, SV_Arg(fn->defined_as->node.token.sv));
    } else {
        if (!fn->defined_as_anon_iota) {
            fn->defined_as_anon_iota = ++c->iota_anonymous_fn;
        }
        temp_sprintf(".anon.%zu", fn->defined_as_anon_iota);
    }

    return name;
}

static LLVMMetadataRef get_debug_file(Compiler *c, const char *path) {
    if (!c->llvm_debug_files.hasheq) {
        c->llvm_debug_files.hasheq = ht_hasheq_cstr;
    }

    LLVMMetadataRef *metadatap = ht_get(&c->llvm_debug_files, path);
    if (metadatap) {
        return *metadatap;
    }

    LLVMMetadataRef metadata = LLVMDIBuilderCreateFile(c->llvm_debug_builder, path, strlen(path), ".", strlen("."));
    ht_set(&c->llvm_debug_files, path, metadata);
    return metadata;
}

static LLVMMetadataRef get_scope_of_definition(Compiler *c, Node *node, Node_Fn *defined_in) {
    if (!defined_in) {
        return get_debug_file(c, node->token.pos.path);
    }

    assert(defined_in->llvm_debug_scope);
    return defined_in->llvm_debug_scope;
}

static LLVMMetadataRef get_debug_for_type(Compiler *c, Type *type);

// Assertion: sizeof(type) == 8
typedef struct {
    SV   name;
    Type type;
} Builtin_Compound_Type_Field;

static LLVMMetadataRef
get_debug_for_builtin_compound_type(Compiler *c, SV name, Builtin_Compound_Type_Field *fields, size_t fields_count) {
    const void *checkpoint = temp_alloc(0);

    LLVMMetadataRef  empty_file_path_metadata = get_debug_file(c, "");
    LLVMMetadataRef *members = temp_alloc(fields_count * sizeof(*members));

    size_t size_bits = 0;
    for (size_t i = 0; i < fields_count; i++) {
        Builtin_Compound_Type_Field it = fields[i];
        assert(compile_sizeof(c, &it.type) == 8);

        members[i] = LLVMDIBuilderCreateMemberType(
            c->llvm_debug_builder,
            c->llvm_debug_compile_unit,
            it.name.data,
            it.name.count,
            empty_file_path_metadata,
            0,
            64,
            64,
            size_bits,
            0,
            get_debug_for_type(c, &it.type));

        size_bits += 64;
    }

    LLVMMetadataRef real_metadata = LLVMDIBuilderCreateStructType(
        c->llvm_debug_builder,
        c->llvm_debug_compile_unit,
        "",
        0,
        empty_file_path_metadata,
        0,
        size_bits,
        64,
        0,
        NULL,
        members,
        fields_count,
        0,
        NULL,
        "",
        0);

    LLVMMetadataRef typedef_metadata = LLVMDIBuilderCreateTypedef(
        c->llvm_debug_builder,
        real_metadata,
        name.data,
        name.count,
        empty_file_path_metadata,
        0,
        c->llvm_debug_compile_unit,
        64);

    temp_reset(checkpoint);
    return typedef_metadata;
}

static_assert(COUNT_TYPES == 21, "");
static LLVMMetadataRef get_debug_for_type(Compiler *c, Type *type) {
    assert(!type->is_meta);
    if (type->ref) {
        Type inner = *type;
        inner.ref--;
        inner.llvm = NULL;
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, get_debug_for_type(c, &inner), sizeof(void *), sizeof(void *), 0, "", 0);
    }

    switch (type->kind) {
    case TYPE_UNIT:
        return NULL;

    case TYPE_BOOL:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "bool", strlen("bool"), 8, DW_ATE_boolean, 0);

    case TYPE_CHAR:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "char", strlen("char"), 8, DW_ATE_unsigned_char, 0);

    case TYPE_I8:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i8", strlen("i8"), 8, DW_ATE_signed, 0);

    case TYPE_I16:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i16", strlen("i16"), 16, DW_ATE_signed, 0);

    case TYPE_I32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i32", strlen("i32"), 32, DW_ATE_signed, 0);

    case TYPE_I64:
    case TYPE_INT:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i64", strlen("i64"), 64, DW_ATE_signed, 0);

    case TYPE_U8:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u8", strlen("u8"), 8, DW_ATE_unsigned, 0);

    case TYPE_U16:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u16", strlen("u16"), 16, DW_ATE_unsigned, 0);

    case TYPE_U32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u32", strlen("u32"), 32, DW_ATE_unsigned, 0);

    case TYPE_U64:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u64", strlen("u64"), 64, DW_ATE_unsigned, 0);

    case TYPE_RAWPTR:
        return LLVMDIBuilderCreatePointerType(c->llvm_debug_builder, NULL, sizeof(void *), sizeof(void *), 0, "", 0);

    case TYPE_FN: {
        const Type_Fn spec = type->spec.fn;

        LLVMMetadataRef *args = temp_alloc((spec.args_count + 1) * sizeof(*args));
        args[0] = get_debug_for_type(c, spec.return_type);
        for (size_t i = 0; i < spec.args_count; i++) {
            args[i + 1] = get_debug_for_type(c, &spec.args[i].type);
        }

        LLVMMetadataRef fn_debug_type =
            LLVMDIBuilderCreateSubroutineType(c->llvm_debug_builder, NULL, args, spec.args_count + 1, 0);

        temp_reset(args);
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, fn_debug_type, sizeof(void *), sizeof(void *), 0, "", 0);
    }

    case TYPE_ENUM: {
        Node_Enum *definition = type->spec.enumm.definition;
        if (!definition->debug) {
            const void *checkpoint = temp_alloc(0);

            const size_t size = compile_sizeof(c, type);
            const SV     name = sv_from_cstr(type_to_cstr(*type));
            definition->debug = LLVMDIBuilderCreateBasicType(
                c->llvm_debug_builder,
                name.data,
                name.count,
                size * 8,
                type_is_signed(*type) ? DW_ATE_signed : DW_ATE_unsigned,
                0);

            temp_reset(checkpoint);
        }
        return definition->debug;
    }

    case TYPE_STRUCT: {
        compile_type(c, type);

        Type_Struct *spec = type->spec.structt;
        if (!spec->debug) {
            const void *checkpoint = temp_alloc(0);

            SV name = {0};
            {
                const char *namespace =
                    temp_emit_nested_fn_name(c, spec->definition->defined_in, spec->definition->module);
                temp_remove_null();

                Node_Atom *defined_as = spec->definition->defined_as;
                if (defined_as) {
                    temp_sprintf("." SV_Fmt, SV_Arg(defined_as->node.token.sv));
                } else {
                    if (!spec->definition->defined_as_anon_iota) {
                        spec->definition->defined_as_anon_iota = ++c->iota_anonymous_struct;
                    }
                    temp_sprintf(".anon.%zu", spec->definition->defined_as_anon_iota);
                }

                name = sv_from_cstr(namespace);
            }

            spec->debug = LLVMDIBuilderCreateReplaceableCompositeType(
                c->llvm_debug_builder,
                DW_TAG_structure_type,
                name.data,
                name.count,
                get_scope_of_definition(c, (Node *) spec->definition, spec->definition->defined_in),
                get_debug_file(c, spec->definition->node.token.pos.path),
                spec->definition->node.token.pos.row + 1,
                0,
                0,
                0,
                0,
                NULL,
                0);

            LLVMMetadataRef *fields = temp_alloc(spec->fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];

                const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t offset_bits = LLVMOffsetOfElement(c->llvm_target_data, spec->llvm, i) * 8;

                fields[i] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    spec->debug,
                    it->name.data,
                    it->name.count,
                    get_debug_file(c, it->pos.path),
                    it->pos.row + 1,
                    size_bits,
                    align_bits,
                    offset_bits,
                    0,
                    get_debug_for_type(c, &it->type));
            }

            LLVMMetadataRef scope_metadata =
                get_scope_of_definition(c, (Node *) spec->definition, spec->definition->defined_in);
            LLVMMetadataRef file_metadata = get_debug_file(c, spec->definition->node.token.pos.path);
            LLVMMetadataRef real = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                scope_metadata,
                name.data,
                name.count,
                file_metadata,
                spec->definition->node.token.pos.row + 1,
                LLVMABISizeOfType(c->llvm_target_data, spec->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8,
                0,
                NULL,
                fields,
                spec->fields_count,
                0,
                NULL,
                "",
                0);

            if (spec->definition->defined_as) {
                real = LLVMDIBuilderCreateTypedef(
                    c->llvm_debug_builder,
                    real,
                    name.data,
                    name.count,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    scope_metadata,
                    LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8);
            }

            LLVMMetadataReplaceAllUsesWith(spec->debug, real);
            spec->debug = real;
            temp_reset(checkpoint);
        }

        return spec->debug;
    }

    case TYPE_SLICE: {
        const void *checkpoint = temp_alloc(0);

        SV name = sv_from_cstr(type_to_cstr_raw(*type));

        Builtin_Compound_Type_Field fields[2] = {0};
        fields[0].name = sv_from_cstr("data");
        fields[0].type = *type->spec.slice.element;
        fields[0].type.ref++;
        fields[0].type.llvm = NULL;

        fields[1].name = sv_from_cstr("count");
        fields[1].type = (Type) {.kind = TYPE_I64};

        LLVMMetadataRef metadata = get_debug_for_builtin_compound_type(c, name, fields, len(fields));
        temp_reset(checkpoint);
        return metadata;
    }

    case TYPE_STRING: {
        Builtin_Compound_Type_Field fields[2] = {0};
        fields[0].name = sv_from_cstr("data");
        fields[0].type = (Type) {.kind = TYPE_CHAR, .ref = 1};

        fields[1].name = sv_from_cstr("count");
        fields[1].type = (Type) {.kind = TYPE_I64};
        return get_debug_for_builtin_compound_type(c, sv_from_cstr("string"), fields, len(fields));
    }

    case TYPE_GROUP: {
        compile_type(c, type);

        Type_Group *spec = &type->spec.group;
        if (!spec->debug) {
            const void *checkpoint = temp_alloc(0);
            const SV    name = sv_from_cstr(type_to_cstr_raw(*type));

            LLVMMetadataRef empty_file_path_metadata = get_debug_file(c, "");

            LLVMMetadataRef *fields = temp_alloc(spec->count * sizeof(*fields));
            for (size_t i = 0; i < spec->count; i++) {
                Type *it = &spec->data[i];

                const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->llvm) * 8;
                const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->llvm) * 8;
                const size_t offset_bits = LLVMOffsetOfElement(c->llvm_target_data, spec->llvm, i) * 8;
                const SV     name = sv_from_cstr(temp_sprintf("%zu", i));

                fields[i] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    c->llvm_debug_compile_unit,
                    name.data,
                    name.count,
                    empty_file_path_metadata,
                    0,
                    size_bits,
                    align_bits,
                    offset_bits,
                    0,
                    get_debug_for_type(c, it));
            }

            spec->debug = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                c->llvm_debug_compile_unit,
                "",
                0,
                empty_file_path_metadata,
                0,
                LLVMABISizeOfType(c->llvm_target_data, spec->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8,
                0,
                NULL,
                fields,
                spec->count,
                0,
                NULL,
                "",
                0);

            spec->debug = LLVMDIBuilderCreateTypedef(
                c->llvm_debug_builder,
                spec->debug,
                name.data,
                name.count,
                empty_file_path_metadata,
                0,
                c->llvm_debug_compile_unit,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8);

            temp_reset(checkpoint);
        }

        return spec->debug;
    }

    case TYPE_MODULE:
        unreachable();

    case TYPE_UNKNOWN_ENUM:
        unreachable();

    default:
        unreachable();
        break;
    }
}

static void set_debug_pos(Compiler *c, Pos pos) {
    LLVMSetCurrentDebugLocation2(
        c->llvm_builder,
        LLVMDIBuilderCreateDebugLocation(c->llvm_context, pos.row + 1, pos.col + 1, c->llvm_debug_scope, NULL));
}

static LLVMValueRef compile_string_struct(Compiler *c, LLVMValueRef data, size_t count, const Pos *pos, bool ref) {
    LLVMValueRef slice_struct = compile_alloca(c, c->llvm_slice_type);
    LLVMBuildStore(c->llvm_builder, data, slice_struct);
    LLVMBuildStore(
        c->llvm_builder,
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), count, false),
        LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, slice_struct, 1, ""));

    if (ref) {
        return slice_struct;
    }

    if (pos) {
        set_debug_pos(c, *pos);
    }
    return LLVMBuildLoad2(c->llvm_builder, c->llvm_slice_type, slice_struct, "");
}

static LLVMValueRef compile_string(Compiler *c, SV sv, const Pos *pos, bool ref) {
    LLVMValueRef memory = LLVMConstStringInContext(c->llvm_context, sv.data, sv.count, false);
    LLVMValueRef data = LLVMAddGlobal(c->llvm_module, LLVMTypeOf(memory), "");
    LLVMSetInitializer(data, memory);
    return compile_string_struct(c, data, sv.count, pos, ref);
}

static_assert(COUNT_CONST_VALUES == 6, "");
static LLVMValueRef compile_const_value(Compiler *c, Const_Value value, Type type) {
    switch (value.kind) {
    case CONST_VALUE_INT:
        return LLVMConstInt(type.llvm, value.as.integer, type_is_signed(type));

    case CONST_VALUE_FN:
        return compile_fn(c, value.as.fn);

    case CONST_VALUE_TYPE:
        unreachable();

    case CONST_VALUE_STRUCT: {
        const Type_Struct *spec = value.as.structt.spec;

        LLVMValueRef *fields = temp_alloc(spec->fields_count * sizeof(*fields));
        for (size_t i = 0; i < spec->fields_count; i++) {
            fields[i] = compile_const_value(c, value.as.structt.fields[i], spec->fields[i].type);
        }

        LLVMValueRef result = LLVMConstStructInContext(c->llvm_context, fields, spec->fields_count, false);
        temp_reset(fields);
        return result;
    }

    case CONST_VALUE_STRING:
        return compile_string(c, value.as.string, NULL, false);

    case CONST_VALUE_MODULE:
        unreachable();

    default:
        unreachable();
    }
}

static void compile_local_var_debug(Compiler *c, Node_Atom *it, LLVMMetadataRef var_debug_type) {
    const SV        name = it->node.token.sv;
    LLVMMetadataRef var_debug_metadata = NULL;
    if (it->definition_spec->arg_index) {
        var_debug_metadata = LLVMDIBuilderCreateParameterVariable(
            c->llvm_debug_builder,
            c->llvm_debug_scope,
            name.data,
            name.count,
            it->definition_spec->arg_index,
            get_debug_file(c, it->node.token.pos.path),
            it->node.token.pos.row + 1,
            var_debug_type,
            false,
            0);
    } else {
        var_debug_metadata = LLVMDIBuilderCreateAutoVariable(
            c->llvm_debug_builder,
            c->llvm_debug_scope,
            name.data,
            name.count,
            get_debug_file(c, it->node.token.pos.path),
            it->node.token.pos.row + 1,
            var_debug_type,
            false,
            0,
            LLVMABIAlignmentOfType(c->llvm_target_data, it->node.type.llvm));
    }

    LLVMMetadataRef var_pos_metadata = LLVMDIBuilderCreateDebugLocation(
        c->llvm_context, it->node.token.pos.row + 1, it->node.token.pos.col + 1, c->llvm_debug_scope, NULL);

    LLVMValueRef next_inst = c->llvm_fn_last_alloca ? LLVMGetNextInstruction(c->llvm_fn_last_alloca) : NULL;
    if (next_inst) {
        LLVMDIBuilderInsertDeclareRecordBefore(
            c->llvm_debug_builder,
            it->definition_spec->llvm,
            var_debug_metadata,
            LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
            var_pos_metadata,
            next_inst);
    } else {
        LLVMDIBuilderInsertDeclareRecordAtEnd(
            c->llvm_debug_builder,
            it->definition_spec->llvm,
            var_debug_metadata,
            LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
            var_pos_metadata,
            LLVMGetInsertBlock(c->llvm_builder));
    }
}

static void compile_var_def(Compiler *c, Node_Atom *it) {
    const void *checkpoint = temp_alloc(0);

    compile_type(c, &it->node.type);

    SV link_as = {0};
    if (it->definition_spec->link_as.count) {
        // Guarantee a terminating '\0'
        link_as = sv_from_cstr(temp_sv_to_cstr(it->definition_spec->link_as));
    }

    SV name = it->node.token.sv;
    if (it->definition_spec->is_extern) {
        // Guarantee a terminating '\0'
        name = sv_from_cstr(temp_sv_to_cstr(name));
    } else if (!it->definition_spec->is_local) {
        name = sv_from_cstr(temp_sprintf("%s." SV_Fmt, it->module->name, SV_Arg(name)));
    }

    if (it->definition_spec->is_local && !it->definition_spec->is_extern) {
        it->definition_spec->llvm = compile_alloca(c, it->node.type.llvm);
    } else {
        if (!link_as.count) {
            link_as = name;
        }
        it->definition_spec->llvm = LLVMAddGlobal(c->llvm_module, it->node.type.llvm, link_as.data);
    }

    if (!it->definition_spec->is_extern) {
        LLVMMetadataRef var_debug_type = get_debug_for_type(c, &it->node.type);
        if (it->definition_spec->is_local) {
            if (!it->definition_spec->arg_index && !it->definition_spec->is_assigned) {
                LLVMBuildStore(c->llvm_builder, LLVMConstNull(it->node.type.llvm), it->definition_spec->llvm);
            }
            compile_local_var_debug(c, it, var_debug_type);
        } else {
            if (it->definition_spec->is_assigned) {
                LLVMSetInitializer(
                    it->definition_spec->llvm, compile_const_value(c, it->definition_spec->const_value, it->node.type));
            } else {
                LLVMSetInitializer(it->definition_spec->llvm, LLVMConstNull(it->node.type.llvm));
            }

            LLVMMetadataRef var_debug_metadata = LLVMDIBuilderCreateGlobalVariableExpression(
                c->llvm_debug_builder,
                get_debug_file(c, it->node.token.pos.path),
                name.data,
                name.count,
                link_as.data,
                link_as.count,
                get_debug_file(c, it->node.token.pos.path),
                it->node.token.pos.row + 1,
                var_debug_type,
                true,
                LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
                NULL,
                0);

            LLVMGlobalSetMetadata(it->definition_spec->llvm, 0, var_debug_metadata);
        }
    }

    temp_reset(checkpoint);
}

static void compile_defers(Compiler *c, size_t from, bool rollback) {
    for (size_t i = c->defers.count; i > from; i--) {
        compile_stmt(c, c->defers.data[i - 1]);
    }

    if (rollback) {
        c->defers.count = from;
    }
}

static LLVMValueRef compile_fn(Compiler *c, Node_Fn *fn) {
    if (fn->llvm) {
        return fn->llvm;
    }

    const void *checkpoint = temp_alloc(0);

    ABI abi = {0};
    abi.args = temp_alloc(fn->args_count * sizeof(*abi.args));
    abi.args_count = fn->args_count;
    fn->node.type.llvm = compile_fn_type(c, fn->node.type, &abi);

    SV link_as = {0};
    if (fn->defined_as && fn->defined_as->definition_spec->link_as.count) {
        // Guarantee a terminating '\0'
        link_as = sv_from_cstr(temp_sv_to_cstr(fn->defined_as->definition_spec->link_as));
    }

    if (fn->is_extern) {
        assert(fn->defined_as);
        if (!link_as.count) {
            link_as = fn->defined_as->node.token.sv;
        }
        fn->llvm = LLVMGetOrInsertFunction(c->llvm_module, link_as.data, link_as.count, fn->node.type.llvm);
    } else {
        const size_t defers_start_save = c->defers_start;
        c->defers_start = c->defers.count;

        LLVMValueRef llvm_fn_save = c->llvm_fn;
        LLVMValueRef llvm_fn_last_alloca_save = c->llvm_fn_last_alloca;

        LLVMMetadataRef   llvm_debug_scope_save = c->llvm_debug_scope;
        LLVMBasicBlockRef llvm_current_block_save = LLVMGetInsertBlock(c->llvm_builder);

        SV fn_name = sv_from_cstr(temp_emit_nested_fn_name(c, fn, fn->module));
        if (!link_as.count) {
            link_as = fn_name;
        }
        fn->llvm = LLVMAddFunction(c->llvm_module, link_as.data, fn->node.type.llvm);

        if (fn->is_inline) {
            LLVMAttributeRef alwaysinline = LLVMCreateEnumAttribute(c->llvm_context, c->llvm_attribute_alwaysinline, 0);
            LLVMAddAttributeAtIndex(fn->llvm, LLVMAttributeFunctionIndex, alwaysinline);
        }

        c->llvm_fn = fn->llvm;
        c->llvm_fn_last_alloca = NULL;

        LLVMMetadataRef fn_debug_type = NULL;
        const Type_Fn   fn_type_spec = fn->node.type.spec.fn;

        {

            LLVMMetadataRef *arg_debug_types = temp_alloc((fn_type_spec.args_count + 1) * sizeof(*arg_debug_types));
            arg_debug_types[0] = get_debug_for_type(c, fn_type_spec.return_type);
            for (size_t i = 0; i < fn_type_spec.args_count; i++) {
                arg_debug_types[i + 1] = get_debug_for_type(c, &fn_type_spec.args[i].type);
            }

            fn_debug_type = LLVMDIBuilderCreateSubroutineType(
                c->llvm_debug_builder,
                get_debug_file(c, fn->node.token.pos.path),
                arg_debug_types,
                fn_type_spec.args_count + 1,
                0);

            temp_reset(arg_debug_types);
        }

        fn->llvm_debug_scope = LLVMDIBuilderCreateFunction(
            c->llvm_debug_builder,
            get_scope_of_definition(c, (Node *) fn, fn->outer_fn),
            fn_name.data,
            fn_name.count,
            link_as.data,
            link_as.count,
            get_debug_file(c, fn->node.token.pos.path),
            fn->node.token.pos.row + 1,
            fn_debug_type,
            true,
            true,
            fn->node.token.pos.row + 1,
            0,
            false);

        LLVMSetSubprogram(fn->llvm, fn->llvm_debug_scope);
        c->llvm_debug_scope = fn->llvm_debug_scope;

        LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, fn->llvm, ""));
        LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);

        size_t abi_iota = 0;
        size_t arg_iota = 0;
        if (!abi.return_abi.direct_types_count) {
            arg_iota++;
            LLVMAttributeRef sret =
                LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_sret, abi.return_type->llvm);
            LLVMAddAttributeAtIndex(fn->llvm, arg_iota, sret);
        }

        for (Node *arg = fn->args.head; arg; arg = arg->next) {
            assert(arg->kind == NODE_DEFINE);
            Node_Define *define = (Node_Define *) arg;

            assert(define->name->kind == NODE_ATOM);
            Node_Atom *it = (Node_Atom *) define->name;
            assert(!it->definition_spec->llvm);

            const ABI_Info it_abi = abi.args[abi_iota++];

            static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
            switch (it_abi.direct_types_count) {
            case 0: {
                it->definition_spec->llvm = LLVMGetParam(c->llvm_fn, arg_iota++);
                compile_local_var_debug(c, it, get_debug_for_type(c, &it->node.type));

#ifdef PLATFORM_X86_64_LINUX
                LLVMAttributeRef byval =
                    LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_byval, it->node.type.llvm);
                LLVMAddAttributeAtIndex(fn->llvm, arg_iota, byval);
#endif // PLATFORM_X86_64_LINUX
            } break;

            case 1:
                compile_var_def(c, it);
                LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), it->definition_spec->llvm);
                break;

            case 2: {
                compile_var_def(c, it);

                // First half
                LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), it->definition_spec->llvm);

                // Second half
                LLVMTypeRef  llvm_i8_type = LLVMInt8TypeInContext(c->llvm_context);
                LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 8, false)};
                LLVMValueRef second =
                    LLVMBuildGEP2(c->llvm_builder, llvm_i8_type, it->definition_spec->llvm, indices, len(indices), "");
                LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), second);
            } break;

            default:
                unreachable();
            }
        }
        assert(arg_iota == abi.actual_args_count);

        assert(fn->body->kind == NODE_BLOCK);
        Node_Block *block = (Node_Block *) fn->body;
        for (Node *it = block->body.head; it; it = it->next) {
            compile_stmt(c, it);
        }

        if (!fn_type_spec.returns_count) {
            compile_defers(c, c->defers_start, true);
            set_debug_pos(c, block->end);
            LLVMBuildRetVoid(c->llvm_builder);
        } else {
            // The semantic analyzer has already determined that the function returns in all execution paths.
            // No need to compile defers here, as this is unreachable.
            set_debug_pos(c, block->end);
            LLVMBuildUnreachable(c->llvm_builder);
        }

        c->defers.count = c->defers_start;
        c->defers_start = defers_start_save;

        c->llvm_fn = llvm_fn_save;
        c->llvm_fn_last_alloca = llvm_fn_last_alloca_save;

        c->llvm_debug_scope = llvm_debug_scope_save;
        LLVMPositionBuilderAtEnd(c->llvm_builder, llvm_current_block_save);
        LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
    }

    temp_reset(checkpoint);
    return fn->llvm;
}

static LLVMValueRef get_builtin_func(Compiler *c, SV name, LLVMTypeRef *type) {
    const Const_Value value = get_const_definition_value(c, c->builtin_module, name, NULL);
    assert(value.kind == CONST_VALUE_FN);

    LLVMValueRef fn = compile_fn(c, value.as.fn);
    if (type) {
        *type = value.as.fn->node.type.llvm;
    }
    return fn;
}

static void compile_panic(Compiler *c, const char *fmt, LLVMValueRef v1, LLVMValueRef v2, LLVMValueRef v3) {
    LLVMTypeRef  fn_type = NULL;
    LLVMValueRef fn_value = get_builtin_func(c, sv_from_cstr("panic_handler"), &fn_type);

    LLVMValueRef zero = LLVMConstNull(LLVMInt64TypeInContext(c->llvm_context));
    LLVMValueRef args[] = {
        LLVMBuildGlobalString(c->llvm_builder, fmt, ""),
        v1 ? v1 : zero,
        v2 ? v2 : zero,
        v3 ? v3 : zero,
    };

    LLVMBuildCall2(c->llvm_builder, fn_type, fn_value, args, len(args), "");
    LLVMBuildUnreachable(c->llvm_builder);
}

static LLVMValueRef compile_cast(Compiler *c, LLVMValueRef from, LLVMTypeRef to_type, bool is_signed) {
    LLVMTypeRef from_type = LLVMTypeOf(from);
    if (from_type == to_type) {
        return from;
    }

    LLVMTypeKind from_kind = LLVMGetTypeKind(from_type);
    LLVMTypeKind to_kind = LLVMGetTypeKind(to_type);

    // Pointer -> Integer
    if (from_kind == LLVMPointerTypeKind && to_kind == LLVMIntegerTypeKind) {
        return LLVMBuildPtrToInt(c->llvm_builder, from, to_type, "");
    }

    // Integer -> Pointer
    if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMPointerTypeKind) {
        return LLVMBuildIntToPtr(c->llvm_builder, from, to_type, "");
    }

    // Integer -> Integer
    if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMIntegerTypeKind) {
        const size_t from_width = LLVMGetIntTypeWidth(from_type);
        const size_t to_width = LLVMGetIntTypeWidth(to_type);
        if (from_width > to_width) {
            return LLVMBuildTrunc(c->llvm_builder, from, to_type, "");
        } else if (from_width < to_width) {
            // Smaller -> Bigger
            if (is_signed) {
                return LLVMBuildSExt(c->llvm_builder, from, to_type, "");
            }
            return LLVMBuildZExt(c->llvm_builder, from, to_type, "");
        } else {
            // Bigger -> Smaller
            return LLVMBuildBitCast(c->llvm_builder, from, to_type, "");
        }
    }

    unreachable();
}

static LLVMValueRef compile_ident(Compiler *c, Node *n, Node_Atom *definition, bool ref) {
    Token token = {0};
    if (n->kind == NODE_ATOM) {
        token = n->token;
    } else if (n->kind == NODE_MEMBER) {
        token = ((Node_Member *) n)->field;
    } else {
        unreachable();
    }

    assert(definition);
    if (definition->definition_spec->is_const) {
        const Const_Value const_value = definition->definition_spec->const_value;

        static_assert(COUNT_CONST_VALUES == 6, "");
        switch (const_value.kind) {
        case CONST_VALUE_STRUCT:
            if (!definition->definition_spec->llvm) {
                const char *name = temp_sprintf("const.anon.%zu", c->iota_anonymous_const++);
                definition->definition_spec->llvm = LLVMAddGlobal(c->llvm_module, n->type.llvm, name);
                temp_reset(name);
                LLVMSetInitializer(definition->definition_spec->llvm, compile_const_value(c, const_value, n->type));
            }

            if (ref) {
                return definition->definition_spec->llvm;
            }

            set_debug_pos(c, token.pos);
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->definition_spec->llvm, "");

        case CONST_VALUE_STRING: {
            const SV sv = const_value.as.string;
            if (!definition->definition_spec->llvm) {
                LLVMValueRef memory = LLVMConstStringInContext(c->llvm_context, sv.data, sv.count, false);
                definition->definition_spec->llvm = LLVMAddGlobal(c->llvm_module, LLVMTypeOf(memory), "");
                LLVMSetInitializer(definition->definition_spec->llvm, memory);
            }
            return compile_string_struct(c, definition->definition_spec->llvm, sv.count, &token.pos, ref);
        }

        default:
            if (!definition->definition_spec->llvm) {
                definition->definition_spec->llvm = compile_const_value(c, const_value, n->type);
            }
            return definition->definition_spec->llvm;
        }
    }

    if (!definition->definition_spec->llvm) {
        compile_stmt(c, (Node *) definition->definition_spec->definition_node);
    }

    if (ref) {
        return definition->definition_spec->llvm;
    }

    set_debug_pos(c, token.pos);
    return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->definition_spec->llvm, "");
}

static_assert(COUNT_NODES == 26, "");
static LLVMValueRef compile_expr(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    if (n->type.kind == TYPE_MODULE) {
        return NULL;
    }

    if (n->type.kind != TYPE_GROUP) {
        compile_type(c, &n->type);
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_INT:
        case TOKEN_BOOL:
        case TOKEN_CHAR:
            return LLVMConstInt(n->type.llvm, n->token.as.integer, type_is_signed(n->type));

        case TOKEN_NULL:
            return LLVMConstNull(n->type.llvm);

        case TOKEN_IDENT:
            return compile_ident(c, n, (Node_Atom *) atom->definition, ref);

        case TOKEN_STRING:
            return compile_string(c, n->token.sv, &n->token.pos, ref);

        case TOKEN_DIRECTIVE_MAIN:
            return compile_fn(c, c->main_fn);

        case TOKEN_DIRECTIVE_PLATFORM:
            return compile_const_value(c, get_platform(c, NULL), n->type);

        default:
            unreachable();
        }
    }

    case NODE_GROUP: {
        Node_Group *group = (Node_Group *) n;
        ll_foreach(it, &group->nodes) {
            LLVMValueRef value = compile_expr(c, it, ref);
            if (it->type.kind != TYPE_GROUP) {
                da_push(&c->group_values, value);
            }
        }
        return NULL;
    }

    case NODE_GHOST: {
        Node_Ghost *ghost = (Node_Ghost *) n;
        LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);

        if (ghost->arg->default_value_is_caller_location) {
            const char *cstr = temp_sprintf(Pos_Fmt, Pos_Arg(n->token.pos));

            SV sv = sv_from_cstr(cstr);
            // Since Pos_Fmt is `%s:%zu:%zu `
            //                             ^
            //                             This space here
            //
            // TODO: Remove this space from the macro itself
            sv.count -= 1;

            LLVMValueRef value = compile_string(c, sv, NULL, ref);
            temp_reset(cstr);
            return value;
        }

        const Const_Value const_value = *ghost->arg->default_value;
        static_assert(COUNT_CONST_VALUES == 6, "");
        switch (const_value.kind) {
        case CONST_VALUE_STRUCT:
            if (!ghost->arg->default_value_llvm) {
                const char *name = temp_sprintf("const.anon.%zu", c->iota_anonymous_const++);
                ghost->arg->default_value_llvm = LLVMAddGlobal(c->llvm_module, n->type.llvm, name);
                temp_reset(name);
                LLVMSetInitializer(ghost->arg->default_value_llvm, compile_const_value(c, const_value, n->type));
            }

            if (ref) {
                return ghost->arg->default_value_llvm;
            }

            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, ghost->arg->default_value_llvm, "");

        case CONST_VALUE_STRING:
            return compile_string(c, const_value.as.string, NULL, ref);

        default:
            return compile_const_value(c, const_value, n->type);
        }
    }

    case NODE_UNARY: {
        Node_Unary  *unary = (Node_Unary *) n;
        LLVMValueRef value = NULL;

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            value = compile_expr(c, unary->value, false);
            set_debug_pos(c, n->token.pos);
            return LLVMBuildNeg(c->llvm_builder, value, "");

        case TOKEN_MUL:
            value = compile_expr(c, unary->value, false);
            if (ref) {
                return value;
            }

            set_debug_pos(c, n->token.pos);
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, value, "");

        case TOKEN_BAND:
            return compile_expr(c, unary->value, true);

        case TOKEN_BNOT:
            value = compile_expr(c, unary->value, false);
            set_debug_pos(c, n->token.pos);
            return LLVMBuildNot(c->llvm_builder, value, "");

        case TOKEN_LNOT:
            value = compile_expr(c, unary->value, false);
            set_debug_pos(c, n->token.pos);
            return LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, value, LLVMConstNull(n->type.llvm), "");

        case TOKEN_SIZEOF:
            return LLVMConstInt(n->type.llvm, compile_sizeof(c, &unary->value->type), false);

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;

        // String comparison
        if (binary->lhs->type.kind == TYPE_STRING) {
            LLVMValueRef lhs = compile_expr(c, binary->lhs, true);
            LLVMValueRef rhs = compile_expr(c, binary->rhs, true);
            set_debug_pos(c, n->token.pos);

            LLVMTypeRef  fn_type = NULL;
            LLVMValueRef fn_value = get_builtin_func(c, sv_from_cstr("string_eq"), &fn_type);

            LLVMValueRef args[] = {lhs, rhs};
            LLVMValueRef equal = LLVMBuildCall2(c->llvm_builder, fn_type, fn_value, args, len(args), "");

            switch (n->token.kind) {
            case TOKEN_EQ:
                return equal;

            case TOKEN_NE:
                return LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, equal, LLVMConstNull(n->type.llvm), "");

            default:
                unreachable();
            }
        }

        // Arithmetic
        {
            typedef struct {
                LLVMValueRef (*i)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
                LLVMValueRef (*u)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
            } Op;

            static_assert(COUNT_TOKENS == 69, "");
            static const Op ops[COUNT_TOKENS] = {
                [TOKEN_ADD] = {.i = LLVMBuildAdd},
                [TOKEN_SUB] = {.i = LLVMBuildSub},
                [TOKEN_MUL] = {.i = LLVMBuildMul},
                [TOKEN_DIV] = {.i = LLVMBuildSDiv, .u = LLVMBuildUDiv},
                [TOKEN_MOD] = {.i = LLVMBuildSRem, .u = LLVMBuildURem},

                [TOKEN_SHL] = {.i = LLVMBuildShl},
                [TOKEN_SHR] = {.i = LLVMBuildAShr, .u = LLVMBuildLShr},
                [TOKEN_BOR] = {.i = LLVMBuildOr},
                [TOKEN_BAND] = {.i = LLVMBuildAnd},
            };

            const Op op = ops[n->token.kind];
            if (op.i) {
                LLVMValueRef lhs = compile_expr(c, binary->lhs, false);
                LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
                LLVMValueRef result = NULL;

                const bool is_pointer_arithmetic = type_is_pointer(n->type);
                if (is_pointer_arithmetic) {
                    LLVMTypeRef llvm_type_i64 = LLVMInt64TypeInContext(c->llvm_context);
                    lhs = LLVMBuildPtrToInt(c->llvm_builder, lhs, llvm_type_i64, "");
                    rhs = LLVMBuildPtrToInt(c->llvm_builder, rhs, llvm_type_i64, "");
                }

                set_debug_pos(c, n->token.pos);
                if (op.u && !type_is_signed(binary->lhs->type)) {
                    result = op.u(c->llvm_builder, lhs, rhs, "");
                } else {
                    result = op.i(c->llvm_builder, lhs, rhs, "");
                }

                if (is_pointer_arithmetic) {
                    result = LLVMBuildIntToPtr(c->llvm_builder, result, n->type.llvm, "");
                }
                return result;
            }
        }

        // Comparison
        {
            typedef struct {
                LLVMIntPredicate i;
                LLVMIntPredicate u;
            } Op;

            static_assert(COUNT_TOKENS == 69, "");
            static const Op ops[COUNT_TOKENS] = {
                [TOKEN_GT] = {.i = LLVMIntSGT, .u = LLVMIntUGT},
                [TOKEN_GE] = {.i = LLVMIntSGE, .u = LLVMIntUGE},
                [TOKEN_LT] = {.i = LLVMIntSLT, .u = LLVMIntULT},
                [TOKEN_LE] = {.i = LLVMIntSLE, .u = LLVMIntULE},
                [TOKEN_EQ] = {.i = LLVMIntEQ},
                [TOKEN_NE] = {.i = LLVMIntNE},
            };

            const Op op = ops[n->token.kind];
            if (op.i) {
                LLVMValueRef lhs = compile_expr(c, binary->lhs, false);
                LLVMValueRef rhs = compile_expr(c, binary->rhs, false);

                set_debug_pos(c, n->token.pos);
                if (op.u && !type_is_signed(binary->lhs->type)) {
                    return LLVMBuildICmp(c->llvm_builder, op.u, lhs, rhs, "");
                } else {
                    return LLVMBuildICmp(c->llvm_builder, op.i, lhs, rhs, "");
                }
            }
        }

        // Arithmetic assignment
        {
            typedef struct {
                LLVMValueRef (*i)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
                LLVMValueRef (*u)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
            } Op;

            static_assert(COUNT_TOKENS == 69, "");
            static const Op ops[COUNT_TOKENS] = {
                [TOKEN_ADD_SET] = {.i = LLVMBuildAdd},
                [TOKEN_SUB_SET] = {.i = LLVMBuildSub},
                [TOKEN_MUL_SET] = {.i = LLVMBuildMul},
                [TOKEN_DIV_SET] = {.i = LLVMBuildSDiv, .u = LLVMBuildUDiv},
                [TOKEN_MOD_SET] = {.i = LLVMBuildSRem, .u = LLVMBuildURem},

                [TOKEN_SHL_SET] = {.i = LLVMBuildShl},
                [TOKEN_SHR_SET] = {.i = LLVMBuildAShr, .u = LLVMBuildLShr},
                [TOKEN_BOR_SET] = {.i = LLVMBuildOr},
                [TOKEN_BAND_SET] = {.i = LLVMBuildAnd},
            };

            const Op op = ops[n->token.kind];
            if (op.i) {
                const size_t group_values_count_save = c->group_values.count;
                const size_t group_count =
                    binary->lhs->type.kind == TYPE_GROUP ? binary->lhs->type.spec.group.count : 0;

                const bool  is_pointer_arithmetic = type_is_pointer(n->type);
                LLVMTypeRef llvm_type_i64 = LLVMInt64TypeInContext(c->llvm_context);
                LLVMTypeRef llvm_type_void = LLVMVoidTypeInContext(c->llvm_context);

                const size_t group_values_ptr_start = c->group_values.count;
                LLVMValueRef ptr = compile_expr(c, binary->lhs, true);

                const size_t group_values_lhs_start = c->group_values.count;
                LLVMValueRef lhs = NULL;
                if (group_count) {
                    assert(c->group_values.count == group_values_count_save + group_count);
                    for (size_t i = 0; i < group_count; i++) {
                        LLVMValueRef ptr = c->group_values.data[group_values_ptr_start + i];

                        Type *type = &binary->lhs->type.spec.group.data[i];
                        compile_type(c, type);

                        LLVMValueRef lhs = LLVMBuildLoad2(c->llvm_builder, type->llvm, ptr, "");
                        if (is_pointer_arithmetic) {
                            lhs = LLVMBuildPtrToInt(c->llvm_builder, lhs, llvm_type_i64, "");
                        }
                        da_push(&c->group_values, lhs);
                    }
                    assert(c->group_values.count == group_values_count_save + group_count * 2);
                } else {
                    lhs = LLVMBuildLoad2(c->llvm_builder, binary->lhs->type.llvm, ptr, "");
                    if (is_pointer_arithmetic) {
                        lhs = LLVMBuildPtrToInt(c->llvm_builder, lhs, llvm_type_i64, "");
                    }
                }

                const size_t group_values_rhs_start = c->group_values.count;
                LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
                if (group_count) {
                    assert(c->group_values.count == group_values_count_save + group_count * 3);
                    for (size_t i = 0; i < group_count; i++) {
                        LLVMValueRef *rhs = &c->group_values.data[group_values_rhs_start + i];
                        if (is_pointer_arithmetic) {
                            *rhs = LLVMBuildPtrToInt(c->llvm_builder, *rhs, llvm_type_i64, "");
                        }
                    }
                } else {
                    if (is_pointer_arithmetic) {
                        rhs = LLVMBuildPtrToInt(c->llvm_builder, rhs, llvm_type_i64, "");
                    }
                }

                set_debug_pos(c, n->token.pos);
                if (group_count) {
                    assert(c->group_values.count == group_values_count_save + group_count * 3);
                    for (size_t i = 0; i < group_count; i++) {
                        LLVMValueRef ptr = c->group_values.data[group_values_ptr_start + i];
                        LLVMValueRef lhs = c->group_values.data[group_values_lhs_start + i];
                        LLVMValueRef rhs = c->group_values.data[group_values_rhs_start + i];

                        LLVMValueRef result = NULL;
                        if (op.u && !type_is_signed(binary->lhs->type)) {
                            result = op.u(c->llvm_builder, lhs, rhs, "");
                        } else {
                            result = op.i(c->llvm_builder, lhs, rhs, "");
                        }

                        if (is_pointer_arithmetic) {
                            result = LLVMBuildIntToPtr(c->llvm_builder, result, llvm_type_void, "");
                        }
                        LLVMBuildStore(c->llvm_builder, result, ptr);
                    }
                } else {
                    LLVMValueRef result = NULL;
                    if (op.u && !type_is_signed(binary->lhs->type)) {
                        result = op.u(c->llvm_builder, lhs, rhs, "");
                    } else {
                        result = op.i(c->llvm_builder, lhs, rhs, "");
                    }

                    if (is_pointer_arithmetic) {
                        result = LLVMBuildIntToPtr(c->llvm_builder, result, llvm_type_void, "");
                    }
                    LLVMBuildStore(c->llvm_builder, result, ptr);
                }

                c->group_values.count = group_values_count_save;
                return NULL;
            }
        }

        static_assert(COUNT_TOKENS == 69, "");
        switch (n->token.kind) {
        case TOKEN_SET: {
            const size_t group_values_count_save = c->group_values.count;

            LLVMValueRef lhs = compile_expr(c, binary->lhs, true);
            LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
            set_debug_pos(c, n->token.pos);

            if (binary->lhs->type.kind == TYPE_GROUP) {
                const size_t count = binary->lhs->type.spec.group.count;
                assert(c->group_values.count == group_values_count_save + count * 2);
                for (size_t i = 0; i < count; i++) {
                    LLVMValueRef ptr = c->group_values.data[group_values_count_save + i];
                    LLVMValueRef value = c->group_values.data[group_values_count_save + count + i];
                    LLVMBuildStore(c->llvm_builder, value, ptr);
                }
            } else {
                LLVMBuildStore(c->llvm_builder, rhs, lhs);
            }

            c->group_values.count = group_values_count_save;
            return NULL;
        }

        default:
            unreachable();
        }
    }

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        if (member->is_enum) {
            return LLVMConstInt(n->type.llvm, member->enum_value, type_is_signed(n->type));
        }

        if (member->lhs->type.kind == TYPE_MODULE) {
            return compile_ident(c, n, member->module_access_definition, ref);
        }

        LLVMValueRef lhs = NULL;
        LLVMTypeRef  lhs_type = NULL;

        if (member->lhs->type.ref) {
            lhs = compile_expr(c, member->lhs, false);
            set_debug_pos(c, n->token.pos);

            LLVMTypeRef llvm_type_ptr = LLVMPointerTypeInContext(c->llvm_context, 0);
            for (size_t i = 1; i < member->lhs->type.ref; i++) {
                lhs = LLVMBuildLoad2(c->llvm_builder, llvm_type_ptr, lhs, "");
            }

            Type type = member->lhs->type;
            type.ref = 0;
            type.llvm = NULL;

            compile_type(c, &type);
            lhs_type = type.llvm;
        } else {
            lhs = compile_expr(c, member->lhs, true);
            lhs_type = member->lhs->type.llvm;
            set_debug_pos(c, n->token.pos);
        }

        LLVMValueRef ptr = LLVMBuildStructGEP2(c->llvm_builder, lhs_type, lhs, member->field_index, "");
        if (ref) {
            return ptr;
        }
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, ptr, "");
    }

    case NODE_IMPORT:
        unreachable();

    case NODE_FN:
        return compile_fn(c, (Node_Fn *) n);

    case NODE_ENUM:
    case NODE_STRUCT:
        unreachable();

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;

        LLVMValueRef memory = compile_alloca(c, n->type.llvm);
        LLVMBuildStore(c->llvm_builder, LLVMConstNull(n->type.llvm), memory);

        size_t ordered_iota = 0;
        for (Node *iter = compound->children.head; iter; iter = iter->next) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            Node *it = iter;
            if (n->type.kind == TYPE_STRUCT) {
                if (compound->is_designated) {
                    assert(it->kind == NODE_BINARY && it->token.kind == TOKEN_SET);
                    Node_Binary *it_binary = (Node_Binary *) it;
                    it_iota = it->token.as.integer;
                    it = it_binary->rhs;
                }

                LLVMValueRef field = LLVMBuildStructGEP2(c->llvm_builder, n->type.llvm, memory, it_iota, "");
                LLVMValueRef value = compile_expr(c, it, false);
                LLVMBuildStore(c->llvm_builder, value, field);
            } else {
                unreachable();
            }
        }

        if (ref) {
            return memory;
        }
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, memory, "");
    }

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        if (call->is_type_cast) {
            LLVMValueRef from = compile_expr(c, call->args.head, false);
            LLVMTypeRef  from_type = call->args.head->type.llvm;

            static_assert(COUNT_TYPE_CASTS == 3, "");
            switch (call->type_cast) {
            case TYPE_CAST_NOP:
                return from;

            case TYPE_CAST_NORMAL:
                set_debug_pos(c, n->token.pos);
                return compile_cast(c, from, n->type.llvm, type_is_signed(call->args.head->type));

            case TYPE_CAST_TO_BOOL:
                set_debug_pos(c, n->token.pos);
                return LLVMBuildICmp(c->llvm_builder, LLVMIntNE, from, LLVMConstNull(from_type), "");

            default:
                unreachable();
            }
        }

        LLVMValueRef fn_value = compile_expr(c, call->fn, false);

        const void *checkpoint = temp_alloc(0);

        ABI abi = {0};
        abi.args = temp_alloc(call->args_count * sizeof(*abi.args));
        abi.args_count = call->args_count;

        const Type_Fn fn_type_spec = call->fn->type.spec.fn;
        abi_set_return_type(c, &abi, fn_type_spec.return_type);
        {
            size_t iota = 0;
            for (Node *arg = call->args.head; arg; arg = arg->next) {
                if (arg->type.kind == TYPE_GROUP) {
                    Type_Group *group = &arg->type.spec.group;
                    for (size_t i = 0; i < group->count; i++) {
                        abi_set_argument_type(c, &abi, iota++, &group->data[i]);
                    }
                } else {
                    abi_set_argument_type(c, &abi, iota++, &arg->type);
                }
            }

            if (fn_type_spec.is_variadic) {
                abi_set_variadic_at(&abi, fn_type_spec.args_count);
            }

            abi.actual_args = temp_alloc(abi.actual_args_count * sizeof(*abi.actual_args));
        }
        LLVMTypeRef fn_type = abi_finalize(c, &abi);

        ABI_Call abi_call = abi_call_create(c, abi, fn_value, fn_type);
        for (Node *arg = call->args.head; arg; arg = arg->next) {
            const size_t group_values_count_save = c->group_values.count;

            LLVMValueRef expr = compile_expr(c, arg, false);
            if (arg->type.kind == TYPE_GROUP) {
                Type_Group *group = &arg->type.spec.group;
                assert(c->group_values.count == group_values_count_save + group->count);
                for (size_t i = 0; i < group->count; i++) {
                    abi_call_add_arg(c, &abi_call, c->group_values.data[group_values_count_save + i], group->data[i]);
                }
            } else {
                abi_call_add_arg(c, &abi_call, expr, arg->type);
            }

            c->group_values.count = group_values_count_save;
        }

        set_debug_pos(c, n->token.pos);

        const bool   is_group = n->type.kind == TYPE_GROUP;
        LLVMValueRef result = abi_call_finalize(c, &abi_call, ref || is_group);
        if (is_group) {
            assert(!ref);
            compile_type(c, &n->type);
            Type_Group *spec = &n->type.spec.group;
            for (size_t i = 0; i < spec->count; i++) {
                LLVMValueRef ptr = LLVMBuildStructGEP2(c->llvm_builder, spec->llvm, result, i, "");
                da_push(&c->group_values, LLVMBuildLoad2(c->llvm_builder, spec->data[i].llvm, ptr, ""));
            }
            result = NULL;
        }
        temp_reset(checkpoint);
        return result;
    }

    case NODE_SLICE:
        unreachable();

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;

        const char *label = "slice";
        if (!index->lhs->type.ref) {
            static_assert(COUNT_TYPES == 21, "");
            switch (index->lhs->type.kind) {
            case TYPE_SLICE:
                // Pass
                break;

            case TYPE_STRING:
                label = "string";
                break;

            default:
                unreachable();
                break;
            }
        }

        LLVMValueRef lhs = compile_expr(c, index->lhs, !index->lhs->type.ref);
        LLVMValueRef a = compile_expr(c, index->a, false);

        Type  element_type_buffer = {0};
        Type *element_type = &element_type_buffer;
        if (index->lhs->type.ref) {
            element_type = n->type.spec.slice.element;
        } else if (index->lhs->type.kind == TYPE_SLICE) {
            element_type = index->lhs->type.spec.slice.element;
        } else if (index->lhs->type.kind == TYPE_STRING) {
            element_type_buffer.kind = TYPE_CHAR;
        } else {
            unreachable();
        }
        compile_type(c, element_type);

        if (index->is_ranged) {
            if (a) {
                a = compile_cast(c, a, LLVMInt64TypeInContext(c->llvm_context), type_is_signed(index->a->type));
            } else {
                a = LLVMConstNull(LLVMInt64TypeInContext(c->llvm_context));
            }

            LLVMValueRef b = compile_expr(c, index->b, false);
            if (b) {
                b = compile_cast(c, b, LLVMInt64TypeInContext(c->llvm_context), type_is_signed(index->b->type));
            }

            set_debug_pos(c, n->token.pos);

            LLVMValueRef ptr = NULL;
            LLVMValueRef count = NULL;
            if (index->lhs->type.ref) {
                ptr = lhs;
            } else if (index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) {
                ptr = LLVMBuildLoad2(c->llvm_builder, LLVMPointerTypeInContext(c->llvm_context, 0), lhs, "");
                count = LLVMBuildLoad2(
                    c->llvm_builder,
                    LLVMInt64TypeInContext(c->llvm_context),
                    LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, lhs, 1, ""),
                    "");

                if (!b) {
                    b = count;
                }
            } else {
                unreachable();
            }

            // Check if bounds are ascending
            {
                LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

                LLVMValueRef check = LLVMBuildICmp(c->llvm_builder, LLVMIntSLE, a, b, "");
                LLVMBuildCondBr(c->llvm_builder, check, success, failure);

                // Failure
                LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
                {
                    const char *message = temp_sprintf(
                        Pos_Fmt "Range (%%zd..%%zd) is invalid: Beginning of range is more than end\n",
                        Pos_Arg(n->token.pos));

                    compile_panic(c, message, a, b, NULL);
                    temp_reset(message);
                }

                // Success
                LLVMPositionBuilderAtEnd(c->llvm_builder, success);
            }

            if (count) {
                // Bounds check
                {
                    LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                    LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

                    LLVMValueRef check_begin_of_a =
                        LLVMBuildICmp(c->llvm_builder, LLVMIntSGE, a, LLVMConstNull(LLVMTypeOf(a)), "");
                    LLVMValueRef check_end_of_a = LLVMBuildICmp(c->llvm_builder, LLVMIntSLE, a, count, "");
                    LLVMValueRef check_a = LLVMBuildAnd(c->llvm_builder, check_begin_of_a, check_end_of_a, "");

                    LLVMValueRef check_begin_of_b =
                        LLVMBuildICmp(c->llvm_builder, LLVMIntSGE, b, LLVMConstNull(LLVMTypeOf(b)), "");
                    LLVMValueRef check_end_of_b = LLVMBuildICmp(c->llvm_builder, LLVMIntSLE, b, count, "");
                    LLVMValueRef check_b = LLVMBuildAnd(c->llvm_builder, check_begin_of_b, check_end_of_b, "");

                    LLVMValueRef check = LLVMBuildAnd(c->llvm_builder, check_a, check_b, "");
                    LLVMBuildCondBr(c->llvm_builder, check, success, failure);

                    // Failure
                    LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
                    {
                        const char *message = temp_sprintf(
                            Pos_Fmt "Range (%%zd..%%zd) is out of bounds in %s of length %%zd\n",
                            Pos_Arg(n->token.pos),
                            label);

                        compile_panic(c, message, a, b, count);
                        temp_reset(message);
                    }

                    // Success
                    LLVMPositionBuilderAtEnd(c->llvm_builder, success);
                }
            }

            LLVMValueRef slice_data = LLVMBuildGEP2(c->llvm_builder, element_type->llvm, ptr, &a, 1, "");
            LLVMValueRef slice_count = LLVMBuildSub(c->llvm_builder, b, a, "");
            LLVMValueRef slice_struct = compile_alloca(c, n->type.llvm);
            LLVMBuildStore(c->llvm_builder, slice_data, slice_struct);
            LLVMBuildStore(
                c->llvm_builder,
                slice_count,
                LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, slice_struct, 1, ""));

            if (ref) {
                return slice_struct;
            }

            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, slice_struct, "");
        }

        set_debug_pos(c, n->token.pos);

        // Bounds check
        {
            LLVMValueRef count = NULL;
            if (index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) {
                count = LLVMBuildStructGEP2(c->llvm_builder, index->lhs->type.llvm, lhs, 1, "");
                count = LLVMBuildLoad2(c->llvm_builder, LLVMInt64TypeInContext(c->llvm_context), count, "");
            } else {
                unreachable();
            }

            LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

            LLVMValueRef check_begin_of_a =
                LLVMBuildICmp(c->llvm_builder, LLVMIntSGE, a, LLVMConstNull(LLVMTypeOf(a)), "");
            LLVMValueRef check_end_of_a = LLVMBuildICmp(c->llvm_builder, LLVMIntSLT, a, count, "");
            LLVMValueRef check = LLVMBuildAnd(c->llvm_builder, check_begin_of_a, check_end_of_a, "");

            LLVMBuildCondBr(c->llvm_builder, check, success, failure);

            // Failure
            LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
            {
                const char *message = temp_sprintf(
                    Pos_Fmt "Index %%zd is out of bounds in %s of length %%zd\n", Pos_Arg(n->token.pos), label);

                compile_panic(c, message, a, count, NULL);
                temp_reset(message);
            }

            // Success
            LLVMPositionBuilderAtEnd(c->llvm_builder, success);
        }

        LLVMValueRef ptr = NULL;
        if (index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) {
            ptr = LLVMBuildLoad2(c->llvm_builder, LLVMPointerTypeInContext(c->llvm_context, 0), lhs, "");
        } else {
            unreachable();
        }
        ptr = LLVMBuildGEP2(c->llvm_builder, element_type->llvm, ptr, &a, 1, "");

        if (ref) {
            return ptr;
        }
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, ptr, "");
    }

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_NODES == 26, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT:
        // Pass
        break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        if (define->is_const) {
            return;
        }

        if (define->is_value_known_at_compile_time) {
            Node_Atom *lhs = NULL;
            while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
                if (!lhs->definition_spec->llvm) {
                    compile_var_def(c, lhs);
                }
            }
        } else {
            const void *checkpoint = temp_alloc(0);

            LLVMValueRef *vars = NULL;
            if (define->expr) {
                vars = temp_alloc(define->count * sizeof(*vars));
            }

            Node_Atom *lhs = NULL;
            while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
                assert(!lhs->definition_spec->llvm); // These are local variables, so compiled in an ordered fashion
                compile_var_def(c, lhs);
                if (define->expr) {
                    vars[lhs->definition_spec->group_index] = lhs->definition_spec->llvm;
                }
            }

            if (define->expr) {
                const size_t group_values_count_save = c->group_values.count;

                LLVMValueRef value = compile_expr(c, define->expr, false);
                set_debug_pos(c, define->node.token.pos);
                if (define->count == 1) {
                    LLVMBuildStore(c->llvm_builder, value, vars[0]);
                } else {
                    for (size_t i = 0; i < define->count; i++) {
                        LLVMValueRef value = c->group_values.data[group_values_count_save + i];
                        LLVMBuildStore(c->llvm_builder, value, vars[i]);
                    }
                }

                c->group_values.count = group_values_count_save;
            }

            temp_reset(checkpoint);
        }
    } break;

    case NODE_BLOCK: {
        const size_t defers_count_save = c->defers.count;

        LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;
        c->llvm_debug_scope = LLVMDIBuilderCreateLexicalBlock(
            c->llvm_debug_builder,
            c->llvm_debug_scope,
            get_debug_file(c, n->token.pos.path),
            n->token.pos.row + 1,
            n->token.pos.col + 1);

        Node_Block *block = (Node_Block *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            compile_stmt(c, it);
        }

        compile_defers(c, defers_count_save, true);
        c->llvm_debug_scope = llvm_debug_scope_save;
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            if (iff->compile_time_real_block) {
                if (iff->compile_time_real_block->kind == NODE_IF) {
                    compile_stmt(c, iff->compile_time_real_block);
                } else if (iff->compile_time_real_block->kind == NODE_BLOCK) {
                    Node_Block *block = (Node_Block *) iff->compile_time_real_block;
                    for (Node *it = block->body.head; it; it = it->next) {
                        compile_stmt(c, it);
                    }
                } else {
                    unreachable();
                }
            }
            return;
        }

        LLVMBasicBlockRef consequence = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMBasicBlockRef antecedence = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

        LLVMBasicBlockRef end = antecedence;
        if (iff->antecedence) {
            end = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        }

        // Condition
        LLVMValueRef condition = compile_expr(c, iff->condition, false);
        set_debug_pos(c, n->token.pos);
        LLVMBuildCondBr(c->llvm_builder, condition, consequence, antecedence);

        // Consequence
        LLVMPositionBuilderAtEnd(c->llvm_builder, consequence);
        compile_stmt(c, iff->consequence);

        LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
        LLVMBuildBr(c->llvm_builder, end);

        // Antecedence
        if (iff->antecedence) {
            LLVMPositionBuilderAtEnd(c->llvm_builder, antecedence);
            compile_stmt(c, iff->antecedence);

            LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
            LLVMBuildBr(c->llvm_builder, end);
        }

        // End
        LLVMPositionBuilderAtEnd(c->llvm_builder, end);
    } break;

    case NODE_FOR: {
        LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;

        Node_For *forr = (Node_For *) n;
        if (forr->init) {
            c->llvm_debug_scope = LLVMDIBuilderCreateLexicalBlock(
                c->llvm_debug_builder,
                c->llvm_debug_scope,
                get_debug_file(c, n->token.pos.path),
                n->token.pos.row + 1,
                n->token.pos.col + 1);

            compile_stmt(c, forr->init);
        }

        LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMBasicBlockRef end = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

        LLVMBasicBlockRef start = body;
        LLVMBasicBlockRef update = start;
        if (forr->update) {
            update = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        }

        LLVMBasicBlockRef llvm_loop_break_save = c->llvm_loop_break;
        c->llvm_loop_break = end;

        LLVMBasicBlockRef llvm_loop_condition_save = c->llvm_loop_continue;
        c->llvm_loop_continue = update;

        size_t loop_defers_start_save = c->loop_defers_start;
        {
            // Condition
            if (forr->condition) {
                start = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
                LLVMBuildBr(c->llvm_builder, start);
                LLVMPositionBuilderAtEnd(c->llvm_builder, start);
                LLVMBuildCondBr(c->llvm_builder, compile_expr(c, forr->condition, false), body, end);
            } else {
                LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
                LLVMBuildBr(c->llvm_builder, body);
            }

            // Body
            LLVMPositionBuilderAtEnd(c->llvm_builder, body);
            compile_stmt(c, forr->body);

            // Update
            if (forr->update) {
                LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
                LLVMBuildBr(c->llvm_builder, update);

                LLVMPositionBuilderAtEnd(c->llvm_builder, update);
                compile_expr(c, forr->update, false);
            }

            // Loop
            LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
            LLVMBuildBr(c->llvm_builder, start);

            // End
            LLVMPositionBuilderAtEnd(c->llvm_builder, end);
        }
        c->llvm_loop_break = llvm_loop_break_save;
        c->llvm_loop_continue = llvm_loop_condition_save;
        c->loop_defers_start = loop_defers_start_save;

        c->llvm_debug_scope = llvm_debug_scope_save;
    } break;

    case NODE_CASE:
        unreachable();

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            if (sw->compile_time_real_block) {
                assert(sw->compile_time_real_block->kind == NODE_BLOCK);
                Node_Block *block = (Node_Block *) sw->compile_time_real_block;
                for (Node *it = block->body.head; it; it = it->next) {
                    compile_stmt(c, it);
                }
            }
            return;
        }

        LLVMValueRef      expr = compile_expr(c, sw->expr, false);
        LLVMBasicBlockRef fallback = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMBasicBlockRef end = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

        set_debug_pos(c, n->token.pos);
        LLVMValueRef sw_llvm = LLVMBuildSwitch(c->llvm_builder, expr, fallback, sw->preds_count);

        size_t iota = 0;
        for (Node *it = sw->cases.head; it; it = it->next) {
            Node_Case *case_ = (Node_Case *) it;
            if (!case_->preds.head) {
                continue; // Fallback
            }

            LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            for (Node *pred = case_->preds.head; pred; pred = pred->next) {
                LLVMAddCase(sw_llvm, compile_const_value(c, sw->preds[iota++].value, sw->expr->type), block);
            }
            LLVMPositionBuilderAtEnd(c->llvm_builder, block);
            compile_stmt(c, case_->body);

            LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
            LLVMBuildBr(c->llvm_builder, end);
        }
        assert(iota == sw->preds_count);

        LLVMPositionBuilderAtEnd(c->llvm_builder, fallback);
        if (sw->fallback) {
            compile_stmt(c, ((Node_Case *) sw->fallback)->body);
        }

        LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
        LLVMBuildBr(c->llvm_builder, end);

        LLVMPositionBuilderAtEnd(c->llvm_builder, end);
    } break;

    case NODE_JUMP:
        compile_defers(c, c->loop_defers_start, false);
        set_debug_pos(c, n->token.pos);
        if (n->token.kind == TOKEN_BREAK) {
            LLVMBuildBr(c->llvm_builder, c->llvm_loop_break);
            LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, ""));
        } else if (n->token.kind == TOKEN_CONTINUE) {
            LLVMBuildBr(c->llvm_builder, c->llvm_loop_continue);
            LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, ""));
        } else {
            unreachable();
        }
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        da_push(&c->defers, defer->stmt);
    } break;

    case NODE_RETURN: {
        Node_Return *returnn = (Node_Return *) n;

        const size_t group_values_count_save = c->group_values.count;
        LLVMValueRef value = compile_expr(c, returnn->value, false);
        if (type_is_compound(n->type)) {
            ABI_Info abi = get_abi_info_for_type(c, &n->type);
            if (n->type.kind == TYPE_GROUP) {
                const size_t count = n->type.spec.group.count;
                assert(c->group_values.count == group_values_count_save + count);

                LLVMValueRef memory = compile_alloca(c, n->type.llvm);
                for (size_t i = 0; i < count; i++) {
                    LLVMValueRef value = c->group_values.data[group_values_count_save + i];
                    LLVMValueRef ptr = LLVMBuildStructGEP2(c->llvm_builder, n->type.llvm, memory, i, "");
                    LLVMBuildStore(c->llvm_builder, value, ptr);
                }
                value = LLVMBuildLoad2(c->llvm_builder, n->type.llvm, memory, "");
            }

            static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
            switch (abi.direct_types_count) {
            case 0:
                set_debug_pos(c, n->token.pos);
                LLVMBuildStore(c->llvm_builder, value, LLVMGetParam(c->llvm_fn, 0));
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRetVoid(c->llvm_builder);
                break;

            case 1:
                value = undo_load(value);
                value = LLVMBuildLoad2(c->llvm_builder, abi.direct_types[0], value, "");
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRet(c->llvm_builder, value);
                break;

            case 2: {
                LLVMTypeRef type =
                    LLVMStructTypeInContext(c->llvm_context, abi.direct_types, abi.direct_types_count, false);
                value = undo_load(value);
                value = LLVMBuildLoad2(c->llvm_builder, type, value, "");
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRet(c->llvm_builder, value);
            } break;

            default:
                unreachable();
            }
        } else {
            set_debug_pos(c, n->token.pos);

            compile_defers(c, c->defers_start, false);
            set_debug_pos(c, n->token.pos);
            if (n->type.kind == TYPE_UNIT) {
                LLVMBuildRetVoid(c->llvm_builder);
            } else {
                LLVMBuildRet(c->llvm_builder, value);
            }
        }

        c->group_values.count = group_values_count_save;
        LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, ""));
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            compile_stmt(c, it);
        }
    } break;

    case NODE_PRINT: {
        Node_Print *print = (Node_Print *) n;

        LLVMTypeRef  fn_type = NULL;
        LLVMValueRef fn_value = get_builtin_func(c, sv_from_cstr("print_handler"), &fn_type);

        const bool   is_signed = type_is_signed(print->value->type);
        LLVMValueRef args[] = {
            LLVMConstInt(LLVMInt1TypeInContext(c->llvm_context), is_signed, false),
            compile_cast(c, compile_expr(c, print->value, false), LLVMInt64TypeInContext(c->llvm_context), is_signed),
        };

        set_debug_pos(c, n->token.pos);
        LLVMBuildCall2(c->llvm_builder, fn_type, fn_value, args, len(args), "");
    } break;

    default: {
        const size_t group_values_count_save = c->group_values.count;
        compile_expr(c, n, false);
        c->group_values.count = group_values_count_save;
    } break;
    }
}

static void compiler_init_llvm_target_data(Compiler *c) {
    if (LLVMInitializeNativeTarget() != 0) {
        fprintf(stderr, "ERROR: Failed to initialize native target\n");
        exit(1);
    }
    LLVMInitializeNativeAsmPrinter();

    c->llvm_context = LLVMContextCreate();
    c->llvm_module = LLVMModuleCreateWithNameInContext("", c->llvm_context);

    char *triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(c->llvm_module, triple);

    char *error = NULL;

    LLVMTargetRef target = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
        fprintf(stderr, "ERROR: %s\n", error);
        exit(1);
    }

    c->llvm_target_machine = LLVMCreateTargetMachine(
        target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
    c->llvm_target_data = LLVMCreateTargetDataLayout(c->llvm_target_machine);

    free(triple);
}

size_t compile_sizeof(Compiler *c, Type *type) {
    if (!c->llvm_target_data) {
        compiler_init_llvm_target_data(c);
    }

    compile_type(c, type);
    if (!LLVMTypeIsSized(type->llvm)) {
        return 0;
    }
    return LLVMABISizeOfType(c->llvm_target_data, type->llvm);
}

void compiler_build(Compiler *c, const char *output_path) {
    const void *checkpoint = temp_alloc(0);

    assert(c->cmd);
    assert(c->arena);
    assert(c->modules);
    assert(c->main_fn);

    if (!c->llvm_context) {
        compiler_init_llvm_target_data(c);
    }
    c->llvm_builder = LLVMCreateBuilderInContext(c->llvm_context);

    c->llvm_attribute_sret = LLVMGetEnumAttributeKindForName("sret", strlen("sret"));
    c->llvm_attribute_byval = LLVMGetEnumAttributeKindForName("byval", strlen("byval"));
    c->llvm_attribute_alwaysinline = LLVMGetEnumAttributeKindForName("alwaysinline", strlen("alwaysinline"));

    c->llvm_debug_builder = LLVMCreateDIBuilder(c->llvm_module);

    c->llvm_debug_compile_unit = LLVMDIBuilderCreateCompileUnit(
        c->llvm_debug_builder,
        LLVMDWARFSourceLanguageC,
        get_debug_file(c, c->main_fn->node.token.pos.path),
        "glos",
        4,
        false,
        "",
        0,
        0,
        "",
        0,
        LLVMDWARFEmissionFull,
        0,
        0,
        0,
        "",
        0,
        "",
        0);

    for (Module *m = c->modules->head; m; m = m->next) {
        ht_foreach(g, &m->globals) {
            Node_Atom *it = *g.value;
            if (it->definition_spec->llvm) {
                continue;
            }

            if (it->definition_spec->is_const) {
                if (it->definition_spec->const_value.kind == CONST_VALUE_FN) {
                    compile_fn(c, it->definition_spec->const_value.as.fn);
                }
            } else {
                compile_var_def(c, it);
            }
        }
    }

    LLVMPassBuilderOptionsRef pass_builder_options = LLVMCreatePassBuilderOptions();
    LLVMRunPasses(c->llvm_module, "always-inline", c->llvm_target_machine, pass_builder_options);
    LLVMDisposePassBuilderOptions(pass_builder_options);

    LLVMDIBuilderFinalize(c->llvm_debug_builder);
    LLVMDisposeDIBuilder(c->llvm_debug_builder);

    const char *object_path = temp_replace_suffix(output_path, EXE_FILE_EXTENSION, OBJ_FILE_EXTENSION);
    temporary_files_push(object_path);
    {
        // TODO: Remove
        // LLVMPrintModuleToFile(c->llvm_module, "/dev/stdout", NULL);

        char *error = NULL;
        if (LLVMVerifyModule(c->llvm_module, LLVMReturnStatusAction, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

        if (LLVMTargetMachineEmitToFile(c->llvm_target_machine, c->llvm_module, object_path, LLVMObjectFile, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

        LLVMDisposeTargetData(c->llvm_target_data);
        LLVMDisposeTargetMachine(c->llvm_target_machine);

        LLVMDisposeBuilder(c->llvm_builder);
        LLVMDisposeModule(c->llvm_module);
        LLVMContextDispose(c->llvm_context);

#ifdef PLATFORM_X86_64_WINDOWS
        if (is_lld_available_in_path()) {
            cmd_push(c->cmd, "lld-link");
        } else {
            cmd_push(c->cmd, "link", "/nologo");
        }

        cmd_push(c->cmd, temp_sprintf("/out:%s", output_path));
        cmd_push(c->cmd, "/defaultlib:libcmt");
#else
        cmd_push(c->cmd, "cc");
        if (is_lld_available_in_path()) {
            cmd_push(c->cmd, "-fuse-ld=lld");
        }
        cmd_push(c->cmd, "-o", output_path);
#endif // PLATFORM_X86_64_WINDOWS

        cmd_push(c->cmd, object_path);
        cmd_push_many(c->cmd, c->link_flags->data, c->link_flags->count);

        const char *proc_name = c->cmd->data[0];
        Proc        proc = cmd_run_async(c->cmd, (Cmd_Stdio) {0});
        if (proc.id == PROC_INVALID) {
            fprintf(stderr, "ERROR: Could not execute '%s'. Make sure a C SDK is setup properly\n", proc_name);
            exit(1);
        }

        const int proc_code = cmd_wait(proc);
        if (proc_code != 0) {
            fprintf(stderr, "ERROR: Process '%s' exited abnormally with code %d\n", proc_name, proc_code);
            exit(1);
        }
    }

    ht_free(&c->llvm_debug_files);
    da_free(&c->context.locals);
    da_free(&c->group_values);
    da_free(&c->defers);
    temp_reset(checkpoint);
}
