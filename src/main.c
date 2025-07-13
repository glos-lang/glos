#include <unistd.h>

#include "checker.h"
#include "compiler.h"
#include "parser.h"

static void usage(FILE *file) {
    fprintf(file, "Usage:\n");
    fprintf(file, "    glos [FLAGS...] FILE\n");
    fprintf(file, "\n");
    fprintf(file, "Flags:\n");
    fprintf(file, "    -h           Show this message\n");
    fprintf(file, "    -r           Run the program\n");
    fprintf(file, "    -o OUTPUT    Set the output path\n");
    fprintf(file, "    -L PATH      Add a library path\n");
    fprintf(file, "    -l NAME      Add a library\n");
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

typedef struct {
    const char **data;
    size_t       count;
    size_t       capacity;
} Flags;

int main(int argc, char **argv) {
    int   result = 0;
    Arena arena = {0};

    bool        run = false;
    Flags       flags = {0};
    const char *input = NULL;
    const char *output = NULL;

    shift(&argc, &argv, "Program name");
    while (!input || argc) {
        const char *arg = shift(&argc, &argv, "Input file");
        if (arg[0] == '-') {
            if (!strcmp(arg, "-h")) {
                usage(stdout);
                exit(0);
            } else if (!strcmp(arg, "-r")) {
                run = true;
            } else if (!strcmp(arg, "-o")) {
                output = shift(&argc, &argv, "Output file");
            } else if (!strcmp(arg, "--")) {
                break;
            } else if (arg[1] == 'L') {
                const char *value = &arg[2];
                if (*value == '\0') {
                    value = shift(&argc, &argv, "Library path");
                }

                da_push(&flags, "-L");
                da_push(&flags, value);
            } else if (arg[1] == 'l') {
                const char *value = &arg[2];
                if (*value == '\0') {
                    value = shift(&argc, &argv, "Library name");
                }

                da_push(&flags, "-l");
                da_push(&flags, value);
            } else {
                fprintf(stderr, "ERROR: Invalid flag '%s'\n\n", arg);
                usage(stderr);
                exit(1);
            }
        } else {
            if (input) {
                fprintf(stderr, "ERROR: Multiple input files is not supported yet\n");
                exit(1);
            }

            input = arg;
        }
    }

    Lexer l = {0};
    if (!lexer_open(&l, input, &arena)) {
        fprintf(stderr, "ERROR: Could not read file '%s'\n", input);
        exit(1);
    }

    Parser p = {.arena = &arena};
    parse_file(&p, l);

    Compiler c = {.context.arena = &arena};
    compiler_init(&c);
    check_nodes(&c, p.nodes);

    Cmd  cmd = {0};
    bool remove_after = false;
    if (run) {
        if (output) {
            if (!strchr(output, '/')) {
                output = temp_sprintf("./%s", output);
            }
        } else {
            static char buffer[] = "/tmp/glos_run_XXXXXX";

            const int fd = mkstemp(buffer);
            if (fd < 0) {
                fprintf(stderr, "ERROR: Could not create temporary executable\n");
                exit(1);
            } else {
                close(fd);
                remove(buffer); // TODO: The production compiler need not do this
            }

            output = buffer;
            remove_after = true;
        }
    } else {
        if (!output) {
            output = temp_sv_to_cstr(sv_strip_suffix(sv_from_cstr(input), sv_from_cstr(".glos")));
        }
    }

    const char *object_file_path = temp_sprintf("%s.o", output);
    compiler_build(&c, object_file_path);

    da_push(&cmd, "cc");
    da_push(&cmd, "-o");
    da_push(&cmd, output);
    da_push(&cmd, object_file_path);
    da_push_many(&cmd, flags.data, flags.count);
    if (cmd_run_sync(&cmd, (CmdStdio) {0})) {
        fprintf(stderr, "ERROR: Could not generate '%s'\n", output);
        exit(1);
    }
    remove(object_file_path);

    if (run) {
        da_push(&cmd, output);
        da_push_many(&cmd, argv, argc);
        result = cmd_run_sync(&cmd, (CmdStdio) {0});

        if (remove_after) {
            remove(output);
        }
    }

    arena_free(&arena);
    da_free(&flags);
    da_free(&cmd);
    return result;
}
