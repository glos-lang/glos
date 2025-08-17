#include <unistd.h>

#include "checker.h"
#include "compiler.h"
#include "message.h"
#include "parser.h"

static void usage_flag(FILE *file, const char *flag, const char *rest) {
    write_message(file, MESSAGE_FG_MAGENTA, "    -%s", flag);
    fprintf(file, " %s\n", rest);
}

static void usage(FILE *file) {
    write_message(file, MESSAGE_ATTRIB_BOLD | MESSAGE_FG_CYAN, "Usage:\n");

    write_message(file, MESSAGE_ATTRIB_BOLD | MESSAGE_FG_GREEN, "    glos");
    fprintf(file, " [FLAGS...] FILE\n\n");

    write_message(file, MESSAGE_ATTRIB_BOLD | MESSAGE_FG_CYAN, "Flags:\n");
    usage_flag(file, "h", "          Show this message");
    usage_flag(file, "r", "          Run the program");
    usage_flag(file, "o", "OUTPUT    Set the output path");
    usage_flag(file, "L", "PATH      Add a library path");
    usage_flag(file, "l", "NAME      Add a library");
}

static const char *shift(int *argc, char ***argv, const char *expected) {
    if (*argc <= 0) {
        error_standalone(ERROR, "%s not provided\n", expected);
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
                error_standalone(ERROR, "Invalid flag '%s'\n", arg);
                usage(stderr);
                exit(1);
            }
        } else {
            if (input) {
                error_standalone(ERROR, "Multiple input files is not supported yet");
                exit(1);
            }

            input = arg;
        }
    }

    Lexer l = {0};
    if (!lexer_open(&l, input, &arena)) {
        error_standalone(ERROR, "Could not read file '%s'", input);
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
                error_standalone(ERROR, "Could not create temporary executable");
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
        error_standalone(ERROR, "Could not generate '%s'", output);
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
