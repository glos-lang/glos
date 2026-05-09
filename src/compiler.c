#include "compiler.h"
#include "ast.h"
#include "basic.h"
#include "dwarf.h"
#include "token.h"
#include "llvm-c/Core.h"
#include "llvm-c/DebugInfo.h"

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

            LLVMTypeRef *fields = arena_alloc(c->arena, spec.fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec.fields_count; i++) {
                AST_Node *it = (AST_Node *) spec.fields[i];
                compile_type(c, &it->type);
                fields[i] = it->type.llvm;
            }

            common->llvm = LLVMStructTypeInContext(c->llvm_context, fields, spec.fields_count, false);
        }
        type->llvm = common->llvm;
    } break;

    default:
        unreachable();
        break;
    }
}

static void compile_fn_type(Compiler *c, AST_Type *type) {
    const AST_Type_Fn spec = type->spec.fn;
    compile_type(c, spec.returnn);

    LLVMTypeRef *args = temp_alloc(spec.args_count * sizeof(*args));
    for (size_t i = 0; i < spec.args_count; i++) {
        AST_Type *it = &spec.args[i]->node.type;
        compile_type(c, it);

        LLVMTypeRef type = it->llvm;
        if (!it->ref && it->kind == AST_TYPE_STRUCT) {
            const size_t size = compile_sizeof(c, it);
            if (size <= 8) {
                type = LLVMIntTypeInContext(c->llvm_context, size * 8);
            }
        }

        args[i] = type;
    }

    LLVMTypeRef return_type = spec.returnn->llvm;
    if (!spec.returnn->ref && spec.returnn->kind == AST_TYPE_STRUCT) {
        const size_t size = compile_sizeof(c, spec.returnn);
        if (size <= 8) {
            return_type = LLVMIntTypeInContext(c->llvm_context, size * 8);
        }
    }

    type->llvm = LLVMFunctionType(return_type, args, spec.args_count, false);
    temp_reset(args);
}

static LLVMValueRef compile_fn(Compiler *c, AST_Node_Fn *fn);
static LLVMValueRef compile_expr(Compiler *c, AST_Node *n, bool ref);
static void         compile_stmt(Compiler *c, AST_Node *n);

static const char *temp_emit_fn_name(Compiler *c, AST_Node_Fn *fn) {
    const char *name = NULL;
    if (fn->outer_fn) {
        name = temp_emit_fn_name(c, fn->outer_fn);
    } else {
        name = temp_sprintf("main");
    }

    temp_remove_null();
    if (fn->defined_as) {
        temp_sprintf("." SV_Fmt, SV_Arg(fn->defined_as->node.token.sv));
    } else {
        temp_sprintf(".anon.%zu", c->iota_anonymous_fn++);
    }
    return name;
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
            LLVMMetadataRef *fields = arena_alloc(c->arena, spec->fields_count * sizeof(*fields));

            size_t offset_bits = 0;
            for (size_t i = 0; i < spec->fields_count; i++) {
                AST_Node *it = (AST_Node *) spec->fields[i];

                const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm) * 8;

                if (offset_bits % align_bits != 0) {
                    offset_bits += align_bits - (offset_bits % align_bits);
                }

                fields[i] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    c->llvm_debug_scope, // TODO: Scope
                    it->token.sv.data,
                    it->token.sv.count,
                    c->llvm_debug_file, // TODO: May not be the current file
                    it->token.pos.row + 1,
                    size_bits,
                    align_bits,
                    offset_bits,
                    0,
                    get_debug_for_type(c, it->type));

                offset_bits += size_bits;
            }

            const void *checkpoint = temp_alloc(0);

            SV             name = {0};
            AST_Node_Atom *definition = spec->definition->defined_as;
            if (definition) {
                name = definition->node.token.sv;
            } else {
                name = sv_from_cstr(temp_sprintf("anon.%zu", c->iota_anonymous_struct++));
            }

            spec->debug = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                c->llvm_debug_file, // TODO: Scope
                name.data,
                name.count,
                c->llvm_debug_file, // TODO: May not be the current file
                definition->node.token.pos.row + 1,
                LLVMABISizeOfType(c->llvm_target_data, common->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, common->llvm) * 8,
                0,
                NULL, // TODO: Derived from
                fields,
                spec->fields_count,
                0,
                NULL,
                "", // TODO: UID
                0);

            temp_reset(checkpoint);
        }

        return spec->debug;
    }

    default:
        unreachable();
        break;
    }
}

static void set_debug_location(Compiler *c, Pos pos) {
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

static void compile_var_def(Compiler *c, AST_Node_Atom *it) {
    const void *checkpoint = temp_alloc(0);

    compile_type(c, &it->node.type);

    SV name = {0};
    if (!it->is_local && !it->is_extern) {
        name = sv_from_cstr(temp_sprintf("main." SV_Fmt, SV_Arg(it->node.token.sv)));
    } else {
        // Guarantee a terminating '\0'
        name = sv_from_cstr(temp_sv_to_cstr(it->node.token.sv));
    }

    if (it->is_local && !it->is_extern) {
        it->llvm = compile_alloca(c, it->node.type.llvm);
    } else {
        it->llvm = LLVMAddGlobal(c->llvm_module, it->node.type.llvm, name.data);
    }

    if (!it->is_extern) {
        LLVMMetadataRef var_debug_type = get_debug_for_type(c, it->node.type);
        if (it->is_local) {
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

                if (!it->is_assigned) {
                    LLVMBuildStore(c->llvm_builder, LLVMConstNull(it->node.type.llvm), it->llvm);
                }
            }

            LLVMMetadataRef var_pos_metadata = LLVMDIBuilderCreateDebugLocation(
                c->llvm_context, it->node.token.pos.row + 1, it->node.token.pos.col + 1, c->llvm_debug_scope, NULL);

            {
                LLVMValueRef next_inst = LLVMGetNextInstruction(c->llvm_fn_last_alloca);
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
        } else {
            if (it->is_assigned) {
                LLVMSetInitializer(it->llvm, compile_const_value(c, it->const_value, it->node.type));
            } else {
                LLVMSetInitializer(it->llvm, LLVMConstNull(it->node.type.llvm));
            }

            LLVMMetadataRef var_debug_metadata = LLVMDIBuilderCreateGlobalVariableExpression(
                c->llvm_debug_builder,
                c->llvm_debug_compile_unit,
                name.data,
                name.count,
                name.data,
                name.count,
                c->llvm_debug_file,
                it->node.token.pos.row + 1,
                var_debug_type,
                false, // TODO(@libllvm): Local variables
                NULL,
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

    compile_fn_type(c, &fn->node.type);

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

        c->llvm_debug_scope = LLVMDIBuilderCreateFunction(
            c->llvm_debug_builder,
            c->llvm_debug_scope ? c->llvm_debug_scope : c->llvm_debug_file,
            fn_name.data,
            fn_name.count,
            fn_name.data,
            fn_name.count,
            c->llvm_debug_file,
            fn->node.token.pos.row + 1,
            fn_debug_type,
            true,
            true,
            fn->node.token.pos.row + 1, // TODO(@libllvm): scope line
            0,
            false);
        LLVMSetSubprogram(fn->llvm, c->llvm_debug_scope);

        LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, fn->llvm, ""));
        LLVMSetCurrentDebugLocation(c->llvm_builder, NULL);

        size_t arg_iota = 0;
        for (AST_Node *arg = fn->args.head; arg; arg = arg->next) {
            assert(arg->kind == AST_NODE_DEFINE);
            AST_Node_Define *define = (AST_Node_Define *) arg;

            assert(define->name->kind == AST_NODE_ATOM);
            AST_Node_Atom *it = (AST_Node_Atom *) define->name;

            assert(!it->llvm);
            compile_var_def(c, it);
            LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), it->llvm);
        }

        assert(fn->body->kind == AST_NODE_BLOCK);
        AST_Node_Block *block = (AST_Node_Block *) fn->body;
        for (AST_Node *it = block->body.head; it; it = it->next) {
            compile_stmt(c, it);
        }

        set_debug_location(c, block->end);
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

            set_debug_location(c, n->token.pos);
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
            set_debug_location(c, n->token.pos);
            return LLVMBuildNeg(c->llvm_builder, value, "");

        case TOKEN_MUL:
            value = compile_expr(c, unary->value, false);
            if (ref) {
                return value;
            }

            set_debug_location(c, n->token.pos);
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, value, "");

        case TOKEN_BAND:
            return compile_expr(c, unary->value, true);

        case TOKEN_BNOT:
            value = compile_expr(c, unary->value, false);
            set_debug_location(c, n->token.pos);
            return LLVMBuildNot(c->llvm_builder, value, "");

        case TOKEN_LNOT:
            value = compile_expr(c, unary->value, false);
            set_debug_location(c, n->token.pos);
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

                set_debug_location(c, n->token.pos);
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

                set_debug_location(c, n->token.pos);
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
            set_debug_location(c, n->token.pos);
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
            set_debug_location(c, n->token.pos);

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
            set_debug_location(c, n->token.pos);
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
        size_t       ordered_iota = 0;
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
                set_debug_location(c, n->token.pos);

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
                set_debug_location(c, n->token.pos);
                return LLVMBuildICmp(c->llvm_builder, LLVMIntNE, from, LLVMConstNull(n->type.llvm), "");

            default:
                unreachable();
            }
        }

        LLVMValueRef fn = compile_expr(c, call->fn, false);

        LLVMTypeRef  *arg_types = temp_alloc(call->args_count * sizeof(*arg_types));
        LLVMValueRef *arg_values = temp_alloc(call->args_count * sizeof(*arg_values));

        size_t iota = 0;
        for (AST_Node *arg = call->args.head; arg; arg = arg->next) {
            LLVMValueRef expr = NULL;
            LLVMTypeRef  type = NULL;
            if (!arg->type.ref && arg->type.kind == AST_TYPE_STRUCT) {
                const size_t size = compile_sizeof(c, &arg->type);
                if (size <= 8) {
                    expr = compile_expr(c, arg, true);
                    type = LLVMIntTypeInContext(c->llvm_context, size * 8);
                    expr = LLVMBuildLoad2(c->llvm_builder, type, expr, "");
                }
            }

            if (!expr) {
                expr = compile_expr(c, arg, false);
                type = LLVMTypeOf(expr);
            }

#ifdef PLATFORM_ARM64_MACOS
            // TODO: ABI
            // if (arg->type.kind == AST_TYPE_STRUCT) {
            //     LLVM_Type_Info info = llvm_type_info(arg->type.llvm);
            //     if (info.size > 16) {
            //         LLVM_Node *temp =
            //             (LLVM_Node *) llvm_var_new(&c->llvm, (SV) {0}, arg->type.llvm, true, false, false);
            //         llvm_build_store(&c->llvm, temp, expr);
            //         expr = temp;
            //     }
            // }
#endif // PLATFORM_ARM64_MACOS

            arg_types[iota] = type;
            arg_values[iota] = expr;
            iota++;
        }
        assert(iota == call->args_count);

        LLVMTypeRef return_type = n->type.llvm;
        if (!n->type.ref && n->type.kind == AST_TYPE_STRUCT) {
            const size_t size = compile_sizeof(c, &n->type);
            if (size <= 8) {
                return_type = LLVMIntTypeInContext(c->llvm_context, size * 8);
            }
        }

        set_debug_location(c, n->token.pos);
        LLVMValueRef result = LLVMBuildCall2(
            c->llvm_builder,
            LLVMFunctionType(return_type, arg_types, call->args_count, false),
            fn,
            arg_values,
            call->args_count,
            "");

        temp_reset(arg_types);

        if (!n->type.ref && n->type.kind == AST_TYPE_STRUCT) {
            LLVMValueRef alloca = compile_alloca(c, n->type.llvm);
            LLVMBuildStore(c->llvm_builder, result, alloca);
            if (ref) {
                return alloca;
            }
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, alloca, "");
        }

        return result;
    }

    default:
        unreachable();
        break;
    }
}

//     bool       debug = true;
//     LLVM_Node *result = NULL;

//     compile_type(c, &n->type);
//     switch (n->kind) {
//     case AST_NODE_ATOM: {
//         AST_Node_Atom *atom = (AST_Node_Atom *) n;
//         static_assert(COUNT_TOKENS == 41, "");
//         switch (n->token.kind) {
//         case TOKEN_BOOL:
//         case TOKEN_INT:
//             debug = false;
//             return_defer(llvm_atom_int(&c->llvm, n->type.llvm, n->token.as.integer));

//         case TOKEN_IDENT: {
//             AST_Node_Atom *definition = (AST_Node_Atom *) atom->definition;
//             assert(definition);

//             if (definition->is_const) {
//                 debug = false;

//                 static_assert(COUNT_CONST_VALUES == 4, "");
//                 switch (definition->const_value.kind) {
//                 case CONST_VALUE_INT:
//                     return_defer(llvm_atom_int(&c->llvm, n->type.llvm, definition->const_value.as.integer));

//                 case CONST_VALUE_FN:
//                     return_defer(compile_fn(c, definition->const_value.as.fn));

//                 case CONST_VALUE_TYPE:
//                     unreachable();

//                 case CONST_VALUE_STRUCT: {
//                     // TODO: Don't generate this over and over
//                     LLVM_Node_Var_Init *value =
//                         compile_const_value_to_var_init(c, n->type.llvm, definition->const_value);

//                     const char *name = temp_sprintf("const.anon.%zu", c->iota_anonymous_const++);

//                     LLVM_Node *memory = llvm_const_new(&c->llvm, sv_from_cstr(name), n->type.llvm, value);
//                     if (ref) {
//                         return_defer(memory);
//                     }
//                     return_defer(llvm_build_load(&c->llvm, memory, n->type.llvm));
//                 }

//                 default:
//                     unreachable();
//                 }
//             }

//             if (!definition->llvm) {
//                 compile_stmt(c, (AST_Node *) definition->definition_node);
//             }

//             if (ref) {
//                 debug = false;
//                 return_defer(definition->llvm);
//             }

//             return_defer(llvm_build_load(&c->llvm, definition->llvm, n->type.llvm));
//         }

//         default:
//             unreachable();
//         }
//     } break;

//     case AST_NODE_UNARY: {
//         AST_Node_Unary *unary = (AST_Node_Unary *) n;
//         LLVM_Node      *value = NULL;

//         static_assert(COUNT_TOKENS == 41, "");
//         switch (n->token.kind) {
//         case TOKEN_SUB:
//             value = compile_expr(c, unary->value, false);
//             return_defer(llvm_build_unary(&c->llvm, LLVM_UNARY_NEG, n->type.llvm, value));

//         case TOKEN_MUL:
//             value = compile_expr(c, unary->value, false);
//             if (ref) {
//                 debug = false;
//                 return_defer(value);
//             }
//             return_defer(llvm_build_load(&c->llvm, value, n->type.llvm));

//         case TOKEN_BAND:
//             debug = false;
//             return_defer(compile_expr(c, unary->value, true));

//         case TOKEN_BNOT:
//             value = compile_expr(c, unary->value, false);
//             return_defer(llvm_build_unary(&c->llvm, LLVM_UNARY_BNOT, n->type.llvm, value));

//         case TOKEN_LNOT:
//             value = compile_expr(c, unary->value, false);
//             return_defer(llvm_build_unary(&c->llvm, LLVM_UNARY_LNOT, n->type.llvm, value));

//         case TOKEN_SIZEOF:
//             debug = false;
//             return_defer(llvm_atom_int(&c->llvm, n->type.llvm, compile_sizeof(c, &unary->value->type)));

//         default:
//             unreachable();
//         }
//     } break;

//     case AST_NODE_BINARY: {
//         AST_Node_Binary *binary = (AST_Node_Binary *) n;

//         static_assert(COUNT_TOKENS == 41, "");
//         static const LLVM_Binary_Kind ops[COUNT_TOKENS] = {
//             [TOKEN_ADD] = LLVM_BINARY_ADD,
//             [TOKEN_SUB] = LLVM_BINARY_SUB,
//             [TOKEN_MUL] = LLVM_BINARY_MUL,
//             [TOKEN_DIV] = LLVM_BINARY_DIV,
//             [TOKEN_MOD] = LLVM_BINARY_MOD,

//             [TOKEN_SHL] = LLVM_BINARY_SHL,
//             [TOKEN_SHR] = LLVM_BINARY_SHR,
//             [TOKEN_BOR] = LLVM_BINARY_BOR,
//             [TOKEN_BAND] = LLVM_BINARY_BAND,

//             [TOKEN_GT] = LLVM_BINARY_GT,
//             [TOKEN_GE] = LLVM_BINARY_GE,
//             [TOKEN_LT] = LLVM_BINARY_LT,
//             [TOKEN_LE] = LLVM_BINARY_LE,
//             [TOKEN_EQ] = LLVM_BINARY_EQ,
//             [TOKEN_NE] = LLVM_BINARY_NE,
//         };

//         const LLVM_Binary_Kind op = ops[n->token.kind];
//         if (op) {
//             LLVM_Node *lhs = compile_expr(c, binary->lhs, false);
//             LLVM_Node *rhs = compile_expr(c, binary->rhs, false);
//             return_defer(llvm_build_binary(&c->llvm, op, n->type.llvm, lhs, rhs));
//         }

//         static_assert(COUNT_TOKENS == 41, "");
//         switch (n->token.kind) {
//         case TOKEN_SET: {
//             LLVM_Node *lhs = compile_expr(c, binary->lhs, true);
//             LLVM_Node *rhs = compile_expr(c, binary->rhs, false);
//             return_defer(llvm_build_store(&c->llvm, lhs, rhs));
//         }

//         default:
//             unreachable();
//         }
//     } break;

//     case AST_NODE_MEMBER: {
//         AST_Node_Member *member = (AST_Node_Member *) n;

//         LLVM_Node *lhs = NULL;
//         LLVM_Type  lhs_type = {0};
//         if (member->lhs->type.ref) {
//             lhs = compile_expr(c, member->lhs, false);
//             lhs_type = member->lhs->type.llvm;

//             assert(lhs_type.kind == LLVM_TYPE_PTR && lhs_type.ptr.type);
//             lhs_type = *lhs_type.ptr.type;

//             for (size_t i = 1; i < member->lhs->type.ref; i++) {
//                 lhs = llvm_build_load(&c->llvm, lhs, lhs_type);
//                 llvm_debug_set_pos(&c->llvm, lhs, n->token.pos.row, n->token.pos.col);

//                 assert(lhs_type.kind == LLVM_TYPE_PTR && lhs_type.ptr.type);
//                 lhs_type = *lhs_type.ptr.type;
//             }

//             assert(lhs_type.kind == LLVM_TYPE_STRUCT);
//         } else {
//             lhs = compile_expr(c, member->lhs, true);
//             lhs_type = member->lhs->type.llvm;
//         }

//         LLVM_Node *value = llvm_build_gep_field(&c->llvm, n->type.llvm, lhs, lhs_type, member->field_index);
//         if (ref) {
//             debug = false;
//             return_defer(value);
//         }
//         return_defer(llvm_build_load(&c->llvm, value, n->type.llvm));
//     }

//     case AST_NODE_FN:
//         return compile_fn(c, (AST_Node_Fn *) n);

//     case AST_NODE_STRUCT:
//         unreachable();

//     case AST_NODE_COMPOUND: {
//         AST_Node_Compound *compound = (AST_Node_Compound *) n;

//         LLVM_Node *memory = (LLVM_Node *) llvm_var_new(&c->llvm, (SV) {0}, n->type.llvm, true, true, false);

//         size_t ordered_iota = 0;
//         for (AST_Node *iter = compound->children.head; iter; iter = iter->next) {
//             size_t it_iota = 0;
//             if (!compound->is_designated) {
//                 it_iota = ordered_iota++;
//             }

//             AST_Node *it = iter;
//             if (n->type.kind == AST_TYPE_STRUCT) {
//                 if (compound->is_designated) {
//                     assert(it->kind == AST_NODE_BINARY && it->token.kind == TOKEN_SET);
//                     AST_Node_Binary *it_binary = (AST_Node_Binary *) it;
//                     it_iota = it->token.as.integer;
//                     it = it_binary->rhs;
//                 }

//                 LLVM_Node *field = llvm_build_gep_field(&c->llvm, n->type.llvm, memory, n->type.llvm, it_iota);
//                 LLVM_Node *value = compile_expr(c, it, false);
//                 llvm_build_store(&c->llvm, field, value);
//             } else {
//                 unreachable();
//             }
//         }

//         debug = false;
//         if (ref) {
//             return_defer(memory);
//         }
//         return_defer(llvm_build_load(&c->llvm, memory, n->type.llvm));
//     }

//     case AST_NODE_CALL: {
//         AST_Node_Call *call = (AST_Node_Call *) n;
//         if (call->is_type_cast) {
//             LLVM_Node *value = compile_expr(c, call->args.head, false);

//             static_assert(COUNT_TYPE_CASTS == 3, "");
//             switch (call->type_cast) {
//             case TYPE_CAST_NOP:
//                 debug = false;
//                 return_defer(value);

//             case TYPE_CAST_NORMAL:
//                 return_defer(llvm_build_cast(&c->llvm, value, n->type.llvm));

//             case TYPE_CAST_TO_BOOL:
//                 return_defer(llvm_build_binary(
//                     &c->llvm,
//                     LLVM_BINARY_NE,
//                     n->type.llvm,
//                     value,
//                     llvm_atom_int(&c->llvm, call->args.head->type.llvm, 0)));

//             default:
//                 unreachable();
//             }
//         }

//         LLVM_Node **args = arena_alloc(c->llvm.arena, call->args_count * sizeof(*args));

//         size_t iota = 0;
//         for (AST_Node *arg = call->args.head; arg; arg = arg->next) {
//             LLVM_Node *expr = compile_expr(c, arg, false);
// #ifdef PLATFORM_ARM64_MACOS
//             if (arg->type.kind == AST_TYPE_STRUCT) {
//                 LLVM_Type_Info info = llvm_type_info(arg->type.llvm);
//                 if (info.size > 16) {
//                     LLVM_Node *temp =
//                         (LLVM_Node *) llvm_var_new(&c->llvm, (SV) {0}, arg->type.llvm, true, false, false);
//                     llvm_build_store(&c->llvm, temp, expr);
//                     expr = temp;
//                 }
//             }
// #endif // PLATFORM_ARM64_MACOS
//             args[iota++] = expr;
//         }

//         assert(iota == call->args_count);
//         return_defer(llvm_build_call(&c->llvm, compile_expr(c, call->fn, false), args, iota, ref));
//     } break;

//     default:
//         unreachable();
//     }

// defer:
//     assert(result);
//     if (debug) {
//         llvm_debug_set_pos(&c->llvm, result, n->token.pos.row, n->token.pos.col);
//     }
//     return result;
// }

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
                set_debug_location(c, n->token.pos);
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
        set_debug_location(c, n->token.pos);
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

                set_debug_location(c, forr->condition->token.pos);
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
        set_debug_location(c, n->token.pos);
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
        if (!n->type.ref && n->type.kind == AST_TYPE_STRUCT) {
            const size_t size = compile_sizeof(c, &n->type);
            if (size <= 8) {
                LLVMTypeRef type = LLVMIntTypeInContext(c->llvm_context, size * 8);
                value = compile_expr(c, returnn->value, true);
                value = LLVMBuildLoad2(c->llvm_builder, type, value, "");
            }
        }

        if (!value) {
            value = compile_expr(c, returnn->value, false);
        }

        set_debug_location(c, n->token.pos);
        if (n->type.kind == AST_TYPE_UNIT) {
            LLVMBuildRetVoid(c->llvm_builder);
        } else {
            LLVMBuildRet(c->llvm_builder, value);
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

        set_debug_location(c, n->token.pos);
        LLVMBuildCall2(c->llvm_builder, c->llvm_printf_type, c->llvm_printf_func, args, len(args), "");
    } break;

    default:
        compile_expr(c, n, false);
        break;
    }
    // switch (n->kind) {
    // case AST_NODE_DEFINE: {
    //     AST_Node_Define *define = (AST_Node_Define *) n;
    //     if (define->is_const) {
    //         return;
    //     }

    //     assert(define->name->kind == AST_NODE_ATOM && define->name->token.kind == TOKEN_IDENT);
    //     AST_Node_Atom *it = (AST_Node_Atom *) define->name;
    //     AST_Node      *it_expr = define->expr;

    //     if (!it->llvm) {
    //         compile_var_def(c, it);
    //         if (it_expr) {
    //             if (it->is_local) {
    //                 llvm_debug_set_pos(
    //                     &c->llvm,
    //                     llvm_build_store(&c->llvm, it->llvm, compile_expr(c, it_expr, false)),
    //                     n->token.pos.row,
    //                     n->token.pos.col);
    //             }
    //         }
    //     }
    // } break;

    // case AST_NODE_BLOCK: {
    //     llvm_debug_scope_push(&c->llvm, n->token.pos.row, n->token.pos.col);

    //     AST_Node_Block *block = (AST_Node_Block *) n;
    //     for (AST_Node *it = block->body.head; it; it = it->next) {
    //         compile_stmt(c, it);
    //     }

    //     llvm_debug_scope_pop(&c->llvm);
    // } break;

    // case AST_NODE_IF: {
    //     AST_Node_If *iff = (AST_Node_If *) n;

    //     LLVM_Node_Block *consequence = llvm_block_new(&c->llvm);
    //     LLVM_Node_Block *antecedence = llvm_block_new(&c->llvm);

    //     LLVM_Node_Block *end = antecedence;
    //     if (iff->antecedence) {
    //         end = llvm_block_new(&c->llvm);
    //     }

    //     // Condition
    //     LLVM_Node *condition = compile_expr(c, iff->condition, false);
    //     llvm_debug_set_pos(
    //         &c->llvm,
    //         llvm_build_branch(&c->llvm, condition, consequence, antecedence),
    //         n->token.pos.row,
    //         n->token.pos.col);

    //     // Consequence
    //     llvm_build_block(&c->llvm, consequence);
    //     compile_stmt(c, iff->consequence);
    //     llvm_build_jump(&c->llvm, end);

    //     // Antecedence
    //     if (iff->antecedence) {
    //         llvm_build_block(&c->llvm, antecedence);
    //         compile_stmt(c, iff->antecedence);
    //         llvm_build_jump(&c->llvm, end);
    //     }

    //     // End
    //     llvm_build_block(&c->llvm, end);
    // } break;

    // case AST_NODE_FOR: {
    //     AST_Node_For *forr = (AST_Node_For *) n;
    //     if (forr->init) {
    //         llvm_debug_scope_push(&c->llvm, n->token.pos.row, n->token.pos.col);
    //         compile_stmt(c, forr->init);
    //     }

    //     LLVM_Node_Block *body = llvm_block_new(&c->llvm);
    //     LLVM_Node_Block *end = llvm_block_new(&c->llvm);

    //     LLVM_Node_Block *start = body;
    //     LLVM_Node_Block *update = start;
    //     if (forr->update) {
    //         update = llvm_block_new(&c->llvm);
    //     }

    //     LLVM_Node_Block *loop_break_save = c->loop_break;
    //     LLVM_Node_Block *loop_condition_save = c->loop_continue;
    //     c->loop_break = end;
    //     c->loop_continue = update;
    //     {
    //         // Condition
    //         if (forr->condition) {
    //             start = llvm_block_new(&c->llvm);
    //             llvm_build_jump(&c->llvm, start);
    //             llvm_build_block(&c->llvm, start);

    //             llvm_debug_set_pos(
    //                 &c->llvm,
    //                 llvm_build_branch(&c->llvm, compile_expr(c, forr->condition, false), body, end),
    //                 forr->condition->token.pos.row,
    //                 forr->condition->token.pos.col);
    //         } else {
    //             llvm_build_jump(&c->llvm, body);
    //         }

    //         // Body
    //         llvm_build_block(&c->llvm, body);
    //         compile_stmt(c, forr->body);

    //         // Update
    //         if (forr->update) {
    //             llvm_build_jump(&c->llvm, update);
    //             llvm_build_block(&c->llvm, update);
    //             compile_expr(c, forr->update, false);
    //         }

    //         // Loop
    //         llvm_build_jump(&c->llvm, start);

    //         // End
    //         llvm_build_block(&c->llvm, end);
    //     }
    //     c->loop_break = loop_break_save;
    //     c->loop_continue = loop_condition_save;

    //     if (forr->init) {
    //         llvm_debug_scope_pop(&c->llvm);
    //     }
    // } break;

    // case AST_NODE_JUMP:
    //     if (n->token.kind == TOKEN_BREAK) {
    //         llvm_build_jump(&c->llvm, c->loop_break);
    //     } else if (n->token.kind == TOKEN_CONTINUE) {
    //         llvm_build_jump(&c->llvm, c->loop_continue);
    //     } else {
    //         unreachable();
    //     }
    //     break;

    // case AST_NODE_RETURN: {
    //     AST_Node_Return *returnn = (AST_Node_Return *) n;

    //     LLVM_Node *value = compile_expr(c, returnn->value, false);
    //     if (ast_type_kind_eq(n->type, AST_TYPE_UNIT)) {
    //         value = NULL;
    //     }

    //     llvm_debug_set_pos(&c->llvm, llvm_build_return(&c->llvm, value), n->token.pos.row, n->token.pos.col);
    // } break;

    // case AST_NODE_EXTERN: {
    //     AST_Node_Extern *externn = (AST_Node_Extern *) n;
    //     for (AST_Node *it = externn->nodes.head; it; it = it->next) {
    //         compile_stmt(c, it);
    //     }
    // } break;

    // case AST_NODE_PRINT: {
    //     AST_Node_Print *print = (AST_Node_Print *) n;
    //     LLVM_Node      *value = compile_expr(c, print->value, false);
    //     llvm_debug_set_pos(&c->llvm, llvm_build_print(&c->llvm, value), n->token.pos.row, n->token.pos.col);
    // } break;

    // default:
    //     compile_expr(c, n, false);
    //     break;
    // }
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

#ifdef PLATFORM_X86_64_WINDOWS
#define OBJ_FILE_EXTENSION ".obj"
#else
#define OBJ_FILE_EXTENSION ".o"
#endif // PLATFORM_X86_64_WINDOWS

void compiler_build(Compiler *c, const char *output) {
    assert(c->cmd);
    assert(c->arena);

    if (!c->llvm_context) {
        compiler_init_llvm_target_data(c);
    }
    c->llvm_builder = LLVMCreateBuilderInContext(c->llvm_context);

    c->llvm_debug_builder = LLVMCreateDIBuilder(c->llvm_module);
    c->llvm_debug_file = LLVMDIBuilderCreateFile(c->llvm_debug_builder, c->path, strlen(c->path), ".", 1);

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

    const char *object = temp_sprintf("%s" OBJ_FILE_EXTENSION, output);
    {
        // TODO: Remove
        // LLVMPrintModuleToFile(c->llvm_module, "/dev/stdout", NULL);

        char *error = NULL;
        if (LLVMVerifyModule(c->llvm_module, LLVMReturnStatusAction, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

        if (LLVMTargetMachineEmitToFile(c->llvm_target_machine, c->llvm_module, object, LLVMObjectFile, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

#ifdef PLATFORM_X86_64_WINDOWS
        if (is_lld_available_in_path()) {
            cmd_push(c->cmd, "lld-link");
        } else {
            cmd_push(c->cmd, "link", "/nologo");
        }

        cmd_push(c->cmd, temp_sprintf("/out:%s", output));
        cmd_push(c->cmd, "/defaultlib:libcmt");
#else
        cmd_push(c->cmd, "cc");
        if (is_lld_available_in_path()) {
            cmd_push(c->cmd, "-fuse-ld=lld");
        }
        cmd_push(c->cmd, "-o", output);
#endif // PLATFORM_X86_64_WINDOWS

        cmd_push(c->cmd, object);
        cmd_push_many(c->cmd, c->link_flags->data, c->link_flags->count); // TODO: Windows

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

        delete_file(object);
    }
    temp_reset(object);

    // c->llvm.main_fn = llvm_fn_new(
    //     &c->llvm, sv_from_cstr("main"), llvm_type_fn(&c->llvm, NULL, 0, llvm_type_basic(LLVM_TYPE_I32)), false);

    // c->llvm.fn = c->llvm.main_fn;
    // llvm_build_call(&c->llvm, fn->llvm, NULL, 0, false);
    // llvm_build_return(&c->llvm, llvm_atom_int(&c->llvm, llvm_type_basic(LLVM_TYPE_I32), 0));
    // llvm_compile(&c->llvm);

    // #if 0
    // fwrite(c->llvm.sb.data, c->llvm.sb.count, 1, stdout);
    // sb_free(&c->llvm.sb);
    // exit(0);
    // #endif

    // c->cmd->count = 0;
    // cmd_push(c->cmd, "clang");
    // cmd_push(c->cmd, "-Wno-override-module");
    // cmd_push(c->cmd, "-o");
    // cmd_push(c->cmd, output);
    // cmd_push(c->cmd, "-x");
    // cmd_push(c->cmd, "ir");
    // cmd_push(c->cmd, "-");
    // cmd_push_many(c->cmd, c->link_flags->data, c->link_flags->count);

    // FILE *f = NULL;
    // Proc  proc = cmd_run_async(c->cmd, (Cmd_Stdio) {.in = &f});
    // if (proc == PROC_INVALID) {
    //     fprintf(stderr, "ERROR: Could not start process 'clang'\n");
    //     exit(1);
    // }

    // if (f) {
    //     fwrite(c->llvm.sb.data, sizeof(char), c->llvm.sb.count, f);
    //     fclose(f);
    // }

    // llvm_free(&c->llvm);
    // da_free(&c->globals);
    // da_free(&c->context.locals);

    // const int code = cmd_wait(proc);
    // if (code != 0) {
    //     fprintf(stderr, "ERROR: Process 'clang' exited abnormally with code %d\n", code);
    //     exit(1);
    // }
}
