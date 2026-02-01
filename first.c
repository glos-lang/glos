#include "src/basic.c"

#ifdef PLATFORM_X86_64_WINDOWS
#define OBJ_FILE_EXTENSION ".obj"
#define EXE_FILE_EXTENSION ".exe"
#else
#define OBJ_FILE_EXTENSION ".o"
#define EXE_FILE_EXTENSION ""
#endif // PLATFORM_X86_64_WINDOWS

static const char *replace_extension(const char *path, const char *old, const char *new) {
    const SV base = sv_strip_suffix(sv_from_cstr(path), sv_from_cstr(old));
    return temp_sprintf(SV_Fmt "%s", SV_Arg(base), new);
}

static bool build_glos(size_t nprocs) {
    static const char *headers[] = {
        "src/ast.h",
        "src/basic.h",
        "src/checker.h",
        "src/compiler.h",
        "src/lexer.h",
        "src/llvm.h",
        "src/parser.h",
        "src/token.h",
    };

    static const char *sources[] = {
        "src/ast.c",
        "src/basic.c",
        "src/checker.c",
        "src/compiler.c",
        "src/lexer.c",
        "src/llvm.c",
        "src/main.c",
        "src/parser.c",
        "src/token.c",
    };

    bool        result = true;
    const void *save = temp_alloc(0);

    Cmd   cmd = {0};
    Procs procs = {0};

    size_t headers_time = 0;
    for (size_t i = 0; i < len(headers); i++) {
        const size_t time = get_path_modified_time(headers[i]);
        headers_time = max(headers_time, time);
    }

    bool built_objects = false;
    for (size_t i = 0; i < len(sources); i++) {
        const char  *src = sources[i];
        const char  *obj = replace_extension(src, ".c", OBJ_FILE_EXTENSION);
        const size_t src_time = get_path_modified_time(src);
        const size_t obj_time = get_path_modified_time(obj);
        if (obj_time >= src_time && obj_time >= headers_time) {
            continue;
        }

        fprintf(stderr, "Building '%s'\n", obj);
        built_objects = true;

        da_push(&cmd, "clang");
        da_push(&cmd, "-ggdb");
        da_push(&cmd, "-o");
        da_push(&cmd, obj);
        da_push(&cmd, src);
        da_push(&cmd, "-c");

        const Proc proc = cmd_run_async(&cmd, (Cmd_Stdio) {0});
        if (proc == PROC_INVALID) {
            fprintf(stderr, "ERROR: Could not start process 'clang'\n");
            exit(1);
        }

        if (!procs_push(&procs, proc, nprocs)) {
            fprintf(stderr, "ERROR: Process 'clang' exited abnormally\n");
            return_defer(false);
        }
    }

    if (!procs_flush(&procs)) {
        fprintf(stderr, "ERROR: Process 'clang' exited abnormally\n");
        return_defer(false);
    }

    if (!built_objects) {
        fprintf(stderr, "OK\n");
        return_defer(true);
    }

    fprintf(stderr, "Building 'glos" EXE_FILE_EXTENSION "'\n");
    da_push(&cmd, "clang");
    da_push(&cmd, "-o");
    da_push(&cmd, "glos" EXE_FILE_EXTENSION);
    for (size_t i = 0; i < len(sources); i++) {
        da_push(&cmd, replace_extension(sources[i], ".c", OBJ_FILE_EXTENSION));
    }

    if (cmd_run_sync(&cmd, (Cmd_Stdio) {0})) {
        fprintf(stderr, "ERROR: Process 'clang' exited abnormally\n");
        return_defer(false);
    }

    fprintf(stderr, "OK\n");

defer:
    da_free(&cmd);
    da_free(&procs);
    temp_reset(save);
    return result;
}

int main(void) {
    if (!build_glos(5)) {
        exit(1);
    }
    return 0;
}
