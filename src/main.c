#include "basic.h"
#include "checker.h"
#include "compiler.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>

static void usage(FILE *f, const char *program) {
    fprintf(
        f,
        "Usage:\n"
        "    %s [FLAGS...] [FILE|DIRECTORY]\n"
        "\n"
        "Flags:\n"
        "    -h              Show this message\n"
        "    -r              Run the program\n"
        "    -o OUTPUT       Set the output path\n"
        "    --              End of compiler options. All following arguments are passed to the program if ran\n",
        program);
}

static const char *shift(int *argc, char ***argv, const char *program, const char *expected) {
    if (*argc <= 0) {
        fprintf(stderr, "ERROR: %s not provided\n\n", expected);
        usage(stderr, program);
        exit(1);
    }

    (*argc)--;
    return *(*argv)++;
}

static const char *get_path_last(const char *path) {
    SV sv = sv_from_cstr(path);
    if (sv.count && sv.data[sv.count - 1] == '/') {
        sv.count--;
    }

    for (size_t i = sv.count; i > 0; i--) {
        if (sv.data[i - 1] == '/') {
            return path + i;
        }
    }

    return NULL;
}

static const char *get_temp_file_path(void) {
#ifdef PLATFORM_X86_64_WINDOWS
    static char dir[MAX_PATH + 1];
    static char path[MAX_PATH + 1];

    DWORD count = GetTempPathA(sizeof(dir), dir);
    if (count == 0 || count >= sizeof(dir)) {
        return NULL;
    }

    if (GetTempFileNameA(dir, "glos", 0, path) == 0) {
        return NULL;
    }

    unixify_path_separators_inplace(path, strlen(path));
    return path;
#else
    static char path[] = "/tmp/glos_XXXXXX";

    int fd = mkstemp(path);
    if (fd < 0) {
        return NULL;
    }

    close(fd);
    return path;
#endif // PLATFORM_X86_64_WINDOWS
}

static const char *get_std_dir_path(Arena *a) {
    const void *checkpoint = temp_alloc(0);
    const char *result = NULL;

#ifdef PLATFORM_X86_64_LINUX
    i64   count = 0;
    char *data = NULL;

    for (size_t capacity = DA_INIT_CAP; true; capacity *= 2) {
        data = temp_alloc(capacity);
        count = readlink("/proc/self/exe", data, capacity);

        if (count == -1) {
            return_defer(NULL);
        }

        if ((size_t) count < capacity) {
            break;
        }

        temp_reset(checkpoint);
    }
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    uint32_t count = DA_INIT_CAP;
    char    *data = temp_alloc(count);

    int _NSGetExecutablePath(char *buffer, uint32_t *size);
    if (_NSGetExecutablePath(data, &count) == -1) {
        temp_reset(data);
        data = temp_alloc(count);

        if (_NSGetExecutablePath(data, &count) == -1) {
            temp_reset(data);
            return NULL;
        }
    }

    count = strlen(data);
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
    i64   count = 0;
    char *data = temp_alloc(0);

    for (size_t capacity = DA_INIT_CAP; true; capacity *= 2) {
        data = temp_alloc(capacity);
        count = GetModuleFileNameA(NULL, data, capacity);

        if (count == 0) {
            return_defer(NULL);
        }

        if (count < capacity - 1) {
            break;
        }
    }

    unixify_path_separators_inplace(data, count);
#endif // PLATFORM_X86_64_WINDOWS

    SV sv = {.data = data, .count = count};
    for (size_t i = sv.count; i; i--) {
        if (sv.data[i - 1] == '/') {
            sv.count = i;
            break;
        }
    }

    return_defer(arena_sprintf(a, SV_Fmt "std", SV_Arg(sv)));

defer:
    temp_reset(checkpoint);
    return result;
}

int main(int argc, char **argv) {
    atexit(temporary_files_cleanup);
    const char *program = shift(&argc, &argv, NULL, NULL);

    int   result = 0;
    Cmd   cmd = {0};
    Arena arena = {0};

    bool        run = false;
    const char *cwd = get_cwd(&arena);
    const char *input_path = NULL;
    const char *output_path = NULL;
    Link_Flags  link_flags = {.arena = &arena};
    while (argc) {
        const char *arg = shift(&argc, &argv, program, "Input path");
        if (*arg == '-') {
            if (!strcmp(arg, "-h")) {
                usage(stdout, program);
                exit(0);
            } else if (!strcmp(arg, "-r")) {
                run = true;
            } else if (!strcmp(arg, "-o")) {
                output_path = shift(&argc, &argv, program, "Output path");
            } else if (!strcmp(arg, "--")) {
                break;
            } else if (arg[1] == 'L') {
                const char *libpath = &arg[2];
                if (*libpath == '\0') {
                    libpath = shift(&argc, &argv, program, "Library path");
                }

                link_flags_add_libpath(&link_flags, sv_from_cstr(libpath));
            } else if (arg[1] == 'l') {
                const char *libname = &arg[2];
                if (*libname == '\0') {
                    libname = shift(&argc, &argv, program, "Library name");
                }

                link_flags_add_libname(&link_flags, sv_from_cstr(libname));
            } else {
                fprintf(stderr, "ERROR: Invalid flag '%s'\n\n", arg);
                usage(stderr, program);
                exit(1);
            }
        } else {
            if (input_path) {
                fprintf(stderr, "ERROR: Multiple input paths provided\n");
                if (run) {
                    fprintf(stderr, "Hint: When using '-r', pass program arguments after '--'\n");
                }
                exit(1);
            }

            input_path = arg;
        }
    }

    if (!input_path) {
        input_path = ".";
    }
    input_path = get_absolute_path(sv_from_cstr(cwd), sv_from_cstr(input_path), &arena);

    if (!output_path) {
        if (directory_exists(input_path)) {
            output_path = get_path_last(input_path);
            if (!output_path) {
                fprintf(stderr, "ERROR: Could not infer output path. Provide it manually via '-o'\n");
                exit(1);
            }
        } else {
            output_path = temp_sv_to_cstr(sv_strip_suffix(sv_from_cstr(input_path), sv_from_cstr(".glos")));
        }

        if (run) {
            const char *temp_path = get_temp_file_path();
            if (temp_path) {
                temporary_files_push(temp_path);
                output_path = temp_path;
            }
        } else {
            if (directory_exists(output_path)) {
                fprintf(stderr, "ERROR: The output path '%s' exists and is a directory\n", output_path);
                exit(1);
            }
        }
    }

    Modules modules = {0};
    Parser  parser = {
         .arena = &arena,
         .modules = &modules,

         .cwd = cwd,
         .std = get_std_dir_path(&arena),
    };

    Compiler compiler = {
        .arena = &arena,
        .parser = &parser,
        .modules = &modules,

        .cmd = &cmd,
        .link_flags = &link_flags,
    };

    // Import the builtin module
    {
        const char *name = "builtin";
        const char *root = parser.std;
        const char *absolute_path = get_absolute_path(sv_from_cstr(root), sv_from_cstr(name), &arena);
        assert(directory_exists(absolute_path));

        compiler.builtin_module = module_get(&parser, absolute_path);
        compiler.builtin_module->name = name;

        parser.module_current = compiler.builtin_module;

        switch (parse_directory(&parser, compiler.builtin_module->relative_path)) {
        case PARSE_OK:
            // Pass
            break;

        case PARSE_FAILURE:
            fprintf(stderr, "ERROR: Could not read directory '%s'\n", compiler.builtin_module->relative_path);
            exit(1);
            break;

        case PARSE_EMPTY_DIRECTORY:
            fprintf(
                stderr,
                "ERROR: Directory '%s' does not contain any glos files\n",
                compiler.builtin_module->relative_path);
            exit(1);
            break;

        default:
            unreachable();
        }

        parser.module_current = NULL;
    }

    compiler.main_module = module_get(&parser, input_path);
    compiler.main_module->name = "main";

    parser.module_current = compiler.main_module;
    if (directory_exists(input_path)) {
        parser.root = input_path;
        input_path = get_relative_path(sv_from_cstr(parser.cwd), sv_from_cstr(input_path), &arena);

        switch (parse_directory(&parser, input_path)) {
        case PARSE_OK:
            // Pass
            break;

        case PARSE_FAILURE:
            fprintf(stderr, "ERROR: Could not read directory '%s'\n", input_path);
            exit(1);
            break;

        case PARSE_EMPTY_DIRECTORY:
            fprintf(stderr, "ERROR: Directory '%s' does not contain any glos files\n", input_path);
            exit(1);
            break;

        default:
            unreachable();
        }
    } else {
        parser.root = get_parent_dir_path(input_path, &arena);
        input_path = get_relative_path(sv_from_cstr(parser.cwd), sv_from_cstr(input_path), &arena);

        if (parse_file(&parser, input_path) != PARSE_OK) {
            fprintf(stderr, "ERROR: Could not read file '%s'\n", input_path);
            exit(1);
        }
    }

#ifdef PLATFORM_X86_64_WINDOWS
    if (!sv_has_suffix(sv_from_cstr(output_path), sv_from_cstr(".exe"))) {
        output_path = temp_sprintf("%s.exe", output_path);
    }
#endif // PLATFORM_X86_64_WINDOWS

    if (run) {
        temporary_files_push(output_path);
    }

    check_nodes(&compiler);
    ll_foreach(it, &modules) {
        const char *path = it->absolute_path;
        if (it == compiler.main_module) {
            path = parser.root;
        }
        link_flags_add_libpath(&link_flags, sv_from_cstr(path));
    }

    compiler_build(&compiler, output_path);

    if (run) {
        const char *child_name = output_path;

#ifndef PLATFORM_X86_64_WINDOWS
        if (!sv_find(sv_from_cstr(child_name), '/', NULL)) {
            child_name = temp_sprintf("./%s", child_name);
        }
#endif // PLATFORM_X86_64_WINDOWS

        cmd.count = 0;
        cmd_push(&cmd, child_name);
        cmd_push_many(&cmd, argv, argc);

        const Proc child_proc = cmd_run_async(&cmd, (Cmd_Stdio) {0});
        if (child_proc.id == PROC_INVALID) {
            fprintf(stderr, "ERROR: Could not start process '%s'\n", child_name);
            exit(1);
        }

        result = cmd_wait(child_proc);
    }

    modules_free(&modules);
    parser_free(&parser);
    arena_free(&arena);
    cmd_free(&cmd);
    da_free(&link_flags);
    return result;
}
