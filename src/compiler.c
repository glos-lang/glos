#include "compiler.h"
#include "ast.h"
#include "basic.h"
#include "dwarf.h"
#include "token.h"
#include "llvm-c/Core.h"
#include "llvm-c/DebugInfo.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/TargetMachine.h>

static_assert(COUNT_AST_TYPES == 14, "");
static void compile_type(Compiler *c, AST_Type *type) {
    if (!type || type->llvm) {
        return;
    }

    // NOTE: Do not use `ast_type*` functions because this function should not care whether a type is a metatype or not.
    if (type->ref || type->kind == AST_TYPE_RAWPTR || type->kind == AST_TYPE_FN) {
        type->llvm = LLVMPointerTypeInContext(c->llvm_context, 0);
        return;
    }

    switch (type->kind) {
    case AST_TYPE_UNIT:
        type->llvm = LLVMVoidTypeInContext(c->llvm_context);
        break;

    case AST_TYPE_BOOL:
        type->llvm = LLVMInt1TypeInContext(c->llvm_context);
        break;

    case AST_TYPE_I8:
    case AST_TYPE_U8:
        type->llvm = LLVMInt8TypeInContext(c->llvm_context);
        break;

    case AST_TYPE_I16:
    case AST_TYPE_U16:
        type->llvm = LLVMInt16TypeInContext(c->llvm_context);
        break;

    case AST_TYPE_I32:
    case AST_TYPE_U32:
        type->llvm = LLVMInt32TypeInContext(c->llvm_context);
        break;

    case AST_TYPE_I64:
    case AST_TYPE_U64:
    case AST_TYPE_INT:
        type->llvm = LLVMInt64TypeInContext(c->llvm_context);
        break;

    case AST_TYPE_FN:
    case AST_TYPE_RAWPTR:
        unreachable();
        break;

    case AST_TYPE_STRUCT: {
        assert(type->spec.structt.definition);

        // TODO(@common)
        AST_Type *common = &type->spec.structt.definition->node.type;
        assert(common->kind == AST_TYPE_STRUCT);

        if (!common->llvm) {
            const AST_Type_Struct spec = common->spec.structt;

            LLVMTypeRef *fields = temp_alloc(spec.fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec.fields_count; i++) {
                AST_Node *it = (AST_Node *) spec.fields[i];
                compile_type(c, &it->type);
                fields[i] = it->type.llvm;
            }

            common->llvm = LLVMStructTypeInContext(c->llvm_context, fields, spec.fields_count, false);
            temp_reset(fields);
        }
        type->llvm = common->llvm;
    } break;

    default:
        unreachable();
        break;
    }
}

#define ABI_DIRECT_TYPES_MAX 2

typedef struct {
    LLVMTypeRef direct_types[ABI_DIRECT_TYPES_MAX];
    size_t      direct_types_count;
} ABI_Info;

static bool type_is_compound(AST_Type type) {
    return !type.ref && type.kind == AST_TYPE_STRUCT;
}

static ABI_Info get_abi_info_for_type(Compiler *c, AST_Type *type) {
    ABI_Info     info = {0};
    const size_t size = compile_sizeof(c, type);

    if (type->ref) {
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;
    }

    static_assert(COUNT_AST_TYPES == 14, "");
    switch (type->kind) {
    case AST_TYPE_UNIT:
        info.direct_types[info.direct_types_count++] = LLVMVoidTypeInContext(c->llvm_context);
        return info;

    case AST_TYPE_BOOL:
        info.direct_types[info.direct_types_count++] = LLVMInt1TypeInContext(c->llvm_context);
        return info;

    case AST_TYPE_RAWPTR:
    case AST_TYPE_FN:
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;

    default:
        // Pass
        break;
    }

    if (size <= 8) {
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
    size_t    actual_args_count;
    ABI_Info  returnn;
} ABI;

static LLVMTypeRef compile_fn_type(Compiler *c, AST_Type type, ABI *abi) {
    const void *checkpoint = temp_alloc(0);

    assert(!type.ref && type.kind == AST_TYPE_FN);
    AST_Type_Fn spec = type.spec.fn;

    compile_type(c, spec.returnn);
    abi->returnn = get_abi_info_for_type(c, spec.returnn);

    abi->actual_args_count = 0;
    if (!abi->returnn.direct_types_count) {
        abi->actual_args_count++;
    }

    for (size_t i = 0; i < spec.args_count; i++) {
        ABI_Info *it = &abi->args[i];
        *it = get_abi_info_for_type(c, &spec.args[i]->node.type);

        if (it->direct_types_count) {
            abi->actual_args_count += it->direct_types_count;
        } else {
            abi->actual_args_count++;
        }
    }

    size_t       args_iota = 0;
    LLVMTypeRef *args = temp_alloc(abi->actual_args_count * sizeof(*args));
    if (!abi->returnn.direct_types_count) {
        args[args_iota++] = LLVMPointerTypeInContext(c->llvm_context, 0);
    }

    for (size_t i = 0; i < spec.args_count; i++) {
        const ABI_Info it_abi = abi->args[i];
        if (it_abi.direct_types_count) {
            for (size_t j = 0; j < it_abi.direct_types_count; j++) {
                args[args_iota++] = it_abi.direct_types[j];
            }
        } else {
            args[args_iota++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        }
    }

    LLVMTypeRef return_type = NULL;

    static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
    switch (abi->returnn.direct_types_count) {
    case 0:
        return_type = LLVMVoidTypeInContext(c->llvm_context);
        break;

    case 1:
        return_type = abi->returnn.direct_types[0];
        break;

    case 2:
        return_type =
            LLVMStructTypeInContext(c->llvm_context, abi->returnn.direct_types, abi->returnn.direct_types_count, false);
        break;

    default:
        unreachable();
    }

    LLVMTypeRef fn_type = LLVMFunctionType(return_type, args, abi->actual_args_count, false);
    temp_reset(checkpoint);
    return fn_type;
}

static LLVMValueRef compile_fn(Compiler *c, AST_Node_Fn *fn);
static LLVMValueRef compile_expr(Compiler *c, AST_Node *n, bool ref);
static void         compile_stmt(Compiler *c, AST_Node *n);

static const char *temp_emit_fn_name(Compiler *c, AST_Node_Fn *fn) {
    const char *name = NULL;
    if (fn->outer_fn) {
        name = temp_emit_fn_name(c, fn->outer_fn);
    } else {
        name = temp_sprintf("main"); // TODO(@package)
    }

    temp_remove_null();
    if (fn->defined_as) {
        temp_sprintf("." SV_Fmt, SV_Arg(fn->defined_as->node.token.sv));
    } else {
        temp_sprintf(".anon.%zu", c->iota_anonymous_fn++);
    }
    return name;
}

static LLVMMetadataRef get_scope_of_definition(Compiler *c, AST_Node_Fn *defined_in) {
    if (!defined_in) {
        return c->llvm_debug_file;
    }

    assert(defined_in->llvm_debug_scope);
    return defined_in->llvm_debug_scope;
}

static_assert(COUNT_AST_TYPES == 14, "");
static LLVMMetadataRef get_debug_for_type(Compiler *c, AST_Type type) {
    assert(!type.is_meta);
    if (type.ref) {
        type.ref--;
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, get_debug_for_type(c, type), sizeof(void *), sizeof(void *), 0, "", 0);
    }

    switch (type.kind) {
    case AST_TYPE_UNIT:
        return NULL;

    case AST_TYPE_BOOL:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "bool", strlen("bool"), 8, DW_ATE_boolean, 0);

    case AST_TYPE_I8:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i8", strlen("i8"), 8, DW_ATE_signed, 0);

    case AST_TYPE_I16:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i16", strlen("i16"), 16, DW_ATE_signed, 0);

    case AST_TYPE_I32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i32", strlen("i32"), 32, DW_ATE_signed, 0);

    case AST_TYPE_I64:
    case AST_TYPE_INT:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i64", strlen("i64"), 64, DW_ATE_signed, 0);

    case AST_TYPE_U8:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u8", strlen("u8"), 8, DW_ATE_unsigned, 0);

    case AST_TYPE_U16:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u16", strlen("u16"), 16, DW_ATE_unsigned, 0);

    case AST_TYPE_U32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u32", strlen("u32"), 32, DW_ATE_unsigned, 0);

    case AST_TYPE_U64:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u64", strlen("u64"), 64, DW_ATE_unsigned, 0);

    case AST_TYPE_RAWPTR:
        return LLVMDIBuilderCreatePointerType(c->llvm_debug_builder, NULL, sizeof(void *), sizeof(void *), 0, "", 0);

    case AST_TYPE_FN: {
        const AST_Type_Fn spec = type.spec.fn;

        LLVMMetadataRef *args = temp_alloc((spec.args_count + 1) * sizeof(*args));
        args[0] = get_debug_for_type(c, *spec.returnn);
        for (size_t i = 0; i < spec.args_count; i++) {
            args[i + 1] = get_debug_for_type(c, spec.args[i]->node.type);
        }

        LLVMMetadataRef fn_debug_type =
            LLVMDIBuilderCreateSubroutineType(c->llvm_debug_builder, NULL, args, spec.args_count + 1, 0);

        temp_reset(args);
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, fn_debug_type, sizeof(void *), sizeof(void *), 0, "", 0);
    }

    case AST_TYPE_STRUCT: {
        assert(type.spec.structt.definition);

        // TODO(@common)
        AST_Type *common = &type.spec.structt.definition->node.type;
        assert(common->kind == AST_TYPE_STRUCT);
        if (!common->llvm) {
            compile_type(c, common);
        }

        AST_Type_Struct *spec = &common->spec.structt;
        if (!spec->debug) {
            const void *checkpoint = temp_alloc(0);

            SV             name = {0};
            AST_Node_Atom *definition = spec->definition->defined_as;
            if (definition) {
                name = definition->node.token.sv;
            } else {
                name = sv_from_cstr(temp_sprintf("anon.%zu", c->iota_anonymous_struct++));
            }

            spec->debug = LLVMDIBuilderCreateReplaceableCompositeType(
                c->llvm_debug_builder,
                DW_TAG_structure_type,
                name.data,
                name.count,
                get_scope_of_definition(c, spec->definition->defined_in),
                c->llvm_debug_file,
                definition->node.token.pos.row + 1,
                0,
                0,
                0,
                0,
                NULL,
                0);

            LLVMMetadataRef *fields = temp_alloc(spec->fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec->fields_count; i++) {
                AST_Node *it = (AST_Node *) spec->fields[i];

                const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t offset_bits = LLVMOffsetOfElement(c->llvm_target_data, common->llvm, i) * 8;

                fields[i] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    spec->debug,
                    it->token.sv.data,
                    it->token.sv.count,
                    c->llvm_debug_file,
                    it->token.pos.row + 1,
                    size_bits,
                    align_bits,
                    offset_bits,
                    0,
                    get_debug_for_type(c, it->type));
            }

            LLVMMetadataRef real = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                get_scope_of_definition(c, spec->definition->defined_in),
                name.data,
                name.count,
                c->llvm_debug_file,
                definition->node.token.pos.row + 1,
                LLVMABISizeOfType(c->llvm_target_data, common->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, common->llvm) * 8,
                0,
                NULL,
                fields,
                spec->fields_count,
                0,
                NULL,
                "",
                0);

            LLVMMetadataReplaceAllUsesWith(spec->debug, real);
            spec->debug = real;
            temp_reset(checkpoint);
        }

        return spec->debug;
    }

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

static_assert(COUNT_CONST_VALUES == 4, "");
static LLVMValueRef compile_const_value(Compiler *c, Const_Value value, AST_Type type) {
    switch (value.kind) {
    case CONST_VALUE_INT:
        return LLVMConstInt(type.llvm, value.as.integer, ast_type_is_signed(type));

    case CONST_VALUE_FN:
        return compile_fn(c, value.as.fn);

    case CONST_VALUE_TYPE:
        unreachable();

    case CONST_VALUE_STRUCT: {
        const AST_Type_Struct spec = value.as.structt.spec;

        LLVMValueRef *fields = temp_alloc(spec.fields_count * sizeof(*fields));
        for (size_t i = 0; i < spec.fields_count; i++) {
            fields[i] = compile_const_value(c, value.as.structt.fields[i], spec.fields[i]->node.type);
        }

        LLVMValueRef result = LLVMConstStructInContext(c->llvm_context, fields, spec.fields_count, false);
        temp_reset(fields);
        return result;
    }

    default:
        unreachable();
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

static void compile_local_var_debug(Compiler *c, AST_Node_Atom *it, LLVMMetadataRef var_debug_type) {
    const SV        name = it->node.token.sv;
    LLVMMetadataRef var_debug_metadata = NULL;
    if (it->arg_index) {
        var_debug_metadata = LLVMDIBuilderCreateParameterVariable(
            c->llvm_debug_builder,
            c->llvm_debug_scope,
            name.data,
            name.count,
            it->arg_index,
            c->llvm_debug_file,
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
            c->llvm_debug_file,
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
            it->llvm,
            var_debug_metadata,
            LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
            var_pos_metadata,
            next_inst);
    } else {
        LLVMDIBuilderInsertDeclareRecordAtEnd(
            c->llvm_debug_builder,
            it->llvm,
            var_debug_metadata,
            LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
            var_pos_metadata,
            LLVMGetInsertBlock(c->llvm_builder));
    }
}

static void compile_var_def(Compiler *c, AST_Node_Atom *it) {
    const void *checkpoint = temp_alloc(0);

    compile_type(c, &it->node.type);

    SV name = it->node.token.sv;
    if (!it->is_local) {
        if (it->is_extern) {
            // Guarantee a terminating '\0'
            name = sv_from_cstr(temp_sv_to_cstr(name));
        } else {
            // TODO(@package)
            name = sv_from_cstr(temp_sprintf("main." SV_Fmt, SV_Arg(name)));
        }
    }

    if (it->is_local && !it->is_extern) {
        it->llvm = compile_alloca(c, it->node.type.llvm);
    } else {
        it->llvm = LLVMAddGlobal(c->llvm_module, it->node.type.llvm, name.data);
    }

    if (!it->is_extern) {
        LLVMMetadataRef var_debug_type = get_debug_for_type(c, it->node.type);
        if (it->is_local) {
            if (!it->arg_index && !it->is_assigned) {
                LLVMBuildStore(c->llvm_builder, LLVMConstNull(it->node.type.llvm), it->llvm);
            }
            compile_local_var_debug(c, it, var_debug_type);
        } else {
            if (it->is_assigned) {
                LLVMSetInitializer(it->llvm, compile_const_value(c, it->const_value, it->node.type));
            } else {
                LLVMSetInitializer(it->llvm, LLVMConstNull(it->node.type.llvm));
            }

            LLVMMetadataRef var_debug_metadata = LLVMDIBuilderCreateGlobalVariableExpression(
                c->llvm_debug_builder,
                c->llvm_debug_file,
                name.data,
                name.count,
                name.data,
                name.count,
                c->llvm_debug_file,
                it->node.token.pos.row + 1,
                var_debug_type,
                false, // TODO: Gather more information on what even is this...
                LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
                NULL,
                0);

            LLVMGlobalSetMetadata(it->llvm, 0, var_debug_metadata);
        }
    }

    temp_reset(checkpoint);
}

static LLVMValueRef compile_fn(Compiler *c, AST_Node_Fn *fn) {
    if (fn->llvm) {
        return fn->llvm;
    }

    const void *checkpoint = temp_alloc(0);

    ABI abi = {0};
    abi.args = temp_alloc(fn->args_count * sizeof(*abi.args));
    abi.args_count = fn->args_count;
    fn->node.type.llvm = compile_fn_type(c, fn->node.type, &abi);

    if (fn->is_extern) {
        assert(fn->defined_as);
        fn->llvm = LLVMAddFunction(c->llvm_module, temp_sv_to_cstr(fn->defined_as->node.token.sv), fn->node.type.llvm);
    } else {
        LLVMValueRef llvm_fn_save = c->llvm_fn;
        LLVMValueRef llvm_fn_last_alloca_save = c->llvm_fn_last_alloca;

        LLVMMetadataRef   llvm_debug_scope_save = c->llvm_debug_scope;
        LLVMBasicBlockRef llvm_current_block_save = LLVMGetInsertBlock(c->llvm_builder);

        SV fn_name = sv_from_cstr(temp_emit_fn_name(c, fn));
        fn->llvm = LLVMAddFunction(c->llvm_module, fn_name.data, fn->node.type.llvm);

        c->llvm_fn = fn->llvm;
        c->llvm_fn_last_alloca = NULL;

        LLVMMetadataRef   fn_debug_type = NULL;
        const AST_Type_Fn fn_type_spec = fn->node.type.spec.fn;

        {

            LLVMMetadataRef *arg_debug_types = temp_alloc((fn_type_spec.args_count + 1) * sizeof(*arg_debug_types));
            arg_debug_types[0] = get_debug_for_type(c, *fn_type_spec.returnn);

            for (size_t i = 0; i < fn_type_spec.args_count; i++) {
                arg_debug_types[i + 1] = get_debug_for_type(c, fn_type_spec.args[i]->node.type);
            }

            fn_debug_type = LLVMDIBuilderCreateSubroutineType(
                c->llvm_debug_builder, c->llvm_debug_file, arg_debug_types, fn_type_spec.args_count + 1, 0);

            temp_reset(arg_debug_types);
        }

        fn->llvm_debug_scope = LLVMDIBuilderCreateFunction(
            c->llvm_debug_builder,
            get_scope_of_definition(c, fn->outer_fn),
            fn_name.data,
            fn_name.count,
            fn_name.data,
            fn_name.count,
            c->llvm_debug_file,
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
        if (!abi.returnn.direct_types_count) {
            arg_iota++;
            LLVMAttributeRef sret =
                LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_sret, fn_type_spec.returnn->llvm);
            LLVMAddAttributeAtIndex(fn->llvm, arg_iota, sret);
        }

        for (AST_Node *arg = fn->args.head; arg; arg = arg->next) {
            assert(arg->kind == AST_NODE_DEFINE);
            AST_Node_Define *define = (AST_Node_Define *) arg;

            assert(define->name->kind == AST_NODE_ATOM);
            AST_Node_Atom *it = (AST_Node_Atom *) define->name;
            assert(!it->llvm);

            const ABI_Info it_abi = abi.args[abi_iota++];

            static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
            switch (it_abi.direct_types_count) {
            case 0: {
                it->llvm = LLVMGetParam(c->llvm_fn, arg_iota++);
                compile_local_var_debug(c, it, get_debug_for_type(c, it->node.type));

#ifdef PLATFORM_X86_64_LINUX
                LLVMAttributeRef byval =
                    LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_byval, it->node.type.llvm);
                LLVMAddAttributeAtIndex(fn->llvm, arg_iota, byval);
#endif // PLATFORM_X86_64_LINUX
            } break;

            case 1:
                compile_var_def(c, it);
                LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), it->llvm);
                break;

            case 2: {
                compile_var_def(c, it);

                // First half
                LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), it->llvm);

                // Second half
                LLVMTypeRef  llvm_i8_type = LLVMInt8TypeInContext(c->llvm_context);
                LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 8, false)};
                LLVMValueRef second = LLVMBuildGEP2(c->llvm_builder, llvm_i8_type, it->llvm, indices, len(indices), "");
                LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), second);
            } break;

            default:
                unreachable();
            }
        }
        assert(arg_iota == abi.actual_args_count);

        assert(fn->body->kind == AST_NODE_BLOCK);
        AST_Node_Block *block = (AST_Node_Block *) fn->body;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            compile_stmt(c, it);
        }

        set_debug_pos(c, block->end);
        if (fn_type_spec.returnn->kind == AST_TYPE_UNIT) {
            LLVMBuildRetVoid(c->llvm_builder);
        } else {
            // The semantic analyzer has already determined that the function returns in all execution paths
            LLVMBuildUnreachable(c->llvm_builder);
        }

        c->llvm_fn = llvm_fn_save;
        c->llvm_fn_last_alloca = llvm_fn_last_alloca_save;

        c->llvm_debug_scope = llvm_debug_scope_save;
        LLVMPositionBuilderAtEnd(c->llvm_builder, llvm_current_block_save);
        LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);
    }

    temp_reset(checkpoint);
    return fn->llvm;
}

static_assert(COUNT_AST_NODES == 16, "");
static LLVMValueRef compile_expr(Compiler *c, AST_Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    compile_type(c, &n->type);
    switch (n->kind) {
    case AST_NODE_ATOM: {
        AST_Node_Atom *atom = (AST_Node_Atom *) n;

        static_assert(COUNT_TOKENS == 41, "");
        switch (n->token.kind) {
        case TOKEN_BOOL:
        case TOKEN_INT:
            return LLVMConstInt(n->type.llvm, n->token.as.integer, ast_type_is_signed(n->type));

        case TOKEN_IDENT: {
            AST_Node_Atom *definition = (AST_Node_Atom *) atom->definition;
            assert(definition);

            if (definition->is_const) {
                static_assert(COUNT_CONST_VALUES == 4, "");
                switch (definition->const_value.kind) {
                case CONST_VALUE_INT:
                    return LLVMConstInt(n->type.llvm, definition->const_value.as.integer, ast_type_is_signed(n->type));

                case CONST_VALUE_FN:
                    return compile_fn(c, definition->const_value.as.fn);

                case CONST_VALUE_TYPE:
                    unreachable();

                case CONST_VALUE_STRUCT: {
                    if (!definition->llvm) {
                        const char *name = temp_sprintf("const.anon.%zu", c->iota_anonymous_const++);
                        definition->llvm = LLVMAddGlobal(c->llvm_module, n->type.llvm, name);
                        temp_reset(name);
                        LLVMSetInitializer(definition->llvm, compile_const_value(c, definition->const_value, n->type));
                    }

                    if (ref) {
                        return definition->llvm;
                    }
                    return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->llvm, "");
                }

                default:
                    unreachable();
                }
            }

            if (!definition->llvm) {
                compile_stmt(c, (AST_Node *) definition->definition_node);
            }

            if (ref) {
                return definition->llvm;
            }

            set_debug_pos(c, n->token.pos);
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->llvm, "");
        }

        default:
            unreachable();
        }
    } break;

    case AST_NODE_UNARY: {
        AST_Node_Unary *unary = (AST_Node_Unary *) n;
        LLVMValueRef    value = NULL;

        static_assert(COUNT_TOKENS == 41, "");
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

    case AST_NODE_BINARY: {
        AST_Node_Binary *binary = (AST_Node_Binary *) n;

        {
            typedef struct {
                LLVMValueRef (*i)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
                LLVMValueRef (*u)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
            } Op;

            static_assert(COUNT_TOKENS == 41, "");
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

                const bool is_pointer_arithmetic = ast_type_is_pointer(n->type);
                if (is_pointer_arithmetic) {
                    LLVMTypeRef llvm_type_i64 = LLVMInt64TypeInContext(c->llvm_context);
                    lhs = LLVMBuildPtrToInt(c->llvm_builder, lhs, llvm_type_i64, "");
                    rhs = LLVMBuildPtrToInt(c->llvm_builder, rhs, llvm_type_i64, "");
                }

                set_debug_pos(c, n->token.pos);
                if (op.u && !ast_type_is_signed(binary->lhs->type)) {
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

        {
            typedef struct {
                LLVMIntPredicate i;
                LLVMIntPredicate u;
            } Op;

            static_assert(COUNT_TOKENS == 41, "");
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
                if (op.u && !ast_type_is_signed(binary->lhs->type)) {
                    return LLVMBuildICmp(c->llvm_builder, op.u, lhs, rhs, "");
                } else {
                    return LLVMBuildICmp(c->llvm_builder, op.i, lhs, rhs, "");
                }
            }
        }

        static_assert(COUNT_TOKENS == 41, "");
        switch (n->token.kind) {
        case TOKEN_SET: {
            LLVMValueRef lhs = compile_expr(c, binary->lhs, true);
            LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
            set_debug_pos(c, n->token.pos);
            return LLVMBuildStore(c->llvm_builder, rhs, lhs);
        }

        default:
            unreachable();
        }
    }

    case AST_NODE_MEMBER: {
        AST_Node_Member *member = (AST_Node_Member *) n;

        LLVMValueRef lhs = NULL;
        LLVMTypeRef  lhs_type = NULL;

        if (member->lhs->type.ref) {
            lhs = compile_expr(c, member->lhs, false);
            set_debug_pos(c, n->token.pos);

            LLVMTypeRef llvm_type_ptr = LLVMPointerTypeInContext(c->llvm_context, 0);
            for (size_t i = 1; i < member->lhs->type.ref; i++) {
                lhs = LLVMBuildLoad2(c->llvm_builder, llvm_type_ptr, lhs, "");
            }

            AST_Type type = member->lhs->type;
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

    case AST_NODE_FN:
        return compile_fn(c, (AST_Node_Fn *) n);

    case AST_NODE_STRUCT:
        unreachable();

    case AST_NODE_COMPOUND: {
        AST_Node_Compound *compound = (AST_Node_Compound *) n;

        LLVMValueRef memory = compile_alloca(c, n->type.llvm);
        LLVMBuildStore(c->llvm_builder, LLVMConstNull(n->type.llvm), memory);

        size_t ordered_iota = 0;
        for (AST_Node *iter = compound->children.head; iter; iter = iter->next) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            AST_Node *it = iter;
            if (n->type.kind == AST_TYPE_STRUCT) {
                if (compound->is_designated) {
                    assert(it->kind == AST_NODE_BINARY && it->token.kind == TOKEN_SET);
                    AST_Node_Binary *it_binary = (AST_Node_Binary *) it;
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

    case AST_NODE_CALL: {
        AST_Node_Call *call = (AST_Node_Call *) n;
        if (call->is_type_cast) {
            LLVMValueRef from = compile_expr(c, call->args.head, false);
            LLVMTypeRef  from_type = call->args.head->type.llvm;

            static_assert(COUNT_TYPE_CASTS == 3, "");
            switch (call->type_cast) {
            case TYPE_CAST_NOP:
                return from;

            case TYPE_CAST_NORMAL: {
                LLVMTypeRef to_type = n->type.llvm;
                if (from_type == to_type) {
                    return from;
                }
                set_debug_pos(c, n->token.pos);

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
                        if (ast_type_is_signed(call->args.head->type)) {
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

            case TYPE_CAST_TO_BOOL:
                set_debug_pos(c, n->token.pos);
                return LLVMBuildICmp(c->llvm_builder, LLVMIntNE, from, LLVMConstNull(from_type), "");

            default:
                unreachable();
            }
        }

        const void *checkpoint = temp_alloc(0);

        ABI abi = {0};
        abi.args = temp_alloc(call->args_count * sizeof(*abi.args));
        abi.args_count = call->args_count;

        LLVMValueRef fn_value = compile_expr(c, call->fn, false);
        LLVMTypeRef  fn_type = compile_fn_type(c, call->fn->type, &abi);

        LLVMValueRef *args = temp_alloc(abi.actual_args_count * sizeof(*args));

        size_t arg_iota = 0;
        if (!abi.returnn.direct_types_count) {
            args[arg_iota++] = compile_alloca(c, n->type.llvm);
        }

        size_t abi_iota = 0;
        for (AST_Node *arg = call->args.head; arg; arg = arg->next) {
            const ABI_Info arg_abi = abi.args[abi_iota++];

            static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
            switch (arg_abi.direct_types_count) {
            case 0: {
                LLVMValueRef expr = NULL;

#ifdef PLATFORM_X86_64_LINUX
                expr = compile_expr(c, arg, true);
#else
                expr = compile_expr(c, arg, false);
                LLVMValueRef temp = compile_alloca(c, arg->type.llvm);
                LLVMBuildStore(c->llvm_builder, expr, temp);
                expr = temp;
#endif // PLATFORM_X86_64_LINUX

                args[arg_iota++] = expr;
            } break;

            case 1: {
                LLVMValueRef expr = NULL;
                if (type_is_compound(arg->type)) {
                    expr = compile_expr(c, arg, true);
                    expr = LLVMBuildLoad2(c->llvm_builder, arg_abi.direct_types[0], expr, "");
                } else {
                    expr = compile_expr(c, arg, false);
                }
                args[arg_iota++] = expr;
            } break;

            case 2: {
                // First half
                LLVMValueRef first = compile_expr(c, arg, true);
                args[arg_iota++] = LLVMBuildLoad2(c->llvm_builder, arg_abi.direct_types[0], first, "");

                // Second half
                LLVMTypeRef  llvm_i8_type = LLVMInt8TypeInContext(c->llvm_context);
                LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 8, false)};
                LLVMValueRef second = LLVMBuildGEP2(c->llvm_builder, llvm_i8_type, first, indices, len(indices), "");
                args[arg_iota++] = LLVMBuildLoad2(c->llvm_builder, arg_abi.direct_types[1], second, "");
            } break;

            default:
                unreachable();
            }
        }
        assert(arg_iota == abi.actual_args_count);

        set_debug_pos(c, n->token.pos);
        LLVMValueRef result = LLVMBuildCall2(c->llvm_builder, fn_type, fn_value, args, abi.actual_args_count, "");
        LLVMValueRef memory = NULL;

        arg_iota = 0;
        if (type_is_compound(n->type)) {
            static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
            switch (abi.returnn.direct_types_count) {
            case 0: {
                memory = args[arg_iota++];
                LLVMAttributeRef sret = LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_sret, n->type.llvm);
                LLVMAddCallSiteAttribute(result, arg_iota, sret);
            } break;

            case 1:
                memory = compile_alloca(c, n->type.llvm);
                LLVMBuildStore(c->llvm_builder, result, memory);
                break;

            case 2: {
                memory = compile_alloca(c, n->type.llvm);

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
        assert(call->fn->type.kind == AST_TYPE_FN);
        const AST_Type_Fn fn_type_spec = call->fn->type.spec.fn;

        for (size_t i = 0; i < abi.args_count; i++) {
            const ABI_Info it_abi = abi.args[i];
            if (it_abi.direct_types_count) {
                arg_iota += it_abi.direct_types_count;
            } else {
                arg_iota++;

                LLVMTypeRef it_type = fn_type_spec.args[i]->node.type.llvm;
                assert(it_type);

                LLVMAttributeRef byval = LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_byval, it_type);
                LLVMAddCallSiteAttribute(result, arg_iota, byval);
            }
        }
        assert(arg_iota == abi.actual_args_count);
#endif // PLATFORM_X86_64_LINUX

        temp_reset(checkpoint);
        if (memory) {
            if (ref) {
                return memory;
            }

            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, memory, "");
        }
        return result;
    }

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_AST_NODES == 16, "");
static void compile_stmt(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case AST_NODE_DEFINE: {
        AST_Node_Define *define = (AST_Node_Define *) n;
        if (define->is_const) {
            return;
        }

        assert(define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT);
        AST_Node_Atom *it = (AST_Node_Atom *) define->name;
        AST_Node      *it_expr = define->expr;

        if (!it->llvm) {
            compile_var_def(c, it);
            if (it_expr && it->is_local) {
                set_debug_pos(c, n->token.pos);
                LLVMBuildStore(c->llvm_builder, compile_expr(c, it_expr, false), it->llvm);
            }
        }
    } break;

    case AST_NODE_BLOCK: {
        LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;
        c->llvm_debug_scope = LLVMDIBuilderCreateLexicalBlock(
            c->llvm_debug_builder, c->llvm_debug_scope, c->llvm_debug_file, n->token.pos.row + 1, n->token.pos.col + 1);

        AST_Node_Block *block = (AST_Node_Block *) n;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            compile_stmt(c, it);
        }

        c->llvm_debug_scope = llvm_debug_scope_save;
    } break;

    case AST_NODE_IF: {
        AST_Node_If *iff = (AST_Node_If *) n;

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

    case AST_NODE_FOR: {
        LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;

        AST_Node_For *forr = (AST_Node_For *) n;
        if (forr->init) {
            c->llvm_debug_scope = LLVMDIBuilderCreateLexicalBlock(
                c->llvm_debug_builder,
                c->llvm_debug_scope,
                c->llvm_debug_file,
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
        LLVMBasicBlockRef llvm_loop_condition_save = c->llvm_loop_continue;
        c->llvm_loop_break = end;
        c->llvm_loop_continue = update;
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

        c->llvm_debug_scope = llvm_debug_scope_save;
    } break;

    case AST_NODE_JUMP:
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

    case AST_NODE_RETURN: {
        AST_Node_Return *returnn = (AST_Node_Return *) n;

        LLVMValueRef value = NULL;
        if (type_is_compound(n->type)) {
            ABI_Info abi = get_abi_info_for_type(c, &n->type);

            static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
            switch (abi.direct_types_count) {
            case 0:
                value = compile_expr(c, returnn->value, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildStore(c->llvm_builder, value, LLVMGetParam(c->llvm_fn, 0));
                LLVMBuildRetVoid(c->llvm_builder);
                break;

            case 1:
                value = compile_expr(c, returnn->value, true);
                value = LLVMBuildLoad2(c->llvm_builder, abi.direct_types[0], value, "");
                set_debug_pos(c, n->token.pos);
                LLVMBuildRet(c->llvm_builder, value);
                break;

            case 2: {
                LLVMTypeRef type =
                    LLVMStructTypeInContext(c->llvm_context, abi.direct_types, abi.direct_types_count, false);
                value = compile_expr(c, returnn->value, true);
                value = LLVMBuildLoad2(c->llvm_builder, type, value, "");
                set_debug_pos(c, n->token.pos);
                LLVMBuildRet(c->llvm_builder, value);
            } break;

            default:
                unreachable();
            }
        } else {
            value = compile_expr(c, returnn->value, false);
            set_debug_pos(c, n->token.pos);
            if (n->type.kind == AST_TYPE_UNIT) {
                LLVMBuildRetVoid(c->llvm_builder);
            } else {
                LLVMBuildRet(c->llvm_builder, value);
            }
        }

        LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, ""));
    } break;

    case AST_NODE_EXTERN: {
        AST_Node_Extern *externn = (AST_Node_Extern *) n;
        for (AST_Node *it = externn->nodes.head; it; it = it->next) {
            compile_stmt(c, it);
        }
    } break;

    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;

        const bool   is_signed = ast_type_is_signed(print->value->type);
        LLVMValueRef args[] = {
            is_signed ? c->llvm_iprint_str : c->llvm_uprint_str,
            compile_expr(c, print->value, false),
        };

        const size_t value_size = compile_sizeof(c, &print->value->type);
        if (value_size != sizeof(int64_t)) {
            LLVMTypeRef llvm_type_i64 = LLVMInt64TypeInContext(c->llvm_context);
            if (is_signed) {
                args[1] = LLVMBuildSExt(c->llvm_builder, args[1], llvm_type_i64, "");
            } else {
                args[1] = LLVMBuildZExt(c->llvm_builder, args[1], llvm_type_i64, "");
            }
        }

        set_debug_pos(c, n->token.pos);
        LLVMBuildCall2(c->llvm_builder, c->llvm_printf_type, c->llvm_printf_func, args, len(args), "");
    } break;

    default:
        compile_expr(c, n, false);
        break;
    }
}

static AST_Node_Fn *get_main(Compiler *c) {
    AST_Node_Atom *main = scope_find(c->globals, sv_from_cstr("main"));
    if (!main) {
        fprintf(
            stderr,
            "ERROR: Function 'main' is not defined\n"
            "\n"
            "```\n"
            "main :: () {\n"
            "}\n"
            "```\n");
        exit(1);
    }

    if (!main->is_const || main->const_value.kind != CONST_VALUE_FN) {
        fprintf(
            stderr, Pos_Fmt "ERROR: Identifier 'main' must be a constant function\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }

    const AST_Type_Fn signature = main->node.type.spec.fn;
    if (signature.args_count) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot take any arguments\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }

    if (signature.returnn->kind != AST_TYPE_UNIT) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot return anything\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }
    return main->const_value.as.fn;
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
}

size_t compile_sizeof(Compiler *c, AST_Type *type) {
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
    assert(c->cmd);
    if (!c->llvm_context) {
        compiler_init_llvm_target_data(c);
    }
    c->llvm_builder = LLVMCreateBuilderInContext(c->llvm_context);

    c->llvm_attribute_sret = LLVMGetEnumAttributeKindForName("sret", strlen("sret"));
    c->llvm_attribute_byval = LLVMGetEnumAttributeKindForName("byval", strlen("byval"));

    c->llvm_debug_builder = LLVMCreateDIBuilder(c->llvm_module);
    c->llvm_debug_file = LLVMDIBuilderCreateFile(c->llvm_debug_builder, c->path, strlen(c->path), ".", 1);
    c->llvm_debug_scope = c->llvm_debug_file;

    c->llvm_debug_compile_unit = LLVMDIBuilderCreateCompileUnit(
        c->llvm_debug_builder,
        LLVMDWARFSourceLanguageC,
        c->llvm_debug_file,
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

    // The 'print' keyword
    {
        const char  iprint_str[] = "%ld\n";
        LLVMTypeRef iprint_type = LLVMArrayType(LLVMInt8TypeInContext(c->llvm_context), len(iprint_str));

        c->llvm_iprint_str = LLVMAddGlobal(c->llvm_module, iprint_type, "");
        LLVMSetInitializer(
            c->llvm_iprint_str, LLVMConstStringInContext(c->llvm_context, iprint_str, strlen(iprint_str), false));

        const char  uprint_str[] = "%zu\n";
        LLVMTypeRef uprint_type = LLVMArrayType(LLVMInt8TypeInContext(c->llvm_context), len(uprint_str));

        c->llvm_uprint_str = LLVMAddGlobal(c->llvm_module, uprint_type, "");
        LLVMSetInitializer(
            c->llvm_uprint_str, LLVMConstStringInContext(c->llvm_context, uprint_str, strlen(uprint_str), false));

        LLVMTypeRef printf_args[] = {
            LLVMPointerTypeInContext(c->llvm_context, 0),
        };

        c->llvm_printf_type =
            LLVMFunctionType(LLVMInt32TypeInContext(c->llvm_context), printf_args, len(printf_args), true);
        c->llvm_printf_func = LLVMAddFunction(c->llvm_module, "printf", c->llvm_printf_type);
    }

    for (size_t i = 0; i < c->globals.count; i++) {
        AST_Node_Atom *it = c->globals.data[i];
        if (it->llvm) {
            continue;
        }

        if (it->is_const) {
            if (it->const_value.kind == CONST_VALUE_FN) {
                compile_fn(c, it->const_value.as.fn);
            }
        } else {
            compile_var_def(c, it);
        }
    }

    LLVMDIBuilderFinalize(c->llvm_debug_builder);

    LLVMTypeRef  main_type = LLVMFunctionType(LLVMInt32TypeInContext(c->llvm_context), NULL, 0, 0);
    LLVMValueRef main_func = LLVMAddFunction(c->llvm_module, "main", main_type);
    LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, main_func, ""));

    AST_Node_Fn *fn = get_main(c);
    LLVMBuildCall2(c->llvm_builder, fn->node.type.llvm, fn->llvm, NULL, 0, "");
    LLVMBuildRet(c->llvm_builder, LLVMConstNull(LLVMInt32TypeInContext(c->llvm_context)));

    const char *object_path = temp_replace_suffix(output_path, EXE_FILE_EXTENSION, OBJ_FILE_EXTENSION);
    temp_paths_push(object_path);
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
    temp_reset(object_path);
}
