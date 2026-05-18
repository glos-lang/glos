#include "basic.h"
#include "checker.h"
#include "compiler.h"
#include "node.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>

static void usage(FILE *f, const char *program) {
    fprintf(
        f,
        "Usage:\n"
        "    %s [FLAGS...] FILE\n"
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

int main(int argc, char **argv) {
    atexit(temp_paths_cleanup);
    const char *program = shift(&argc, &argv, NULL, NULL);

    int   result = 0;
    Cmd   cmd = {0};
    Arena arena = {0};

    bool        run = false;
    const char *input = NULL;
    const char *output_path = NULL;
    Link_Flags  link_flags = {0};
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

#ifdef PLATFORM_X86_64_WINDOWS
                da_push(&link_flags, arena_sprintf(&arena, "/libpath:%s", libpath));
#else
                da_push(&link_flags, "-L");
                da_push(&link_flags, libpath);
#endif // PLATFORM_X86_64_WINDOWS
            } else if (arg[1] == 'l') {
                const char *libname = &arg[2];
                if (*libname == '\0') {
                    libname = shift(&argc, &argv, program, "Library name");
                }

#ifdef PLATFORM_X86_64_WINDOWS
                da_push(&link_flags, arena_sprintf(&arena, "%s.lib", libname));
#else
                da_push(&link_flags, "-l");
                da_push(&link_flags, libname);
#endif // PLATFORM_X86_64_WINDOWS
            } else {
                fprintf(stderr, "ERROR: Invalid flag '%s'\n\n", arg);
                usage(stderr, program);
                exit(1);
            }
        } else {
            if (input) {
                fprintf(stderr, "ERROR: Multiple input paths provided\n");
                if (run) {
                    fprintf(stderr, "Hint: When using '-r', pass program arguments after '--'\n");
                }
                exit(1);
            }

            input = arg;
        }
    }

    if (!input) {
        fprintf(stderr, "ERROR: Input path not provided\n\n");
        usage(stderr, program);
        exit(1);
    }

    Parser parser = {.arena = &arena};
    if (!parse_file(&parser, input)) {
        fprintf(stderr, "ERROR: Could not read file '%s'\n", input);
        exit(1);
    }

    if (!output_path) {
        output_path = temp_sv_to_cstr(sv_strip_suffix(sv_from_cstr(input), sv_from_cstr(".glos")));
        if (run) {
            const char *temp_path = get_temp_file_path();
            if (temp_path) {
                temp_paths_push(temp_path);
                output_path = temp_path;
            }
        }
    }

#ifdef PLATFORM_X86_64_WINDOWS
    if (!sv_has_suffix(sv_from_cstr(output_path), sv_from_cstr(".exe"))) {
        output_path = temp_sprintf("%s.exe", output_path);
    }
#endif // PLATFORM_X86_64_WINDOWS

    if (run) {
        temp_paths_push(output_path);
    }

    Compiler compiler = {
        .cmd = &cmd,
        .link_flags = &link_flags,

        .arena = &arena,
    };
    check_nodes(&compiler, parser.nodes);
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

    arena_free(&arena);
    cmd_free(&cmd);
    da_free(&link_flags);
    return result;
}
