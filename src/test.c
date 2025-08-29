#include <ctype.h>
#include <dirent.h>
#include <stdint.h>

#include "basic.h"

static const char *replace_extension(SV path, SV old, SV new) {
    SV base = sv_strip_suffix(path, old);
    base.count--;
    return temp_sprintf(SVFmt "." SVFmt, SVArg(base), SVArg(new));
}

static bool yes_or_no_prompt(const char *prompt) {
    fprintf(stderr, "\n%s (Y/n): ", prompt);

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

typedef struct {
    char  *data;
    size_t count;
    size_t capacity;
} SB;

static bool read_file_into_sb(FILE *f, SB *s) {
    const size_t count_save = s->count;

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
                s->count = count_save;
                return false;
            }
        }
    }
#undef READ_CHUNK_SIZE

    return true;
}

static bool read_path_into_sb(const char *path, SB *s) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    const bool result = read_file_into_sb(f, s);
    fclose(f);
    return result;
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
    SB    *sb;
    size_t start;
    size_t count;
} DynSV;

static DynSV sv_to_dyn_sv(SV sv, SB *sb) {
    DynSV dyn = {0};
    dyn.sb = sb;
    dyn.start = sv.data - sb->data;
    dyn.count = sv.count;
    return dyn;
}

static SV dyn_sv_to_sv(DynSV dyn) {
    return (SV) {.data = dyn.sb->data + dyn.start, .count = dyn.count};
}

typedef struct {
    int   exit;
    DynSV out;
    DynSV err;
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

static bool compare_tests(Test expected, Test actual) {
    const bool exit_mismatch = expected.exit != actual.exit;

    const SV expected_out = dyn_sv_to_sv(expected.out);
    const SV expected_err = dyn_sv_to_sv(expected.err);
    const SV actual_out = dyn_sv_to_sv(actual.out);
    const SV actual_err = dyn_sv_to_sv(actual.err);

    const bool stdout_mismatch = !sv_eq(expected_out, actual_out);
    const bool stderr_mismatch = !sv_eq(expected_err, actual_err);

    if (exit_mismatch || stdout_mismatch || stderr_mismatch) {
        fprintf(stderr, "FAIL\n");

        if (exit_mismatch) {
            fprintf(stderr, "\n");
            fprintf(stderr, "Exit Code:\n");
            fprintf(stderr, "  Expected: %d\n", expected.exit);
            fprintf(stderr, "  Actual:   %d\n", actual.exit);
        }

        if (stdout_mismatch) {
            fprintf(stderr, "\n");
            fprintf(stderr, "Standard Output:\n");
            fprintf(stderr, "  Expected: %zu byte(s)", expected_out.count);
            print_lines_with_indent(stderr, expected_out, "    ");

            fprintf(stderr, "\n");
            fprintf(stderr, "  Actual:   %zu byte(s)", actual_out.count);
            print_lines_with_indent(stderr, actual_out, "    ");
        }

        if (stderr_mismatch) {
            fprintf(stderr, "\n");
            fprintf(stderr, "Standard Error:\n");
            fprintf(stderr, "  Expected: %zu byte(s)", expected_err.count);
            print_lines_with_indent(stderr, expected_err, "    ");

            fprintf(stderr, "\n");
            fprintf(stderr, "  Actual:   %zu byte(s)", actual_err.count);
            print_lines_with_indent(stderr, actual_err, "    ");
        }
        return false;
    } else {
        fprintf(stderr, "PASS\n");
        return true;
    }
}

static void usage(FILE *f) {
    fprintf(f, "Usage:\n");
    fprintf(f, "    test [FLAGS...]\n");
    fprintf(f, "\n");
    fprintf(f, "Commands:\n");
    fprintf(f, "    -h             Show this message\n");
    fprintf(f, "    -check         Only check the programs, don't prompt for error recording\n");
    fprintf(f, "    -cc COMMAND    Set the command used for linking\n");
}

static const char *shift(int *argc, char ***argv, const char *expected) {
    if (*argc <= 0) {
        fprintf(stderr, "ERROR: %s not provided\n", expected);
        usage(stderr);
        exit(1);
    }

    (*argc)--;
    return *(*argv)++;
}

int main(int argc, char **argv) {
    bool        check = false;
    const char *cc = NULL;

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
        } else if (!strcmp(arg, "-cc")) {
            cc = shift(&argc, &argv, "Command");
        } else {
            fprintf(stderr, "ERROR: Invalid flag '%s'\n", arg);
            fprintf(stderr, "\n");
            usage(stderr);
            exit(1);
        }
    }

    Arena arena = {0};
    Paths paths = {0};
    if (!read_dir(&paths, get_current_dir(&arena), ".", sv_from_cstr("glos"), &arena)) {
        fprintf(stderr, "ERROR: Could not contents of current directory\n");
        exit(1);
    }

    SB  sb = {0};
    Cmd cmd = {0};
    for (size_t i = 0; i < paths.count; i++) {
        const char *it = paths.data[i];
        const char *record_path = replace_extension(sv_from_cstr(it), sv_from_cstr("glos"), sv_from_cstr("txt"));

        sb.count = 0;
        const bool record_exists = read_path_into_sb(record_path, &sb);

        Test expected = {0};
        if (record_exists) {
            SV contents = {sb.data, sb.count};
            for (size_t row = 1; contents.count; row++) {
                SV it = sv_split(&contents, '\n');
                SV line = sv_trim(it, ' ');
                SV key = sv_split(&line, ' ');
                SV value = sv_trim(line, ' ');
                if (sv_match(key, "EXIT")) {
                    expected.exit = parse_uint_value(value, "exit code", record_path, row, line);
                } else if (sv_match(key, "STDOUT")) {
                    expected.out = sv_to_dyn_sv(
                        parse_bytes_value(value, &contents, "standard output", record_path, &row, line), &sb);
                } else if (sv_match(key, "STDERR")) {
                    expected.err = sv_to_dyn_sv(
                        parse_bytes_value(value, &contents, "standard error", record_path, &row, line), &sb);
                } else {
                    fprintf(stderr, "%s:%zu: ERROR: Invalid key '" SVFmt "'\n", record_path, row, SVArg(key));
                    exit(1);
                }
            }

            fprintf(stderr, "Replaying '%s' ... ", it);
        } else {
            // TODO: Prompt the user for the CLI args
            fprintf(stderr, "Recording '%s' ... ", it);
        }

        da_push(&cmd, "../glos");

        if (cc) {
            da_push(&cmd, "-cc");
            da_push(&cmd, cc);
        }

        da_push(&cmd, "-r");
        da_push(&cmd, it);

        Test actual = {0};
        {
            FILE *stdout_pipe = NULL;
            FILE *stderr_pipe = NULL;
            actual.exit = cmd_run_sync(&cmd, (CmdStdio) {.out = &stdout_pipe, .err = &stderr_pipe});

            actual.out.sb = &sb;
            actual.out.start = sb.count;
            if (!read_file_into_sb(stdout_pipe, &sb)) {
                fprintf(stderr, "\nERROR: Could not read standard output of process\n");
                exit(1);
            }
            actual.out.count = sb.count - actual.out.start;

            actual.err.sb = &sb;
            actual.err.start = sb.count;
            if (!read_file_into_sb(stderr_pipe, &sb)) {
                fprintf(stderr, "\nERROR: Could not read standard error of process\n");
                exit(1);
            }
            actual.err.count = sb.count - actual.err.start;

            fclose(stdout_pipe);
            fclose(stderr_pipe);
        }

        bool need_to_record = false;
        if (record_exists) {
            if (!compare_tests(expected, actual)) {
                if (check) {
                    exit(1);
                }

                if (yes_or_no_prompt("Record new behavior")) {
                    need_to_record = true;
                }
            }
        } else {
            need_to_record = true;
        }

        if (need_to_record) {
            FILE *f = fopen(record_path, "wb");
            if (!f) {
                fprintf(stderr, "\nERROR: Could not write file '%s'\n", record_path);
                exit(1);
            }

            fprintf(f, "EXIT %d\n", actual.exit);

            fprintf(f, "STDOUT %zu\n", actual.out.count);
            const SV actual_out = dyn_sv_to_sv(actual.out);
            fwrite(actual_out.data, actual_out.count, 1, f);
            fprintf(f, "\n");

            fprintf(f, "STDERR %zu\n", actual.err.count);
            const SV actual_err = dyn_sv_to_sv(actual.err);
            fwrite(actual_err.data, actual_err.count, 1, f);
            fprintf(f, "\n");

            fclose(f);

            if (!record_exists) {
                fprintf(stderr, "OK\n");
            }
        }
    }

    da_free(&sb);
    da_free(&cmd);
    da_free(&paths);
    arena_free(&arena);
}
