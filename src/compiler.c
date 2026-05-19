#include "compiler.h"
#include "basic.h"
#include "dwarf.h"
#include "node.h"
#include "token.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "thirdparty/stb_ds.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/TargetMachine.h>

static_assert(COUNT_TYPES == 17, "");
static void compile_type(Compiler *c, Type *type) {
    if (!type || type->llvm) {
        return;
    }

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

    case TYPE_STRUCT: {
        assert(type->spec.structt);

        Type_Struct *spec = type->spec.structt;
        if (!spec->llvm) {

            LLVMTypeRef *fields = temp_alloc(spec->fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = (Type_Struct_Field *) &spec->fields[i];
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

static_assert(COUNT_TYPES == 17, "");
static bool type_is_compound(Type type) {
    if (type.ref) {
        return false;
    }

    switch (type.kind) {
    case TYPE_STRUCT:
    case TYPE_SLICE:
    case TYPE_STRING:
        return true;

    default:
        return false;
    }
}

static ABI_Info get_abi_info_for_type(Compiler *c, Type *type) {
    ABI_Info     info = {0};
    const size_t size = compile_sizeof(c, type);

    if (type->ref) {
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;
    }

    static_assert(COUNT_TYPES == 17, "");
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

static LLVMTypeRef compile_fn_type(Compiler *c, Type type, ABI *abi) {
    const void *checkpoint = temp_alloc(0);

    assert(!type.ref && type.kind == TYPE_FN);
    Type_Fn spec = type.spec.fn;

    compile_type(c, spec.returnn);
    abi->returnn = get_abi_info_for_type(c, spec.returnn);

    abi->actual_args_count = 0;
    if (!abi->returnn.direct_types_count) {
        abi->actual_args_count++;
    }

    for (size_t i = 0; i < spec.args_count; i++) {
        ABI_Info *it = &abi->args[i];
        *it = get_abi_info_for_type(c, &spec.args[i].type);

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

static LLVMValueRef compile_fn(Compiler *c, Node_Fn *fn);
static LLVMValueRef compile_expr(Compiler *c, Node *n, bool ref);
static void         compile_stmt(Compiler *c, Node *n);

static const char *temp_emit_nested_fn_name(Compiler *c, Node_Fn *fn) {
    if (!fn) {
        return temp_sprintf("main"); // TODO(@package)
    }

    const char *name = temp_emit_nested_fn_name(c, fn->outer_fn);
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
    ptrdiff_t index = shgeti(c->llvm_debug_files, path);
    if (index != -1) {
        return c->llvm_debug_files[index].value;
    }

    LLVMMetadataRef metadata = LLVMDIBuilderCreateFile(c->llvm_debug_builder, path, strlen(path), ".", strlen("."));
    shput(c->llvm_debug_files, path, metadata);
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
            LLVMDIBuilderCreatePointerType(c->llvm_debug_builder, get_debug_for_type(c, &it.type), 64, 64, 0, "", 0));

        size_bits += 64;
    }

    LLVMMetadataRef metadata = LLVMDIBuilderCreateStructType(
        c->llvm_debug_builder,
        c->llvm_debug_compile_unit,
        name.data,
        name.count,
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

    temp_reset(checkpoint);
    return metadata;
}

static_assert(COUNT_TYPES == 17, "");
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
        args[0] = get_debug_for_type(c, spec.returnn);
        for (size_t i = 0; i < spec.args_count; i++) {
            args[i + 1] = get_debug_for_type(c, &spec.args[i].type);
        }

        LLVMMetadataRef fn_debug_type =
            LLVMDIBuilderCreateSubroutineType(c->llvm_debug_builder, NULL, args, spec.args_count + 1, 0);

        temp_reset(args);
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, fn_debug_type, sizeof(void *), sizeof(void *), 0, "", 0);
    }

    case TYPE_STRUCT: {
        compile_type(c, type);

        Type_Struct *spec = type->spec.structt;
        if (!spec->debug) {
            const void *checkpoint = temp_alloc(0);

            SV name = {0};
            {
                const char *namespace = temp_emit_nested_fn_name(c, spec->definition->defined_in);
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

            LLVMMetadataRef real = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                get_scope_of_definition(c, (Node *) spec->definition, spec->definition->defined_in),
                name.data,
                name.count,
                get_debug_file(c, spec->definition->node.token.pos.path),
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

static LLVMValueRef compile_string(Compiler *c, SV sv, const Pos *pos, bool ref) {
    LLVMValueRef memory = LLVMConstStringInContext(c->llvm_context, sv.data, sv.count, false);
    LLVMValueRef slice_data = LLVMAddGlobal(c->llvm_module, LLVMTypeOf(memory), "");
    LLVMSetInitializer(slice_data, memory);

    LLVMValueRef slice_count = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), sv.count, false);
    LLVMValueRef slice_struct = compile_alloca(c, c->llvm_slice_type);
    LLVMBuildStore(c->llvm_builder, slice_data, slice_struct);
    LLVMBuildStore(
        c->llvm_builder, slice_count, LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, slice_struct, 1, ""));

    if (ref) {
        return slice_struct;
    }

    if (pos) {
        set_debug_pos(c, *pos);
    }
    return LLVMBuildLoad2(c->llvm_builder, c->llvm_slice_type, slice_struct, "");
}

static_assert(COUNT_CONST_VALUES == 5, "");
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

    SV name = it->node.token.sv;
    if (it->definition_spec->is_extern) {
        // Guarantee a terminating '\0'
        name = sv_from_cstr(temp_sv_to_cstr(name));
    } else if (!it->definition_spec->is_local) {
        // TODO(@package)
        name = sv_from_cstr(temp_sprintf("main." SV_Fmt, SV_Arg(name)));
    }

    if (it->definition_spec->is_local && !it->definition_spec->is_extern) {
        it->definition_spec->llvm = compile_alloca(c, it->node.type.llvm);
    } else {
        it->definition_spec->llvm = LLVMAddGlobal(c->llvm_module, it->node.type.llvm, name.data);
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
                name.data,
                name.count,
                get_debug_file(c, it->node.token.pos.path),
                it->node.token.pos.row + 1,
                var_debug_type,
                false, // TODO: Gather more information on what even is this...
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

    if (fn->is_extern) {
        assert(fn->defined_as);
        fn->llvm = LLVMGetOrInsertFunction(
            c->llvm_module,
            fn->defined_as->node.token.sv.data,
            fn->defined_as->node.token.sv.count,
            fn->node.type.llvm);
    } else {
        const size_t defers_start_save = c->defers_start;
        c->defers_start = c->defers.count;

        LLVMValueRef llvm_fn_save = c->llvm_fn;
        LLVMValueRef llvm_fn_last_alloca_save = c->llvm_fn_last_alloca;

        LLVMMetadataRef   llvm_debug_scope_save = c->llvm_debug_scope;
        LLVMBasicBlockRef llvm_current_block_save = LLVMGetInsertBlock(c->llvm_builder);

        SV fn_name = sv_from_cstr(temp_emit_nested_fn_name(c, fn));
        fn->llvm = LLVMAddFunction(c->llvm_module, fn_name.data, fn->node.type.llvm);

        c->llvm_fn = fn->llvm;
        c->llvm_fn_last_alloca = NULL;

        LLVMMetadataRef fn_debug_type = NULL;
        const Type_Fn   fn_type_spec = fn->node.type.spec.fn;

        {

            LLVMMetadataRef *arg_debug_types = temp_alloc((fn_type_spec.args_count + 1) * sizeof(*arg_debug_types));
            arg_debug_types[0] = get_debug_for_type(c, fn_type_spec.returnn);

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
            fn_name.data,
            fn_name.count,
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
        if (!abi.returnn.direct_types_count) {
            arg_iota++;
            LLVMAttributeRef sret =
                LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_sret, fn_type_spec.returnn->llvm);
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

        if (fn_type_spec.returnn->kind == TYPE_UNIT) {
            compile_defers(c, c->defers_start, true);
            set_debug_pos(c, block->end);
            LLVMBuildRetVoid(c->llvm_builder);
        } else {
            // The semantic analyzer has already determined that the function returns in all execution paths
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

#ifdef PLATFORM_X86_64_WINDOWS
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif // PLATFORM_X86_64_WINDOWS

// NOTE: Only stdout and stderr are supported
static LLVMValueRef compile_get_stdio_file(Compiler *c, int fileno) {
#ifdef PLATFORM_X86_64_WINDOWS
    LLVMValueRef iob_args[] = {
        LLVMConstInt(LLVMInt32TypeInContext(c->llvm_context), fileno, 0),
    };
    return LLVMBuildCall2(c->llvm_builder, c->llvm_iob_type, c->llvm_iob_func, iob_args, len(iob_args), "");
#else
    LLVMValueRef var = NULL;
    switch (fileno) {
    case STDOUT_FILENO:
        var = c->llvm_stdout_value;
        break;

    case STDERR_FILENO:
        var = c->llvm_stderr_value;
        break;

    default:
        unreachable();
    }
    return LLVMBuildLoad2(c->llvm_builder, LLVMPointerTypeInContext(c->llvm_context, 0), var, "");
#endif // PLATFORM_X86_64_WINDOWS
}

static void compile_panic(Compiler *c, const char *fmt, ...) {
    LLVMValueRef llvm_stdout = compile_get_stdio_file(c, STDOUT_FILENO);
    LLVMValueRef llvm_stderr = compile_get_stdio_file(c, STDERR_FILENO);

    size_t  count = 0;
    va_list ap;
    va_start(ap, fmt);
    while (true) {
        LLVMValueRef it = va_arg(ap, LLVMValueRef);
        if (!it) {
            break;
        }
        count++;
    }
    va_end(ap);

    LLVMValueRef *args = temp_alloc(2 + count);
    size_t        iota = 0;

    args[iota++] = llvm_stderr;
    args[iota++] = LLVMBuildGlobalString(c->llvm_builder, fmt, "");

    va_start(ap, fmt);
    while (true < count) {
        LLVMValueRef it = va_arg(ap, LLVMValueRef);
        if (!it) {
            break;
        }
        args[iota++] = it;
    }
    va_end(ap);
    assert(iota == 2 + count);

    LLVMBuildCall2(c->llvm_builder, c->llvm_fprintf_type, c->llvm_fprintf_func, args, iota, "");

    LLVMBuildCall2(c->llvm_builder, c->llvm_fflush_type, c->llvm_fflush_func, &llvm_stdout, 1, "");
    LLVMBuildCall2(c->llvm_builder, c->llvm_fflush_type, c->llvm_fflush_func, &llvm_stderr, 1, "");

    LLVMBuildCall2(c->llvm_builder, c->llvm_abort_type, c->llvm_abort_func, NULL, 0, "");
    LLVMBuildUnreachable(c->llvm_builder);
    temp_reset(args);
}
#define compile_panic(c, fmt, ...)    compile_panic(c, fmt, __VA_ARGS__, NULL)
#define compile_panic_no_args(c, fmt) compile_panic(c, fmt, NULL)

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

static LLVMValueRef compile_string_eq(Compiler *c, LLVMValueRef lhs, LLVMValueRef rhs) {
    LLVMValueRef lhs_count = LLVMBuildExtractValue(c->llvm_builder, lhs, 1, "");
    LLVMValueRef rhs_count = LLVMBuildExtractValue(c->llvm_builder, rhs, 1, "");

    LLVMBasicBlockRef count_equal_block = LLVMGetInsertBlock(c->llvm_builder);
    LLVMBasicBlockRef data_equal_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
    LLVMBasicBlockRef end_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

    LLVMValueRef count_equal = LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, lhs_count, rhs_count, "");
    LLVMBuildCondBr(c->llvm_builder, count_equal, data_equal_block, end_block);

    // Count equal
    LLVMValueRef data_equal = NULL;
    {
        LLVMPositionBuilderAtEnd(c->llvm_builder, data_equal_block);

        LLVMValueRef memcmp_args[] = {
            LLVMBuildExtractValue(c->llvm_builder, lhs, 0, ""),
            LLVMBuildExtractValue(c->llvm_builder, rhs, 0, ""),
            lhs_count,
        };

        data_equal = LLVMBuildCall2(
            c->llvm_builder, c->llvm_memcmp_type, c->llvm_memcmp_func, memcmp_args, len(memcmp_args), "");
        data_equal = LLVMBuildICmp(
            c->llvm_builder, LLVMIntEQ, data_equal, LLVMConstNull(LLVMInt32TypeInContext(c->llvm_context)), "");
        LLVMBuildBr(c->llvm_builder, end_block);
    }

    LLVMPositionBuilderAtEnd(c->llvm_builder, end_block);
    LLVMTypeRef  llvm_type_i1 = LLVMInt1TypeInContext(c->llvm_context);
    LLVMValueRef string_equal = LLVMBuildPhi(c->llvm_builder, llvm_type_i1, "");

    LLVMValueRef      phi_values[] = {data_equal, LLVMConstNull(llvm_type_i1)};
    LLVMBasicBlockRef phi_blocks[] = {data_equal_block, count_equal_block};
    LLVMAddIncoming(string_equal, phi_values, phi_blocks, len(phi_blocks));
    return string_equal;
}

static_assert(COUNT_NODES == 23, "");
static LLVMValueRef compile_expr(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    compile_type(c, &n->type);
    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;

        static_assert(COUNT_TOKENS == 60, "");
        switch (n->token.kind) {
        case TOKEN_INT:
        case TOKEN_BOOL:
        case TOKEN_CHAR:
            return LLVMConstInt(n->type.llvm, n->token.as.integer, type_is_signed(n->type));

        case TOKEN_NULL:
            return LLVMConstNull(n->type.llvm);

        case TOKEN_IDENT: {
            Node_Atom *definition = (Node_Atom *) atom->definition;
            assert(definition);

            if (definition->definition_spec->is_const) {
                const Const_Value const_value = definition->definition_spec->const_value;

                static_assert(COUNT_CONST_VALUES == 5, "");
                switch (const_value.kind) {
                case CONST_VALUE_STRUCT:
                    if (!definition->definition_spec->llvm) {
                        const char *name = temp_sprintf("const.anon.%zu", c->iota_anonymous_const++);
                        definition->definition_spec->llvm = LLVMAddGlobal(c->llvm_module, n->type.llvm, name);
                        temp_reset(name);
                        LLVMSetInitializer(
                            definition->definition_spec->llvm, compile_const_value(c, const_value, n->type));
                    }

                    if (ref) {
                        return definition->definition_spec->llvm;
                    }

                    set_debug_pos(c, n->token.pos);
                    return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->definition_spec->llvm, "");

                case CONST_VALUE_STRING:
                    return compile_string(c, const_value.as.string, &n->token.pos, ref);

                default:
                    return compile_const_value(c, const_value, n->type);
                }
            }

            if (!definition->definition_spec->llvm) {
                compile_stmt(c, (Node *) definition->definition_spec->definition_node);
            }

            if (ref) {
                return definition->definition_spec->llvm;
            }

            set_debug_pos(c, n->token.pos);
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->definition_spec->llvm, "");
        }

        case TOKEN_STRING:
            return compile_string(c, n->token.sv, &n->token.pos, ref);

        default:
            unreachable();
        }
    }

    case NODE_GHOST: {
        Node_Ghost *ghost = (Node_Ghost *) n;
        LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);

        const Const_Value const_value = *ghost->arg->default_value;
        static_assert(COUNT_CONST_VALUES == 5, "");
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

        static_assert(COUNT_TOKENS == 60, "");
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
            LLVMValueRef lhs = compile_expr(c, binary->lhs, false);
            LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
            set_debug_pos(c, n->token.pos);

            LLVMValueRef equal = compile_string_eq(c, lhs, rhs);
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

            static_assert(COUNT_TOKENS == 60, "");
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

            static_assert(COUNT_TOKENS == 60, "");
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

            static_assert(COUNT_TOKENS == 60, "");
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
                LLVMValueRef ptr = compile_expr(c, binary->lhs, true);
                LLVMValueRef lhs = LLVMBuildLoad2(c->llvm_builder, binary->lhs->type.llvm, ptr, "");
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
                return LLVMBuildStore(c->llvm_builder, result, ptr);
            }
        }

        static_assert(COUNT_TOKENS == 60, "");
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

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;

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

    case NODE_FN:
        return compile_fn(c, (Node_Fn *) n);

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
        for (Node *arg = call->args.head; arg; arg = arg->next) {
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
        assert(call->fn->type.kind == TYPE_FN);
        const Type_Fn fn_type_spec = call->fn->type.spec.fn;

        for (size_t i = 0; i < abi.args_count; i++) {
            const ABI_Info it_abi = abi.args[i];
            if (it_abi.direct_types_count) {
                arg_iota += it_abi.direct_types_count;
            } else {
                arg_iota++;

                LLVMTypeRef it_type = fn_type_spec.args[i].type.llvm;
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

    case NODE_SLICE:
        unreachable();

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;

        const char *label = "slice";
        if (!index->lhs->type.ref) {
            static_assert(COUNT_TYPES == 17, "");
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
                        Pos_Fmt "Range (%%lld..%%lld) is invalid: Beginning of range is more than end\n",
                        Pos_Arg(n->token.pos));

                    compile_panic(c, message, a, b);
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
                            Pos_Fmt "Range (%%lld..%%lld) is out of bounds in %s of length %%lld\n",
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
                    Pos_Fmt "Index %%lld is out of bounds in %s of length %%lld\n", Pos_Arg(n->token.pos), label);

                compile_panic(c, message, a, count);
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

static_assert(COUNT_NODES == 23, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        if (assertt->is_compile_time) {
            return;
        }

        LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMValueRef      check = compile_expr(c, assertt->expr, false);

        set_debug_pos(c, n->token.pos);
        LLVMBuildCondBr(c->llvm_builder, check, success, failure);

        // Failure
        LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
        {
            const char *message = NULL;
            if (assertt->message) {
                message = temp_sprintf(
                    Pos_Fmt "Assertion failed: " SV_Fmt "\n", Pos_Arg(n->token.pos), SV_Arg(assertt->message_sv));
            } else {
                message = temp_sprintf(Pos_Fmt "Assertion failed\n", Pos_Arg(n->token.pos));
            }

            compile_panic_no_args(c, message);
            temp_reset(message);
        }

        // Success
        LLVMPositionBuilderAtEnd(c->llvm_builder, success);
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        if (define->is_const) {
            return;
        }

        assert(define->name->kind == NODE_ATOM && define->name->token.kind == TOKEN_IDENT);
        Node_Atom *it = (Node_Atom *) define->name;
        Node      *it_expr = define->expr;

        if (!it->definition_spec->llvm) {
            compile_var_def(c, it);
            if (it_expr && it->definition_spec->is_local) {
                set_debug_pos(c, n->token.pos);
                LLVMBuildStore(c->llvm_builder, compile_expr(c, it_expr, false), it->definition_spec->llvm);
            }
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

        LLVMValueRef value = NULL;
        if (type_is_compound(n->type)) {
            ABI_Info abi = get_abi_info_for_type(c, &n->type);

            static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
            switch (abi.direct_types_count) {
            case 0:
                value = compile_expr(c, returnn->value, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildStore(c->llvm_builder, value, LLVMGetParam(c->llvm_fn, 0));
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRetVoid(c->llvm_builder);
                break;

            case 1:
                value = compile_expr(c, returnn->value, true);
                value = LLVMBuildLoad2(c->llvm_builder, abi.direct_types[0], value, "");
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRet(c->llvm_builder, value);
                break;

            case 2: {
                LLVMTypeRef type =
                    LLVMStructTypeInContext(c->llvm_context, abi.direct_types, abi.direct_types_count, false);
                value = compile_expr(c, returnn->value, true);
                value = LLVMBuildLoad2(c->llvm_builder, type, value, "");
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRet(c->llvm_builder, value);
            } break;

            default:
                unreachable();
            }
        } else {
            value = compile_expr(c, returnn->value, false);
            set_debug_pos(c, n->token.pos);

            compile_defers(c, c->defers_start, false);
            set_debug_pos(c, n->token.pos);
            if (n->type.kind == TYPE_UNIT) {
                LLVMBuildRetVoid(c->llvm_builder);
            } else {
                LLVMBuildRet(c->llvm_builder, value);
            }
        }

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

        const bool   is_signed = type_is_signed(print->value->type);
        LLVMValueRef args[] = {
            is_signed ? c->llvm_iprint_str : c->llvm_uprint_str,
            compile_cast(c, compile_expr(c, print->value, false), LLVMInt64TypeInContext(c->llvm_context), is_signed),
        };

        set_debug_pos(c, n->token.pos);
        LLVMBuildCall2(c->llvm_builder, c->llvm_printf_type, c->llvm_printf_func, args, len(args), "");
    } break;

    default:
        compile_expr(c, n, false);
        break;
    }
}

static Node_Fn *get_main(Compiler *c) {
    Node_Atom *main = scope_find(c->globals, sv_from_cstr("main"));
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

    if (!main->definition_spec->is_const || main->definition_spec->const_value.kind != CONST_VALUE_FN) {
        fprintf(
            stderr, Pos_Fmt "ERROR: Identifier 'main' must be a constant function\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }

    const Type_Fn signature = main->node.type.spec.fn;
    if (signature.args_count) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot take any arguments\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }

    if (signature.returnn->kind != TYPE_UNIT) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot return anything\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }
    return main->definition_spec->const_value.as.fn;
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
    Node_Fn *main_fn = get_main(c);

    if (!c->llvm_context) {
        compiler_init_llvm_target_data(c);
    }
    c->llvm_builder = LLVMCreateBuilderInContext(c->llvm_context);

    c->llvm_attribute_sret = LLVMGetEnumAttributeKindForName("sret", strlen("sret"));
    c->llvm_attribute_byval = LLVMGetEnumAttributeKindForName("byval", strlen("byval"));

    c->llvm_debug_builder = LLVMCreateDIBuilder(c->llvm_module);

    c->llvm_debug_compile_unit = LLVMDIBuilderCreateCompileUnit(
        c->llvm_debug_builder,
        LLVMDWARFSourceLanguageC,
        get_debug_file(c, main_fn->node.token.pos.path),
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
        const char  iprint_str[] = "%lld\n";
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

    // Panics
    {
        LLVMTypeRef llvm_ptr_type = LLVMPointerTypeInContext(c->llvm_context, 0);

        LLVMTypeRef fprintf_args[] = {
            llvm_ptr_type,
            llvm_ptr_type,
        };

        c->llvm_fprintf_type =
            LLVMFunctionType(LLVMInt32TypeInContext(c->llvm_context), fprintf_args, len(fprintf_args), true);
        c->llvm_fprintf_func = LLVMAddFunction(c->llvm_module, "fprintf", c->llvm_fprintf_type);

        LLVMTypeRef fflush_args[] = {
            llvm_ptr_type,
        };

        c->llvm_fflush_type =
            LLVMFunctionType(LLVMInt32TypeInContext(c->llvm_context), fflush_args, len(fflush_args), false);
        c->llvm_fflush_func = LLVMAddFunction(c->llvm_module, "fflush", c->llvm_fflush_type);

        c->llvm_abort_type = LLVMFunctionType(LLVMVoidTypeInContext(c->llvm_context), NULL, 0, false);
        c->llvm_abort_func = LLVMAddFunction(c->llvm_module, "abort", c->llvm_abort_type);

#ifdef PLATFORM_X86_64_WINDOWS
        LLVMTypeRef iob_args[] = {
            LLVMInt32TypeInContext(c->llvm_context),
        };

        c->llvm_iob_type = LLVMFunctionType(llvm_ptr_type, iob_args, len(iob_args), false);
        c->llvm_iob_func = LLVMAddFunction(c->llvm_module, "__acrt_iob_func", c->llvm_iob_type);
#endif // PLATFORM_X86_64_WINDOWS

#ifdef PLATFORM_X86_64_LINUX
        c->llvm_stdout_value = LLVMAddGlobal(c->llvm_module, llvm_ptr_type, "stdout");
        c->llvm_stderr_value = LLVMAddGlobal(c->llvm_module, llvm_ptr_type, "stderr");
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
        c->llvm_stdout_value = LLVMAddGlobal(c->llvm_module, llvm_ptr_type, "__stdoutp");
        c->llvm_stderr_value = LLVMAddGlobal(c->llvm_module, llvm_ptr_type, "__stderrp");
#endif // PLATFORM_ARM64_MACOS
    }

    // String comparisons
    {
        LLVMTypeRef memcmp_args[] = {
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMInt64TypeInContext(c->llvm_context),
        };

        c->llvm_memcmp_type =
            LLVMFunctionType(LLVMInt32TypeInContext(c->llvm_context), memcmp_args, len(memcmp_args), false);
        c->llvm_memcmp_func = LLVMAddFunction(c->llvm_module, "memcmp", c->llvm_memcmp_type);
    }

    for (size_t i = 0; i < c->globals.count; i++) {
        Node_Atom *it = c->globals.data[i];
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

    LLVMDIBuilderFinalize(c->llvm_debug_builder);

    LLVMTypeRef  main_type = LLVMFunctionType(LLVMInt32TypeInContext(c->llvm_context), NULL, 0, 0);
    LLVMValueRef main_func = LLVMAddFunction(c->llvm_module, "main", main_type);
    LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, main_func, ""));

    LLVMBuildCall2(c->llvm_builder, main_fn->node.type.llvm, main_fn->llvm, NULL, 0, "");
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

    shfree(c->llvm_debug_files);
    temp_reset(checkpoint);
}

// TODO: Should we use '%llu' instead of '%zu' for symmetry?
