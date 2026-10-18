#include "compiler/compiler.h"
#include "basic.h"
#include "checker.h"
#include "error.h"
#include "lld.h"

void link_flags_add_libpath(Link_Flags *ls, SV path) {
#ifdef PLATFORM_X86_64_WINDOWS
    da_push(ls, arena_sprintf(&default_arena, "/libpath:" SV_Fmt, SV_Arg(path)));
#else
    da_push(ls, arena_sprintf(&default_arena, "-L" SV_Fmt, SV_Arg(path)));
#endif // PLATFORM_X86_64_WINDOWS
}

void link_flags_add_libname(Link_Flags *ls, SV name) {
#ifdef PLATFORM_X86_64_LINUX
    da_push(ls, arena_sprintf(&default_arena, "-l" SV_Fmt, SV_Arg(name)));
#endif //  PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    if (sv_has_suffix(name, SV_Lit(".framework"))) {
        da_push(ls, "-framework");
        da_push(ls, arena_sv_to_cstr(&default_arena, sv_strip_suffix(name, SV_Lit(".framework"))));
    } else {
        da_push(ls, arena_sprintf(&default_arena, "-l" SV_Fmt, SV_Arg(name)));
    }
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
    da_push(ls, arena_sprintf(&default_arena, SV_Fmt ".lib", SV_Arg(name)));
#endif // PLATFORM_X86_64_WINDOWS
}

static void compiler_init_llvm_target_data(Compiler *c) {
    if (LLVMInitializeNativeTarget() != 0) {
        error_standalone(EK_ERROR, "Failed to initialize native target");
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
        error_standalone(EK_ERROR, "%s", error);
        exit(1);
    }

    LLVMCodeGenOptLevel opt_level;
    switch (c->optimization_level) {
    case O0:
        opt_level = LLVMCodeGenLevelNone;
        break;

    case O1:
        opt_level = LLVMCodeGenLevelLess;
        break;

    case O2:
        opt_level = LLVMCodeGenLevelDefault;
        break;

    case O3:
        opt_level = LLVMCodeGenLevelAggressive;
        break;

    default:
        unreachable();
        break;
    }

    c->llvm_target_machine =
        LLVMCreateTargetMachine(target, triple, "generic", "", opt_level, LLVMRelocPIC, LLVMCodeModelDefault);
    c->llvm_target_data = LLVMCreateTargetDataLayout(c->llvm_target_machine);

    // Initialize the common types
    {
        LLVMTypeRef slice_fields[] = {
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMInt64TypeInContext(c->llvm_context),
        };
        c->llvm_slice_type = LLVMStructTypeInContext(c->llvm_context, slice_fields, len(slice_fields), false);

        LLVMTypeRef trait_fields[] = {
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMPointerTypeInContext(c->llvm_context, 0),
        };
        c->llvm_trait_type = LLVMStructTypeInContext(c->llvm_context, trait_fields, len(trait_fields), false);

        LLVMTypeRef dynamic_array_fields[] = {
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMInt64TypeInContext(c->llvm_context),
            LLVMInt64TypeInContext(c->llvm_context),
            c->llvm_trait_type,
        };
        c->llvm_dynamic_array_or_map_type =
            LLVMStructTypeInContext(c->llvm_context, dynamic_array_fields, len(dynamic_array_fields), false);
    }

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
    const void *checkpoint = arena_alloc(&temp_arena, 0);

    assert(c->cmd);
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

#ifdef PLATFORM_X86_64_WINDOWS
    {
        LLVMTypeRef i32 = LLVMInt32TypeInContext(c->llvm_context);

        LLVMAddModuleFlag(
            c->llvm_module, LLVMModuleFlagBehaviorWarning, "CodeView", 8, LLVMValueAsMetadata(LLVMConstInt(i32, 1, 0)));

        LLVMAddModuleFlag(
            c->llvm_module,
            LLVMModuleFlagBehaviorWarning,
            "Debug Info Version",
            18,
            LLVMValueAsMetadata(LLVMConstInt(i32, LLVMDebugMetadataVersion(), 0)));

        if (c->run) {
            temporary_files_push(temp_replace_suffix(output_path, EXE_FILE_EXTENSION, ".pdb"));
        }
    }
#endif // PLATFORM_X86_64_WINDOWS

    compile_type(c, &c->hasher_type);
    compile_type(c, &c->format_type);
    compile_type(c, &c->type_info_type);
    compile_type(c, &c->source_code_location_type);

    if (c->error_enums_list.count) {
        LLVMValueRef *enums = arena_alloc(&default_arena, c->error_enums_list.count * sizeof(*enums));
        for (size_t i = 0; i < c->error_enums_list.count; i++) {
            enums[i] = compile_type_info(c, &c->error_enums_list.data[i]->node.type);
        }

        LLVMValueRef slice = create_const_slice_from_memory(
            c, LLVMPointerTypeInContext(c->llvm_context, 0), enums, c->error_enums_list.count);

        arena_reset(&default_arena, enums);
        if (!c->error_enums_var->definition_spec->llvm) {
            compile_var_def(c, c->error_enums_var);
        }
        LLVMSetInitializer(c->error_enums_var->definition_spec->llvm, slice);
    }

    perf_begin();
    {
        const Const_Value entry = get_const_definition_value(c, c->builtin_module, sv_from_cstr("runtime_entry"), NULL);
        assert(entry.kind == CONST_VALUE_FN);
        compile_fn(c, entry.as.fn);
    }
    perf_end("LLVM code generation");

    perf_begin();

    {
        char passes[16];
        snprintf(passes, sizeof(passes), "default<O%d>", c->optimization_level);

        LLVMPassBuilderOptionsRef pass_builder_options = LLVMCreatePassBuilderOptions();
        LLVMRunPasses(c->llvm_module, passes, c->llvm_target_machine, pass_builder_options);
        LLVMDisposePassBuilderOptions(pass_builder_options);
    }

    LLVMDIBuilderFinalize(c->llvm_debug_builder);
    LLVMDisposeDIBuilder(c->llvm_debug_builder);

    const char *object_path = temp_replace_suffix(output_path, EXE_FILE_EXTENSION, OBJ_FILE_EXTENSION);
    temporary_files_push(object_path);
    {
        // LLVMPrintModuleToFile(c->llvm_module, "/dev/stdout", NULL);
        char *error = NULL;
        // if (LLVMVerifyModule(c->llvm_module, LLVMReturnStatusAction, &error)) {
        //     error_standalone(EK_ERROR, "%s", error);
        //     exit(1);
        // }

        if (LLVMTargetMachineEmitToFile(c->llvm_target_machine, c->llvm_module, object_path, LLVMObjectFile, &error)) {
            error_standalone(EK_ERROR, "%s", error);
            exit(1);
        }

        LLVMDisposeTargetData(c->llvm_target_data);
        LLVMDisposeTargetMachine(c->llvm_target_machine);

        LLVMDisposeBuilder(c->llvm_builder);
        LLVMDisposeModule(c->llvm_module);
        LLVMContextDispose(c->llvm_context);
    }
    perf_end("LLVM -> object file");

    perf_begin();
    {
#ifdef PLATFORM_X86_64_LINUX
        cmd_push(c->cmd, "ld.lld");
        cmd_push(c->cmd, "-dynamic-linker", "/lib64/ld-linux-x86-64.so.2");
        cmd_push(c->cmd, "-o", output_path);
        cmd_push(c->cmd, object_path);

        static const char *libc_dirs[] = {
            "/usr/lib/x86_64-linux-gnu",
            "/lib/x86_64-linux-gnu",
            "/usr/lib64",
            "/lib64",
        };

        const char *libc_dir = NULL;
        const char *libc_crt1_path = NULL;
        const char *libc_crti_path = NULL;
        const char *libc_crtn_path = NULL;
        for (size_t i = 0; i < len(libc_dirs); i++) {
            const char *it = libc_dirs[i];

            libc_crt1_path = arena_sprintf(&temp_arena, "%s/crt1.o", libc_dirs[i]);
            if (file_exists(libc_crt1_path)) {
                libc_dir = it;
                libc_crti_path = arena_sprintf(&temp_arena, "%s/crti.o", libc_dirs[i]);
                libc_crtn_path = arena_sprintf(&temp_arena, "%s/crtn.o", libc_dirs[i]);
                break;
            }

            arena_reset(&temp_arena, libc_crt1_path);
        }

        if (!libc_dir) {
            error_standalone(EK_ERROR, "Could not find the C runtime objects");
            exit(1);
        }

        cmd_push(c->cmd, "-L", libc_dir);
        cmd_push(c->cmd, libc_crt1_path, libc_crti_path, libc_crtn_path);

        cmd_push_many(c->cmd, c->link_flags->data, c->link_flags->count);
        cmd_push(c->cmd, "-lc", "-lm");
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
        SV sdk_path = {0};
        {
            cmd_push(c->cmd, "xcrun", "--show-sdk-path");
            FILE *f = NULL;
            Proc  proc = cmd_run_async(c->cmd, (Cmd_Stdio) {.out = &f});
            if (proc.id == PROC_INVALID) {
                error_standalone(EK_ERROR, "Could not execute 'xcrun'. Make sure a C SDK is setup properly.");
                exit(1);
            }

            if (!read_fp(f, &sdk_path, &temp_arena, true)) {
                error_standalone(EK_ERROR, "Could not read standard output of 'xcrun'");
                exit(1);
            }

            const int code = cmd_wait(proc);
            if (code) {
                error_standalone(EK_ERROR, "Process 'xcrun' exited abnormally with code %d", code);
                exit(1);
            }

            sdk_path = sv_strip_suffix(sdk_path, SV_Lit("\n"));
        }

        if (!sdk_path.count) {
            sdk_path = SV_Lit("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk");
        }

        cmd_push(c->cmd, "ld64.lld");
        cmd_push(c->cmd, "-arch", "arm64");
        cmd_push(c->cmd, "-platform_version", "macos", "14.0", "14.0");
        cmd_push(c->cmd, "-o", output_path);
        cmd_push(c->cmd, object_path);

        cmd_push(c->cmd, "-syslibroot", arena_sv_to_cstr(&temp_arena, sdk_path));
        cmd_push_many(c->cmd, c->link_flags->data, c->link_flags->count);
        cmd_push(c->cmd, "-lSystem", "-lm");
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
        cmd_push(c->cmd, "lld-link");
        cmd_push(c->cmd, arena_sprintf(&temp_arena, "/out:%s", output_path));
        cmd_push(c->cmd, object_path);

        cmd_push(c->cmd, "/debug", "/defaultlib:libcmt");
        cmd_push_many(c->cmd, c->link_flags->data, c->link_flags->count);
#endif // PLATFORM_X86_64_WINDOWS

        const int code = run_lld(c->cmd->data, c->cmd->count);
        if (code != 0) {
            error_standalone(EK_ERROR, "Linker exited abnormally with code %d", code);
            exit(1);
        }

        c->cmd->count = 0;

        // #ifdef PLATFORM_X86_64_WINDOWS
        //         if (is_lld_available_in_path()) {
        //             cmd_push(c->cmd, "lld-link");
        //         } else {
        //             cmd_push(c->cmd, "link", "/nologo");
        //         }

        //         cmd_push(c->cmd, arena_sprintf(&temp_arena, "/out:%s", output_path));
        //         cmd_push(c->cmd, "/debug", "/defaultlib:libcmt");
        // #else
        //         cmd_push(c->cmd, "cc");
        //         if (is_lld_available_in_path()) {
        //             cmd_push(c->cmd, "-fuse-ld=lld");
        //         }
        //         cmd_push(c->cmd, "-o", output_path);
        //         cmd_push(c->cmd, "-g");
        // #endif // PLATFORM_X86_64_WINDOWS

        //         cmd_push(c->cmd, object_path);
        //         cmd_push_many(c->cmd, c->link_flags->data, c->link_flags->count);

        // #ifndef PLATFORM_X86_64_WINDOWS
        //         // The '%' operator needs the math library to operate on floats in unoptimized builds.
        //         cmd_push(c->cmd, "-lm");
        // #endif // PLATFORM_X86_64_WINDOWS

        //         const char *proc_name = c->cmd->data[0];
        //         Proc        proc = cmd_run_async(c->cmd, (Cmd_Stdio) {0});
        //         if (proc.id == PROC_INVALID) {
        //             error_standalone(EK_ERROR, "Could not execute '%s'. Make sure a C SDK is setup properly",
        //             proc_name); exit(1);
        //         }

        //         const int proc_code = cmd_wait(proc);
        //         if (proc_code != 0) {
        //             error_standalone(EK_ERROR, "Process '%s' exited abnormally with code %d", proc_name, proc_code);
        //             exit(1);
        //         }
    }
    perf_end("Linking object files");

    ht_free(&c->llvm_debug_files);
    ht_free(&c->type_info_cache);

    da_free(&c->error_enums_list);

    ht_free(&c->methods_table);
    da_free(&c->methods_to_check);

    ht_free(&c->map_operators);

    da_free(&c->partial_stack);

    ht_free(&c->monomorph_intern);
    ht_free(&c->monomorph_replacements);
    da_free(&c->monomorph_parameters);
    da_free(&c->monomorphization_stack);

    da_free(&c->context.defines);
    da_free(&c->context.imports);

    da_free(&c->struct_fields);
    da_free(&c->trait_methods);
    da_free(&c->designated_initializers);

    da_free(&c->arg_values);
    da_free(&c->group_values);
    da_free(&c->defers);
    arena_reset(&temp_arena, checkpoint);
}
