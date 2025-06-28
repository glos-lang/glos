#include <unistd.h>

#include "checker.h"
#include "compiler.h"
#include "parser.h"

static void usage(FILE *file) {
    fprintf(file, "Usage:\n");
    fprintf(file, "    glos COMMAND [...]\n");
    fprintf(file, "\n");
    fprintf(file, "Commands:\n");
    fprintf(file, "    help                       Show this message\n");
    fprintf(file, "    run   [FILE]               Run the program\n");
    fprintf(file, "    build [FLAGS...] [FILE]    Compile the program\n");
    fprintf(file, "\n");
    fprintf(file, "Build Flags:\n");
    fprintf(file, "    -o OUTPUT                  Set the output path\n");
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
    Arena arena = {0};
    shift(&argc, &argv, "Program name");

    bool        run = false;
    const char *input = NULL;
    const char *output = NULL;
    const char *command = shift(&argc, &argv, "Command");
    if (!strcmp(command, "help")) {
        usage(stdout);
        exit(0);
    } else if (!strcmp(command, "run")) {
        run = true;
        input = shift(&argc, &argv, "Input file");
    } else if (!strcmp(command, "build")) {
        input = shift(&argc, &argv, "Input file");
        if (!strcmp(input, "-o")) {
            output = shift(&argc, &argv, "Output file");
            input = shift(&argc, &argv, "Input file");
        }
    } else {
        fprintf(stderr, "ERROR: Invalid command '%s'\n\n", command);
        usage(stderr);
        exit(1);
    }

    Lexer l = {0};
    if (!lexer_open(&l, input, &arena)) {
        fprintf(stderr, "ERROR: Could not read file '%s'\n", input);
        exit(1);
    }

    Parser p = {.arena = &arena};
    parse_file(&p, l);

    Context c = {0};
    check_nodes(&c, p.nodes);

    Cmd cmd = {0};
    if (run) {
        static char output[] = "/tmp/glos_run_XXXXXX";

        const int fd = mkstemp(output);
        if (fd < 0) {
            fprintf(stderr, "ERROR: Could not create temporary executable\n");
            exit(1);
        } else {
            close(fd);
            remove(output); // TODO: The production compiler need not do this
        }
        compile_nodes(&c, &cmd, output);

        da_push(&cmd, output);
        da_push_many(&cmd, argv, argc);

        result = cmd_run_sync(&cmd, (CmdStdio) {0});
        remove(output);
    } else {
        if (!output) {
            output = temp_sv_to_cstr(sv_strip_suffix(sv_from_cstr(input), sv_from_cstr(".glos")));
        }
        compile_nodes(&c, &cmd, output);
    }

    context_free(&c);
    arena_free(&arena);
    da_free(&cmd);
    return result;
}
