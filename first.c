#include "src/basic.h"
#include <ctype.h>

#ifdef PLATFORM_X86_64_WINDOWS
#define OBJ_FILE_EXTENSION ".obj"
#define EXE_FILE_EXTENSION ".exe"
#else
#define OBJ_FILE_EXTENSION ".o"
#define EXE_FILE_EXTENSION ""
#endif // PLATFORM_X86_64_WINDOWS

#define TESTS_LIST_PATH "tests/tests.conf"

static void usage(FILE *f, const char *program) {
    fprintf(
        f,
        "Usage:\n"
        "    %s [FLAGS...]\n"
        "\n"
        "Flags:\n"
        "    -h              Show this message\n"
        "    -t              Run tests\n"
        "    -T              Run tests in non-interactive mode\n"
        "    -j NPROCS       Set the maximum number of parallel processes. Default is 5\n",
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

static const char *replace_suffix(const char *path, const char *old, const char *new) {
    const SV base = sv_strip_suffix(sv_from_cstr(path), sv_from_cstr(old));
    return temp_sprintf(SV_Fmt "%s", SV_Arg(base), new);
}

static bool build_glos(Cmd *cmd, size_t nprocs) {
    static const char *headers[] = {
        "src/ast.h",
        "src/basic.h",
        "src/checker.h",
        "src/compiler.h",
        "src/context.h",
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
        "src/context.c",
        "src/lexer.c",
        "src/llvm.c",
        "src/main.c",
        "src/parser.c",
        "src/token.c",
    };

    bool        result = true;
    const void *save = temp_alloc(0);
    Procs       procs = {.nprocs = nprocs};

    size_t headers_time_latest = 0;
    for (size_t i = 0; i < len(headers); i++) {
        const size_t time = get_modified_time(headers[i]);
        headers_time_latest = max(headers_time_latest, time);
    }

    bool need_linking = get_modified_time("glos" EXE_FILE_EXTENSION) == 0;
    for (size_t i = 0; i < len(sources); i++) {
        const char  *src = sources[i];
        const char  *obj = replace_suffix(src, ".c", OBJ_FILE_EXTENSION);
        const size_t src_time = get_modified_time(src);
        const size_t obj_time = get_modified_time(obj);
        if (obj_time >= src_time && obj_time >= headers_time_latest) {
            continue;
        }

        fprintf(stderr, "Building '%s'\n", obj);
        need_linking = true;

        cmd_push(cmd, "clang");
        cmd_push(cmd, "-ggdb");
        cmd_push(cmd, "-c");
        cmd_push(cmd, "-o");
        cmd_push(cmd, obj);
        cmd_push(cmd, src);

        const Proc proc = cmd_run_async(cmd, (Cmd_Stdio) {0});
        if (proc == PROC_INVALID) {
            fprintf(stderr, "ERROR: Could not start process 'clang'\n");
            exit(1);
        }

        if (!procs_push(&procs, proc)) {
            fprintf(stderr, "ERROR: Process 'clang' exited abnormally\n");
            return_defer(false);
        }
    }

    if (!procs_flush(&procs)) {
        fprintf(stderr, "ERROR: Process 'clang' exited abnormally\n");
        return_defer(false);
    }

    if (!need_linking) {
        return_defer(true);
    }

    fprintf(stderr, "Building 'glos" EXE_FILE_EXTENSION "'\n");
    cmd_push(cmd, "clang");
    cmd_push(cmd, "-o");
    cmd_push(cmd, "glos" EXE_FILE_EXTENSION);
    for (size_t i = 0; i < len(sources); i++) {
        cmd_push(cmd, replace_suffix(sources[i], ".c", OBJ_FILE_EXTENSION));
    }

    if (cmd_run_sync(cmd, (Cmd_Stdio) {0})) {
        fprintf(stderr, "ERROR: Process 'clang' exited abnormally\n");
        return_defer(false);
    }

defer:
    da_free(&procs);
    temp_reset(save);
    return result;
}

static char single_char_prompt(FILE *in, FILE *out, const char *choices, const char **descriptions) {
    fprintf(out, " (");
    for (const char *p = choices, **d = descriptions; *p; p++, d++) {
        char it = *p;
        if (p == choices) {
            it = toupper(it);
        } else {
            it = tolower(it);
            fprintf(out, ", ");
        }
        fprintf(out, "%c: %s", it, *d);
    }
    fprintf(out, "): ");

    char buffer[16];
    if (fgets(buffer, sizeof(buffer), in) == NULL) {
        return false;
    }

    const size_t n = strlen(buffer);
    if (n > 0 && buffer[n - 1] == '\n') {
        buffer[n - 1] = '\0';
    }

    const char choice = tolower(*buffer);
    if (!choice) {
        return tolower(*choices);
    }

    if (strchr(choices, choice)) {
        return choice;
    }

    fprintf(stderr, "ERROR: Invalid choice '%c'\n", choice);
    return -1;
}

static void print_lines_with_indent(FILE *f, SV sv, const char *indent) {
    bool not_empty = sv.count != 0;
    if (not_empty) {
        fputs(" {\n", f);
    } else {
        fputs(" {}\n", f);
    }

    while (sv.count) {
        const SV line = sv_split_mut(&sv, '\n');
        fputs(indent, f);
        fwrite(line.data, line.count, 1, f);
        fputc('\n', f);
    }

    if (not_empty) {
        fputs("  }\n", f);
    }
}

static bool parse_uint_from_sv(SV s, size_t *n) {
    if (s.data == NULL || n == NULL || s.count == 0) {
        return false;
    }

    size_t result = 0;
    for (size_t i = 0; i < s.count; ++i) {
        const char it = s.data[i];
        if (!isdigit(it)) {
            return false;
        }

        const int digit = it - '0';
        if (result > (SIZE_MAX - digit) / 10) {
            return false;
        }

        result = result * 10 + digit;
    }

    *n = result;
    return true;
}

static size_t parse_uint_value(SV value, const char *label, const char *path, size_t row, SV line) {
    size_t n = 0;
    if (!parse_uint_from_sv(value, &n)) {
        fprintf(
            stderr,
            "%s:%zu:%zu: ERROR: Invalid %s '" SV_Fmt "'\n",
            path,
            row,
            value.data - line.data + 1,
            label,
            SV_Arg(value));

        exit(1);
    }

    return n;
}

static SV parse_bytes_value(SV value, SV *contents, const char *label, const char *path, size_t *row, SV line) {
    SV bytes = {0};

    const char *label_full = temp_sprintf("%s byte(s) count", label);
    bytes.count = parse_uint_value(value, label_full, path, (*row)++, line);
    temp_reset(label_full);

    if (bytes.count >= contents->count) {
        fprintf(
            stderr,
            "%s:%zu:1: ERROR: Expected %zu byte(s) and a newline, got %zu byte(s) instead\n",
            path,
            *row,
            bytes.count,
            contents->count);

        exit(1);
    }

    bytes.data = contents->data;
    for (size_t i = 0; i < bytes.count; i++) {
        if (bytes.data[i] == '\n') {
            (*row)++;
        }
    }

    sv_drop_mut(contents, bytes.count + 1);
    (*row)++;

    return bytes;
}

typedef struct {
    int exit;
    SV  out;
    SV  err;
} Test_Info;

static bool test_info_diff(Test_Info expected, Test_Info actual, const char *name) {
    const bool exit_mismatch = expected.exit != actual.exit;

    const bool stdout_mismatch = !sv_eq(expected.out, actual.out);
    const bool stderr_mismatch = !sv_eq(expected.err, actual.err);

    if (exit_mismatch || stdout_mismatch || stderr_mismatch) {
        fprintf(stderr, "\nERROR: Test case '%s' FAILED\n", name);

        if (exit_mismatch) {
            fprintf(stderr, "\n");
            fprintf(stderr, "Exit Code:\n");
            fprintf(stderr, "  Expected: %d\n", expected.exit);
            fprintf(stderr, "  Actual:   %d\n", actual.exit);
        }

        if (stdout_mismatch) {
            fprintf(stderr, "\n");
            fprintf(stderr, "Standard Output:\n");
            fprintf(stderr, "  Expected: %zu byte(s)", expected.out.count);
            print_lines_with_indent(stderr, expected.out, "    ");

            fprintf(stderr, "\n");
            fprintf(stderr, "  Actual:   %zu byte(s)", actual.out.count);
            print_lines_with_indent(stderr, actual.out, "    ");
        }

        if (stderr_mismatch) {
            fprintf(stderr, "\n");
            fprintf(stderr, "Standard Error:\n");
            fprintf(stderr, "  Expected: %zu byte(s)", expected.err.count);
            print_lines_with_indent(stderr, expected.err, "    ");

            fprintf(stderr, "\n");
            fprintf(stderr, "  Actual:   %zu byte(s)", actual.err.count);
            print_lines_with_indent(stderr, actual.err, "    ");
        }
        return false;
    } else {
        return true;
    }
}

typedef struct {
    const char *name;

    const char **args;
    size_t       args_count;

    bool        record_exists;
    const char *record_path;

    Proc  proc;
    FILE *pout;
    FILE *perr;

    Test_Info expected;
} Test;

typedef Dynamic_Array(Test) Tests;

static void test_prepare_cmd(Test test, Cmd *cmd) {
    cmd_push(cmd, "./glos" EXE_FILE_EXTENSION);
    cmd_push(cmd, "-r");
    cmd_push(cmd, test.name);
}

static bool tests_flush(Tests *tests, Cmd *cmd, bool interactive, Arena *arena, const void *arena_save) {
    size_t i = 0;
    while (i < tests->count) {
        Test *it = &tests->data[i];

        Test_Info actual = {0};
        if (it->pout) {
            if (!read_fp_into_arena(it->pout, &actual.out, arena)) {
                fprintf(stderr, "ERROR: Could not read standard output of test case '%s'\n", it->name);
                exit(1);
            }
            fclose(it->pout);
        } else {
            actual.out = (SV) {0};
        }

        if (it->perr) {
            if (!read_fp_into_arena(it->perr, &actual.err, arena)) {
                fprintf(stderr, "ERROR: Could not read standard error of test case '%s'\n", it->name);
                exit(1);
            }
            fclose(it->perr);
        } else {
            actual.err = (SV) {0};
        }
        actual.exit = cmd_wait(it->proc);

        bool need_to_record = false;
        if (it->record_exists) {
            if (!test_info_diff(it->expected, actual, it->name)) {
                if (!interactive) {
                    exit(1);
                }

                const char *descriptions[] = {
                    "Record",
                    "Skip",
                    "Rerun",
                    "Quit",
                };

                fprintf(stderr, "\nWhat to do for test case '%s'", it->name);
                const char choice = single_char_prompt(stdin, stderr, "ynrq", descriptions);
                if (choice == 'y') {
                    need_to_record = true;
                } else if (choice == 'r') {
                    if (actual.out.data) {
                        arena_reset(arena, actual.out.data);
                    } else if (actual.err.data) {
                        arena_reset(arena, actual.err.data);
                    }

                    fprintf(stderr, "Replaying '%s'\n", it->name);
                    test_prepare_cmd(*it, cmd);
                    it->proc = cmd_run_async(cmd, (Cmd_Stdio) {.out = &it->pout, .err = &it->perr});
                    continue;
                } else if (choice == 'q') {
                    return false;
                }
            }
        } else {
            need_to_record = true;
        }

        if (need_to_record) {
            FILE *f = fopen(it->record_path, "w");
            if (!f) {
                fprintf(stderr, "ERROR: Could not write file '%s'\n", it->record_path);
                exit(1);
            }

            fprintf(f, "EXIT %d\n", actual.exit);

            fprintf(f, "STDOUT %zu\n", actual.out.count);
            fwrite(actual.out.data, actual.out.count, 1, f);
            fprintf(f, "\n");

            fprintf(f, "STDERR %zu\n", actual.err.count);
            fwrite(actual.err.data, actual.err.count, 1, f);
            fprintf(f, "\n");

            fclose(f);
        }

        i++;
    }

    tests->count = 0;
    arena_reset(arena, arena_save);
    return true;
}

static bool build_test_object_file(Cmd *cmd, Procs *procs, const char *output, const char *input) {
    if (get_modified_time(output) < get_modified_time(input)) {
        fprintf(stderr, "Building '%s'\n", output);
        cmd_push(cmd, "clang");
        cmd_push(cmd, "-ggdb");
        cmd_push(cmd, "-c");
        cmd_push(cmd, "-o");
        cmd_push(cmd, output);
        cmd_push(cmd, input);

        const Proc proc = cmd_run_async(cmd, (Cmd_Stdio) {0});
        if (proc == PROC_INVALID) {
            fprintf(stderr, "ERROR: Could not start process 'clang'\n");
            return false;
        }

        if (!procs_push(procs, proc)) {
            fprintf(stderr, "ERROR: Process 'clang' exited abnormally\n");
            return false;
        }
    }

    return true;
}

static bool run_tests(Cmd *cmd, size_t nprocs, bool interactive) {
    bool        result = true;
    Tests       tests = {0};
    Arena       arena = {0};
    const char *temp_save = temp_alloc(0);

    // TODO: Generating .o files for all platforms for now. Later once strings are implemented, this can be set directly
    // in the source code, and need not be hardcoded in `tests.conf`
    //
    // Consider implementing a "subset" of strings. Basically just get strings working for extern blocks.
    {
        Procs procs = {.nprocs = nprocs};

        // ABI
        if (!build_test_object_file(cmd, &procs, "tests/abi/abi.o", "tests/abi/abi.c")) {
            return false;
        }

        if (!procs_flush(&procs)) {
            fprintf(stderr, "ERROR: Process 'clang' exited abnormally\n");
            return false;
        }

        da_free(&procs);
    }

    SV contents = {0};
    if (!read_file_into_arena(TESTS_LIST_PATH, &contents, &arena)) {
        fprintf(stderr, "ERROR: Could not read file '%s'\n", TESTS_LIST_PATH);
        return_defer(false);
    }

    const void *arena_save = arena_alloc(&arena, 0);
    while (contents.count) {
        SV line = sv_trim(sv_split(sv_split_mut(&contents, '\n'), '#'), ' ');
        if (line.count == 0) {
            continue;
        }

        Test test = {0};
        test.name = temp_sv_to_cstr(sv_split_mut(&line, ' '));
        test_prepare_cmd(test, cmd);

        // TODO: Proper shell parsing
        while (line.count) {
            line = sv_trim(line, ' ');
            da_push(cmd, temp_sv_to_cstr(sv_split_mut(&line, ' ')));
        }

        const char *record_path = replace_suffix(test.name, ".glos", ".bin");

        SV         contents = {0};
        const bool record_exists = read_file_into_arena(record_path, &contents, &arena);

        Test_Info expected = {0};
        if (record_exists) {
            for (size_t row = 1; contents.count; row++) {
                SV line = sv_trim(sv_split_mut(&contents, '\n'), ' ');
                SV key = sv_split_mut(&line, ' ');
                SV value = sv_trim(line, ' ');
                if (sv_match(key, "EXIT")) {
                    expected.exit = parse_uint_value(value, "exit code", record_path, row, line);
                } else if (sv_match(key, "STDOUT")) {
                    expected.out = parse_bytes_value(value, &contents, "standard output", record_path, &row, line);
                } else if (sv_match(key, "STDERR")) {
                    expected.err = parse_bytes_value(value, &contents, "standard error", record_path, &row, line);
                } else {
                    fprintf(stderr, "%s:%zu: ERROR: Invalid key '" SV_Fmt "'\n", record_path, row, SV_Arg(key));
                    exit(1);
                }
            }

            fprintf(stderr, "Replaying");
        } else {
            fprintf(stderr, "Recording");
        }

        for (size_t i = 2; i < cmd->count; i++) {
            fprintf(stderr, " %s", cmd->data[i]);
        }
        fprintf(stderr, "\n");

        test.record_exists = record_exists;
        test.record_path = record_path;

        test.proc = cmd_run_async(cmd, (Cmd_Stdio) {.out = &test.pout, .err = &test.perr});
        test.expected = expected;
        da_push(&tests, test);

        if (tests.count >= nprocs) {
            if (!tests_flush(&tests, cmd, interactive, &arena, arena_save)) {
                return_defer(true);
            }
        }
    }

    tests_flush(&tests, cmd, interactive, &arena, arena_save);

defer:
    da_free(&tests);
    arena_free(&arena);
    temp_reset(temp_save);
    return result;
}

int main(int argc, char **argv) {
    const char *program = shift(&argc, &argv, NULL, NULL);

    bool   tests = false;
    bool   interactive = true;
    size_t nprocs = 5;
    while (argc) {
        const char *arg = shift(&argc, &argv, program, "Input path");
        if (!strcmp(arg, "-h")) {
            usage(stdout, program);
            exit(0);
        } else if (!strcmp(arg, "-t")) {
            if (tests) {
                fprintf(stderr, "ERROR: Multiple test flags provided\n");
                exit(1);
            }

            tests = true;
        } else if (!strcmp(arg, "-T")) {
            if (tests) {
                fprintf(stderr, "ERROR: Multiple test flags provided\n");
                exit(1);
            }

            tests = true;
            interactive = false;
        } else if (arg[0] == '-' && arg[1] == 'j') {
            if (arg[2]) {
                arg += 2;
            } else {
                arg = shift(&argc, &argv, program, "Parallel process count");
            }

            if (!parse_uint_from_sv(sv_from_cstr(arg), &nprocs) || nprocs == 0) {
                fprintf(stderr, "ERROR: Invalid parallel process count '%s'\n", arg);
                exit(1);
            }
        } else {
            fprintf(stderr, "ERROR: Invalid flag '%s'\n\n", arg);
            usage(stderr, program);
            exit(1);
        }
    }

    Cmd cmd = {0};
    if (!build_glos(&cmd, nprocs)) {
        exit(1);
    }

    if (tests) {
        if (!run_tests(&cmd, nprocs, interactive)) {
            exit(1);
        }
    }

    da_free(&cmd);
    return 0;
}

#include "src/basic.c"

// TODO: Do we need to return `false` in these functions? Just exit...
