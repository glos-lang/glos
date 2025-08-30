#include <stdint.h>
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
    fprintf(file, " [FLAGS...] [FILE|DIR]\n\n");

    write_message(file, MESSAGE_ATTRIB_BOLD | MESSAGE_FG_CYAN, "Flags:\n");
    usage_flag(file, "h", "           Show this message");
    usage_flag(file, "r", "           Run the program");
    usage_flag(file, "o", "OUTPUT     Set the output path");
    usage_flag(file, "L", "PATH       Add a library path");
    usage_flag(file, "l", "NAME       Add a library");
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

static const char *path_last(const char *path) {
    const char *last = path + strlen(path) - 1;
    while (last > path && last[-1] != '/') {
        last--;
    }
    return last;
}

static const char *get_std_path(Arena *a) {
#if defined(__APPLE__)
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
#elif defined(__linux__)
    long  count = 0;
    char *data = NULL;

    for (size_t capacity = DA_INIT_CAP; true; capacity *= 2) {
        data = temp_alloc(capacity);
        count = readlink("/proc/self/exe", data, capacity);

        if (count == -1) {
            temp_reset(data);
            return NULL;
        }

        if ((size_t) count < capacity) {
            break;
        }

        temp_reset(data);
    }
#endif

    SV sv = {.data = data, .count = count};
    for (size_t i = sv.count; i; i--) {
        if (sv.data[i - 1] == '/') {
            sv.count = i;
            break;
        }
    }

    const char *path = arena_sprintf(a, SVFmt "std/", SVArg(sv));
    temp_reset(data);
    return path;
}

int main(int argc, char **argv) {
    int      result = 0;
    Arena    arena = {0};
    Packages packages = {0};

    Parser parser = {
        .arena = &arena,
        .packages = &packages,
    };

    Compiler compiler = {
        .context.arena = &arena,
        .context.packages = &packages,
    };

    bool        run = false;
    const char *input = NULL;
    const char *output = NULL;

    shift(&argc, &argv, "Program name");
    while (argc) {
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

                da_push(&compiler.link_flags, "-L");
                da_push(&compiler.link_flags, value);
            } else if (arg[1] == 'l') {
                const char *value = &arg[2];
                if (*value == '\0') {
                    value = shift(&argc, &argv, "Library name");
                }

                da_push(&compiler.link_flags, "-l");
                da_push(&compiler.link_flags, value);
            } else {
                error_standalone(ERROR, "Invalid flag '%s'\n", arg);
                usage(stderr);
                exit(1);
            }
        } else {
            if (input) {
                error_standalone(ERROR, "Multiple input paths provided");
                exit(1);
            }

            input = arg;
        }
    }

    parser.cwd = get_current_dir(parser.arena);
    parser.std = get_std_path(&arena);
    if (input) {
        input = get_relative_path(parser.cwd, input, &arena);
    } else {
        input = ".";
    }

    Package package = {
        .path = sv_from_cstr(input),
        .name.sv = sv_from_cstr("main"),
    };
    packages_push(&packages, &package);

    if (is_dir(input)) {
        parser.root = input;
    }

    ParseDirError pde = parse_dir(&parser, input, false);
    if (pde == PDE_EMPTY) {
        error_standalone(ERROR, "Directory '%s' does not contain any glos files", input);
        exit(1);
    }

    if (pde == PDE_FAILED && !parse_file(&parser, input)) {
        error_standalone(ERROR, "Could not read '%s'", input);
        exit(1);
    }

    compiler_init(&compiler);
    check_packages(&compiler, packages);

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
            if (is_dir(input)) {
                output = path_last(get_absolute_path(parser.cwd, input, &arena));
            } else {
                output = temp_sv_to_cstr(sv_strip_suffix(sv_from_cstr(path_last(input)), sv_from_cstr(".glos")));
            }
        }
    }

    if (is_dir(output)) {
        error_standalone(ERROR, "The output path '%s' exists and is a directory", output);
        exit(1);
    }

    compiler_build(&compiler, output);

    if (run) {
        da_push(&cmd, output);
        da_push_many(&cmd, argv, argc);
        result = cmd_run_sync(&cmd, (CmdStdio) {0});

        if (remove_after) {
            remove(output);
        }
    }

    parser_free(&parser);
    packages_free(&packages);
    arena_free(&arena);
    da_free(&cmd);
    return result;
}
