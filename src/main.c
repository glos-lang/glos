#include "checker.h"
#include "compiler.h"
#include "parser.h"

static void usage(FILE *f) {
    fprintf(
        f,
        "Usage:\n"
        "    glos [FLAGS...] FILE\n"
        "\n"
        "Flags:\n"
        "    -h              Show this message\n"
        "    -r              Run the program\n"
        "    -o OUTPUT       Set the output path\n"
        "    --              End of compiler options. All following arguments are passed to the program if ran\n");
}

static const char *shift(int *argc, char ***argv, const char *expected) {
    if (*argc <= 0) {
        fprintf(stderr, "ERROR: %s not provided\n\n", expected);
        usage(stderr);
        exit(1);
    }

    (*argc)--;
    return *(*argv)++;
}

int main(int argc, char **argv) {
    int   result = 0;
    Cmd   cmd = {0};
    Arena arena = {0};
    shift(&argc, &argv, "Program name");

    bool        run = false;
    const char *input = NULL;
    const char *output = NULL;
    while (argc) {
        const char *arg = shift(&argc, &argv, "Input path");
        if (*arg == '-') {
            if (!strcmp(arg, "-h")) {
                usage(stdout);
                exit(0);
            } else if (!strcmp(arg, "-r")) {
                run = true;
            } else if (!strcmp(arg, "-o")) {
                output = shift(&argc, &argv, "Output path");
            } else if (!strcmp(arg, "--")) {
                break;
            } else {
                fprintf(stderr, "ERROR: Invalid flag '%s'\n\n", arg);
                usage(stderr);
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
        usage(stderr);
        exit(1);
    }

    Parser parser = {.arena = &arena};

#ifdef PROFILE
    const double profile_parse_begin = get_time();
#endif // PROFILE

    if (!parse_file(&parser, input)) {
        fprintf(stderr, "ERROR: Could not read file '%s'\n", input);
        exit(1);
    }

#ifdef PROFILE
    const double profile_parse_end = get_time();
    printf("[PROFILE] Parsing took %.7g seconds\n", profile_parse_end - profile_parse_begin);
#endif // PROFILE

    if (!output) {
        output = temp_sv_to_cstr(sv_strip_suffix(sv_from_cstr(input), sv_from_cstr(".glos")));
    }

#ifdef PLATFORM_X86_64_WINDOWS
    if (!sv_has_suffix(sv_from_cstr(output), sv_from_cstr(".exe"))) {
        output = temp_sprintf("%s.exe", output);
    }
#endif // PLATFORM_X86_64_WINDOWS

#ifdef PROFILE
    const double profile_check_begin = get_time();
#endif // PROFILE

    check_nodes(parser.nodes);

#ifdef PROFILE
    const double profile_check_end = get_time();
    printf("[PROFILE] Checking took %.7g seconds\n", profile_check_end - profile_check_begin);
#endif // PROFILE

    Compiler compiler = {
        .cmd = &cmd,
        .llvm.arena = &arena,
        .path = input,
    };
    compiler_build(&compiler, parser.nodes, output);

    if (run) {
        cmd.count = 0;
        cmd_push(&cmd, temp_sprintf("./%s", output));
        cmd_push_many(&cmd, argv, argc);

#ifdef PROFILE
        const double profile_run_begin = get_time();
#endif // PROFILE

        result = cmd_run_sync(&cmd, (CmdStdio) {0});

#ifdef PROFILE
        const double profile_run_end = get_time();
        printf("[PROFILE] Running executable took %.7g seconds\n", profile_run_end - profile_run_begin);
#endif // PROFILE

        delete_file(output);
    }

    arena_free(&arena);
    cmd_free(&cmd);
    return result;
}
