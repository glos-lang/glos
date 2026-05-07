#include "compiler.h"
#include "ast.h"
#include "basic.h"
#include "dwarf.h"
#include "token.h"

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

    if (ast_type_is_pointer(*type) || type->kind == AST_TYPE_FN) {
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
        todo();
        // assert(type->spec.structt.definition);

        // // TODO: Think of a better solution
        // AST_Type *common = &type->spec.structt.definition->node.type;
        // assert(common->kind == AST_TYPE_STRUCT);

        // if (common->llvm.kind != LLVM_TYPE_STRUCT) {
        //     // TODO: Consider proper name
        //     const char           *name = temp_sprintf("anon.%zu", c->iota_anonymous_struct++);
        //     const AST_Type_Struct spec = common->spec.structt;

        //     LLVM_Field *fields = arena_alloc(c->llvm.arena, spec.fields_count * sizeof(*fields));
        //     common->llvm = llvm_type_struct(&c->llvm, fields, spec.fields_count, sv_from_cstr(name));

        //     for (size_t i = 0; i < spec.fields_count; i++) {
        //         AST_Node *it = (AST_Node *) spec.fields[i];
        //         compile_type(c, &it->type);
        //         fields[i].name = it->token.sv;
        //         fields[i].type = it->type.llvm;
        //     }
        // }
        // type->llvm = common->llvm;
    } break;

    default:
        unreachable();
        break;
    }
}

static void compile_fn_type(Compiler *c, AST_Type *type) {
    const AST_Type_Fn type_spec = type->spec.fn;
    compile_type(c, type_spec.returnn);

    LLVMTypeRef *args = temp_alloc(type_spec.args_count * sizeof(*args));
    for (size_t i = 0; i < type_spec.args_count; i++) {
        AST_Type *it = &type_spec.args[i]->node.type;
        compile_type(c, it);
        args[i] = it->llvm;
    }

    type->llvm = LLVMFunctionType(type_spec.returnn->llvm, args, type_spec.args_count, false);
    temp_reset(args);
}

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
    assert(!type.is_type);
    if (type.ref) {
        todo(); // TODO(@libllvm): Pointer types
    }

    switch (type.kind) {
    case AST_TYPE_UNIT:
        unreachable();

    case AST_TYPE_BOOL:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "bool", strlen("bool"), 8, DW_ATE_boolean, LLVMDIFlagZero);

    case AST_TYPE_I8:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "i8", strlen("i8"), 8, DW_ATE_signed, LLVMDIFlagZero);

    case AST_TYPE_I16:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "i16", strlen("i16"), 16, DW_ATE_signed, LLVMDIFlagZero);

    case AST_TYPE_I32:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "i32", strlen("i32"), 32, DW_ATE_signed, LLVMDIFlagZero);

    case AST_TYPE_I64:
    case AST_TYPE_INT:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "i64", strlen("i64"), 64, DW_ATE_signed, LLVMDIFlagZero);

    case AST_TYPE_U8:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "u8", strlen("u8"), 8, DW_ATE_unsigned, LLVMDIFlagZero);

    case AST_TYPE_U16:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "u16", strlen("u16"), 16, DW_ATE_unsigned, LLVMDIFlagZero);

    case AST_TYPE_U32:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "u32", strlen("u32"), 32, DW_ATE_unsigned, LLVMDIFlagZero);

    case AST_TYPE_U64:
        return LLVMDIBuilderCreateBasicType(
            c->llvm_debug_builder, "u64", strlen("u64"), 64, DW_ATE_unsigned, LLVMDIFlagZero);

    case AST_TYPE_RAWPTR:
        todo();
        break;

    case AST_TYPE_FN:
        todo();
        break;

    case AST_TYPE_STRUCT:
        todo();
        break;

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

static LLVMValueRef compile_fn(Compiler *c, AST_Node_Fn *fn) {
    if (!fn->llvm) {
        const void *checkpoint = temp_alloc(0);

        compile_fn_type(c, &fn->node.type);
        if (fn->is_extern) {
            assert(fn->defined_as);
            fn->llvm =
                LLVMAddFunction(c->llvm_module, temp_sv_to_cstr(fn->defined_as->node.token.sv), fn->node.type.llvm);
        } else {
            LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;
            LLVMValueRef    llvm_fn_save = c->llvm_fn;

            SV fn_name = sv_from_cstr(temp_emit_fn_name(c, fn));
            fn->llvm = LLVMAddFunction(c->llvm_module, fn_name.data, fn->node.type.llvm);
            c->llvm_fn = fn->llvm;

            // TODO(@libllvm): Function return
            // TODO(@libllvm): Function arguments
            LLVMMetadataRef fn_debug_type =
                LLVMDIBuilderCreateSubroutineType(c->llvm_debug_builder, c->llvm_debug_file, NULL, 0, LLVMDIFlagZero);

            c->llvm_debug_scope = LLVMDIBuilderCreateFunction(
                c->llvm_debug_builder,
                c->llvm_debug_file,
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
                LLVMDIFlagZero,
                false);

            LLVMSetSubprogram(fn->llvm, c->llvm_debug_scope);

            LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, fn->llvm, ""));
            assert(!fn->args_count); // TODO(@libllvm): Function arguments

            assert(fn->body->kind == AST_NODE_BLOCK);
            AST_Node_Block *block = (AST_Node_Block *) fn->body;
            for (AST_Node *it = block->body.head; it; it = it->next) {
                compile_stmt(c, it);
            }

            assert(!fn->returnn); // TODO(@libllvm): Function return

            set_debug_location(c, block->end);
            LLVMBuildRetVoid(c->llvm_builder);

            c->llvm_fn = llvm_fn_save;
            c->llvm_debug_scope = llvm_debug_scope_save;
        }

        temp_reset(checkpoint);
    }

    return fn->llvm;

    // LLVM_Node_Fn *llvm_fn_save = c->llvm.fn;
    // {
    //     compile_type(c, &fn->node.type);
    //     if (fn->is_extern) {
    //         assert(fn->defined_as);
    //         fn->llvm = (LLVM_Node *) llvm_fn_new(&c->llvm, fn->defined_as->node.token.sv, fn->node.type.llvm, true);
    //         return fn->llvm;
    //     }

    //     const char *name = arena_clone_from_temp(c->llvm.arena, temp_emit_fn_name(c, fn));
    //     c->llvm.fn = llvm_fn_new(&c->llvm, sv_from_cstr(name), fn->node.type.llvm, false);
    //     fn->llvm = (LLVM_Node *) c->llvm.fn;
    //     llvm_fn_debug_set_pos(&c->llvm, c->llvm.fn, fn->node.token.pos.row, fn->node.token.pos.col);

    //     llvm_debug_scope_push(&c->llvm, fn->node.token.pos.row, fn->node.token.pos.col);
    //     {
    //         size_t arg_iota = 0;
    //         for (AST_Node *arg = fn->args.head; arg; arg = arg->next) {
    //             assert(arg->kind == AST_NODE_DEFINE);
    //             AST_Node_Define *define = (AST_Node_Define *) arg;

    //             assert(define->name->kind == AST_NODE_ATOM);
    //             AST_Node_Atom *it = (AST_Node_Atom *) define->name;

    //             LLVM_Node_Var *var = llvm_fn_arg_get(c->llvm.fn, arg_iota++);
    //             llvm_var_set_name(var, it->node.token.sv);
    //             llvm_var_debug_set_pos(&c->llvm, var, it->node.token.pos.row, it->node.token.pos.col);
    //             it->llvm = (LLVM_Node *) var;
    //         }

    //         assert(fn->body->kind == AST_NODE_BLOCK);
    //         AST_Node_Block *block = (AST_Node_Block *) fn->body;

    //         for (AST_Node *it = block->body.head; it; it = it->next) {
    //             compile_stmt(c, it);
    //         }

    //         LLVM_Node *returnn = NULL;
    //         if (fn->returnn) {
    //             returnn = llvm_atom_zero(&c->llvm, fn->node.type.spec.fn.returnn->llvm);
    //         }
    //         // TODO: Not needed for non-void functions
    //         // Then the `if (value->kind == LLVM_NODE_LOAD)` in `llvm_build_return()` can be turned into an assertion
    //         llvm_debug_set_pos(&c->llvm, llvm_build_return(&c->llvm, returnn), block->end.row, block->end.col);
    //     }
    //     llvm_debug_scope_pop(&c->llvm);
    // }
    // c->llvm.fn = llvm_fn_save;

    // return fn->llvm;
}

// TODO:
static_assert(COUNT_CONST_VALUES == 4, "");
// static LLVM_Node_Var_Init *compile_const_value_to_var_init(Compiler *c, LLVM_Type type, Const_Value value) {
//     todo();
//     unused(c);
//     unused(type);
//     unused(value);
//     // switch (value.kind) {
//     // case CONST_VALUE_INT:
//     //     return llvm_var_init_new_int(&c->llvm, type, value.as.integer);

//     // case CONST_VALUE_FN:
//     //     return llvm_var_init_new_node(&c->llvm, compile_fn(c, value.as.fn));

//     // case CONST_VALUE_TYPE:
//     //     unreachable();

//     // case CONST_VALUE_STRUCT: {
//     //     const AST_Type_Struct spec = value.as.structt.spec;

//     //     LLVM_Node_Var_Init **fields = arena_alloc(c->llvm.arena, spec.fields_count * sizeof(*fields));
//     //     for (size_t i = 0; i < spec.fields_count; i++) {
//     //         // TODO: Think of a better solution for common metadata for a struct
//     //         const LLVM_Type it_type = spec.definition->node.type.llvm.structt.fields[i].type;
//     //         fields[i] = compile_const_value_to_var_init(c, it_type, value.as.structt.fields[i]);
//     //     }

//     //     return llvm_var_init_new_struct(&c->llvm, type, fields, spec.fields_count);
//     // }

//     // default:
//     //     unreachable();
//     //     break;
//     // }
// }

// TODO: Replace
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
                    todo(); // TODO(@libllvm)

                    // // TODO: Don't generate this over and over
                    // LLVM_Node_Var_Init *value =
                    //     compile_const_value_to_var_init(c, n->type.llvm, definition->const_value);

                    // const char *name = temp_sprintf("const.anon.%zu", c->iota_anonymous_const++);

                    // LLVM_Node *memory = llvm_const_new(&c->llvm, sv_from_cstr(name), n->type.llvm, value);
                    // if (ref) {
                    //     return_defer(memory);
                    // }
                    // return_defer(llvm_build_load(&c->llvm, memory, n->type.llvm));
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
            todo();

        case TOKEN_BAND:
            todo();

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
        // TODO: Pointer arithmetic

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

                set_debug_location(c, n->token.pos);
                if (op.u && !ast_type_is_signed(binary->lhs->type)) {
                    return op.u(c->llvm_builder, lhs, rhs, "");
                } else {
                    return op.i(c->llvm_builder, lhs, rhs, "");
                }
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
    } break;

    default:
        todo();
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

// TODO: Replace
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

    assert(!it->is_local); // TODO(@libllvm): Local variables
    it->llvm = LLVMAddGlobal(c->llvm_module, it->node.type.llvm, name.data);

    if (!it->is_extern) {
        assert(!it->is_assigned); // TODO(@libllvm): Variable assignment
        LLVMSetInitializer(it->llvm, LLVMConstNull(it->node.type.llvm));

        LLVMMetadataRef var_debug_type = get_debug_for_type(c, it->node.type);
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

    // if (!it->is_local && it->is_assigned) {
    //     llvm_var_set_init(var, compile_const_value_to_var_init(c, it->node.type.llvm, it->const_value));
    // }

    temp_reset(checkpoint);
}

static_assert(COUNT_AST_NODES == 16, "");
static void compile_stmt(Compiler *c, AST_Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
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

    case AST_NODE_PRINT: {
        AST_Node_Print *print = (AST_Node_Print *) n;
        LLVMValueRef    args[] = {
            ast_type_is_signed(print->value->type) ? c->llvm_iprint_str : c->llvm_uprint_str,
            compile_expr(c, print->value, false),
        };

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

    if (!ast_type_kind_eq(*signature.returnn, AST_TYPE_UNIT)) {
        fprintf(stderr, Pos_Fmt "ERROR: Function 'main' cannot return anything\n", Pos_Arg(main->node.token.pos));
        exit(1);
    }
    return main->const_value.as.fn;
}

size_t compile_sizeof(Compiler *c, AST_Type *type) {
    compile_type(c, type);
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

    c->llvm_context = LLVMContextCreate();
    c->llvm_module = LLVMModuleCreateWithNameInContext("", c->llvm_context);
    c->llvm_target_data = LLVMGetModuleDataLayout(c->llvm_module);
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
        char *error = NULL;
        if (LLVMVerifyModule(c->llvm_module, LLVMReturnStatusAction, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

        if (LLVMInitializeNativeTarget() != 0) {
            fprintf(stderr, "ERROR: Failed to initialize native target\n");
            exit(1);
        }
        LLVMInitializeNativeAsmPrinter();

        char *triple = LLVMGetDefaultTargetTriple();
        LLVMSetTarget(c->llvm_module, triple);

        LLVMTargetRef target;
        if (LLVMGetTargetFromTriple(triple, &target, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

        LLVMTargetMachineRef target_machine = LLVMCreateTargetMachine(
            target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);

        if (LLVMTargetMachineEmitToFile(target_machine, c->llvm_module, object, LLVMObjectFile, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

#ifdef PLATFORM_X86_64_WINDOWS
        cmd_push(c->cmd, "link", "/nologo");
        cmd_push(c->cmd, temp_sprintf("/out:%s", output));
        cmd_push(c->cmd, "/defaultlib:libcmt");
#else
        cmd_push(c->cmd, "cc");
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
