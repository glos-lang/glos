#include "checker.h"
#include "compiler.h"
#include "parser.h"

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

int main(int argc, char **argv) {
    const char *program = shift(&argc, &argv, NULL, NULL);

    int   result = 0;
    Cmd   cmd = {0};
    Arena arena = {0};

    bool        run = false;
    const char *input = NULL;
    const char *output = NULL;
    while (argc) {
        const char *arg = shift(&argc, &argv, program, "Input path");
        if (*arg == '-') {
            if (!strcmp(arg, "-h")) {
                usage(stdout, program);
                exit(0);
            } else if (!strcmp(arg, "-r")) {
                run = true;
            } else if (!strcmp(arg, "-o")) {
                output = shift(&argc, &argv, program, "Output path");
            } else if (!strcmp(arg, "--")) {
                break;
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

    if (!output) {
        output = temp_sv_to_cstr(sv_strip_suffix(sv_from_cstr(input), sv_from_cstr(".glos")));
    }

#ifdef PLATFORM_X86_64_WINDOWS
    if (!sv_has_suffix(sv_from_cstr(output), sv_from_cstr(".exe"))) {
        output = temp_sprintf("%s.exe", output);
    }
#endif // PLATFORM_X86_64_WINDOWS

    Compiler compiler = {
        .cmd = &cmd,
        .llvm.arena = &arena,
        .path = input,
    };
    check_nodes(&compiler, parser.nodes);
    compiler_build(&compiler, output);

    if (run) {
        cmd.count = 0;
        cmd_push(&cmd, temp_sprintf("./%s", output));
        cmd_push_many(&cmd, argv, argc);
        // TODO: This makes the error message worse in case the process could not be started
        result = cmd_run_sync(&cmd, (Cmd_Stdio) {0});
        delete_file(output);
    }

    arena_free(&arena);
    cmd_free(&cmd);
    return result;
}
