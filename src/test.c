#include <ctype.h>
#include <dirent.h>
#include <stdint.h>

#include "basic.h"

static const char *replace_extension(SV path, SV old, SV new) {
    SV base = sv_strip_suffix(path, old);
    base.count--;
    return temp_sprintf(SVFmt "." SVFmt, SVArg(base), SVArg(new));
}

static bool yes_or_no_prompt(void) {
    char buffer[16];
    if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
        return false;
    }

    const size_t n = strlen(buffer);
    if (n > 0 && buffer[n - 1] == '\n') {
        buffer[n - 1] = '\0';
    }

    return buffer[0] == '\0' || buffer[0] == 'y' || buffer[0] == 'Y';
}

typedef struct {
    char  *data;
    size_t count;
    size_t capacity;
} SB;

static bool read_file_into_arena(FILE *f, SV *out, SB *s, Arena *a) {
    const size_t start = s->count;

#define READ_CHUNK_SIZE 4096
    while (true) {
        da_grow(s, READ_CHUNK_SIZE);
        const size_t n = fread(s->data + s->count, 1, READ_CHUNK_SIZE, f);
        s->count += n;

        if (n < READ_CHUNK_SIZE) {
            if (feof(f)) {
                break;
            }

            if (ferror(f)) {
                s->count = start;
                return false;
            }
        }
    }
#undef READ_CHUNK_SIZE

    out->count = s->count - start;
    out->data = arena_clone(a, s->data + start, out->count);

    s->count = start;
    return true;
}

static bool read_path_into_arena(const char *path, SV *out, SB *s, Arena *arena) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    const bool result = read_file_into_arena(f, out, s, arena);
    fclose(f);
    return result;
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
            "%s:%zu:%zu: ERROR: Invalid %s '" SVFmt "'\n",
            path,
            row,
            value.data - line.data + 1,
            label,
            SVArg(value));

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

    sv_drop(contents, bytes.count + 1);
    (*row)++;

    return bytes;
}

typedef struct {
    int exit;
    SV  out;
    SV  err;
} Test;

static void print_lines_with_indent(FILE *f, SV sv, const char *indent) {
    bool not_empty = sv.count != 0;
    if (not_empty) {
        fputs(" {\n", f);
    } else {
        fputs(" {}\n", f);
    }

    while (sv.count) {
        const SV line = sv_split(&sv, '\n');
        fputs(indent, f);
        fwrite(line.data, line.count, 1, f);
        fputc('\n', f);
    }

    if (not_empty) {
        fputs("  }\n", f);
    }
}

static bool compare_tests(Test expected, Test actual, const char *name) {
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

static void usage(FILE *f) {
    fprintf(f, "Usage:\n");
    fprintf(f, "    test [FLAGS...]\n");
    fprintf(f, "\n");
    fprintf(f, "Commands:\n");
    fprintf(f, "    -h          Show this message\n");
    fprintf(f, "    -j COUNT    Set the parallel processes count. Default is 5\n");
    fprintf(f, "    -check      Only check the programs, don't prompt for error recording\n");
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
    const char *name;

    bool        record_exists;
    const char *record_path;

    Proc  proc;
    FILE *pout;
    FILE *perr;

    Test expected;
} Unit;

typedef struct {
    Unit  *data;
    size_t count;
    size_t capacity;
    size_t maximum;
} Units;

static void flush_units(Units *units, SB *sb, Arena *arena, bool check) {
    for (size_t i = 0; i < units->count; i++) {
        Unit it = units->data[i];

        Test actual = {0};
        actual.exit = cmd_wait(it.proc);

        if (!read_file_into_arena(it.pout, &actual.out, sb, arena)) {
            fprintf(stderr, "ERROR: Could not read standard output of test case '%s'\n", it.name);
            exit(1);
        }

        if (!read_file_into_arena(it.perr, &actual.err, sb, arena)) {
            fprintf(stderr, "ERROR: Could not read standard error of test case '%s'\n", it.name);
            exit(1);
        }

        fclose(it.pout);
        fclose(it.perr);

        bool need_to_record = false;
        if (it.record_exists) {
            if (!compare_tests(it.expected, actual, it.name)) {
                if (check) {
                    exit(1);
                }

                fprintf(stderr, "\nRecord new behavior for test case '%s' (Y/n): ", it.name);
                if (yes_or_no_prompt()) {
                    need_to_record = true;
                }
            }
        } else {
            need_to_record = true;
        }

        if (need_to_record) {
            FILE *f = fopen(it.record_path, "wb");
            if (!f) {
                fprintf(stderr, "ERROR: Could not write file '%s'\n", it.record_path);
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

            if (!it.record_exists) {
                fprintf(stderr, "Recorded test case '%s'\n", it.name);
            }
        }
    }

    units->count = 0;
    arena_free(arena);
}

int main(int argc, char **argv) {
    bool  check = false;
    Units units = {.maximum = 5};

    shift(&argc, &argv, "Program name");
    while (argc) {
        const char *arg = shift(&argc, &argv, "Argument");
        if (arg[0] != '-') {
            break;
        }

        if (!strcmp(arg, "-h")) {
            usage(stdout);
            exit(0);
        } else if (!strcmp(arg, "-check")) {
            check = true;
        } else if (arg[1] == 'j') {
            if (arg[2]) {
                arg += 2;
            } else {
                arg = shift(&argc, &argv, "Parallel process count");
            }

            if (!parse_uint_from_sv(sv_from_cstr(arg), &units.maximum) || !units.maximum) {
                fprintf(stderr, "ERROR: Invalid parallel process count '%s'\n", arg);
                exit(1);
            }
        } else {
            fprintf(stderr, "ERROR: Invalid flag '%s'\n", arg);
            fprintf(stderr, "\n");
            usage(stderr);
            exit(1);
        }
    }

    Paths paths = {0};
    Arena paths_arena = {0};
    if (!read_dir(&paths, get_current_dir(&paths_arena), ".", sv_from_cstr("glos"), &paths_arena)) {
        fprintf(stderr, "ERROR: Could not contents of current directory\n");
        exit(1);
    }

    SB    sb = {0};
    Cmd   cmd = {0};
    Arena arena = {0};
    for (size_t i = 0; i < paths.count; i++) {
        const char *it = paths.data[i];
        const char *record_path = replace_extension(sv_from_cstr(it), sv_from_cstr("glos"), sv_from_cstr("txt"));

        SV         contents = {0};
        const bool record_exists = read_path_into_arena(record_path, &contents, &sb, &arena);

        {
            Test expected = {0};
            if (record_exists) {
                for (size_t row = 1; contents.count; row++) {
                    SV it = sv_split(&contents, '\n');
                    SV line = sv_trim(it, ' ');
                    SV key = sv_split(&line, ' ');
                    SV value = sv_trim(line, ' ');
                    if (sv_match(key, "EXIT")) {
                        expected.exit = parse_uint_value(value, "exit code", record_path, row, line);
                    } else if (sv_match(key, "STDOUT")) {
                        expected.out = parse_bytes_value(value, &contents, "standard output", record_path, &row, line);
                    } else if (sv_match(key, "STDERR")) {
                        expected.err = parse_bytes_value(value, &contents, "standard error", record_path, &row, line);
                    } else {
                        fprintf(stderr, "%s:%zu: ERROR: Invalid key '" SVFmt "'\n", record_path, row, SVArg(key));
                        exit(1);
                    }
                }

                fprintf(stderr, "Replaying '%s'\n", it);
            } else {
                // TODO: Prompt the user for the CLI args
                fprintf(stderr, "Recording '%s'\n", it);
            }

            da_push(&cmd, "../glos");
            da_push(&cmd, "-r");
            da_push(&cmd, it);

            Unit unit = {0};
            unit.name = it;

            unit.record_exists = record_exists;
            unit.record_path = record_path;

            unit.proc = cmd_run_async(&cmd, (CmdStdio) {.out = &unit.pout, .err = &unit.perr});
            unit.expected = expected;
            da_push(&units, unit);

            if (units.count < units.maximum) {
                continue;
            }
        }

        flush_units(&units, &sb, &arena, check);
    }

    flush_units(&units, &sb, &arena, check);

    da_free(&sb);
    da_free(&cmd);
    da_free(&units);

    da_free(&paths);
    arena_free(&paths_arena);
}
