#include <stdarg.h>
#include <unistd.h>

#include "message.h"

static bool is_terminal;
static bool is_terminal_checked;

static void check_is_terminal(void) {
    if (is_terminal_checked) {
        return;
    }

    is_terminal = isatty(STDERR_FILENO);
    if (getenv("INSIDE_EMACS")) {
        is_terminal = false;
    }
    is_terminal_checked = true;
}

void message_begin(MessageKind kind, Pos pos) {
    check_is_terminal();

    if (pos.path) {
        if (is_terminal) fprintf(stderr, "\033[1;4m");
        fprintf(stderr, "%s:%zu:%zu:", pos.path, pos.row + 1, pos.col + 1);
        if (is_terminal) fprintf(stderr, "\033[0m");
        fprintf(stderr, " ");
    }

    switch (kind) {
    case MESSAGE_NOTE:
        if (is_terminal) fprintf(stderr, "\033[1;33m");
        fprintf(stderr, "NOTE: ");
        if (is_terminal) fprintf(stderr, "\033[0m");
        break;

    case MESSAGE_ERROR:
        if (is_terminal) fprintf(stderr, "\033[1;31m");
        fprintf(stderr, "ERROR: ");
        if (is_terminal) fprintf(stderr, "\033[0m");
        break;
    }
}

void message_end(Pos pos, SV sv) {
    fprintf(stderr, "\n");
    if (pos.path) {
        check_is_terminal();
        fprintf(stderr, "\n    ");

        if (is_terminal) fprintf(stderr, "\033[1;36m");
        fprintf(stderr, "%zu | ", pos.row + 1);
        if (is_terminal) fprintf(stderr, "\033[0m");

        sv.data -= pos.col;
        for (size_t i = 0;; i++) {
            if (sv.data[i] == '\n' || sv.data[i] == '\0') {
                sv.count = i;
                break;
            }
        }

        fprintf(stderr, SVFmt "\n", SVArg(sv));

        const int count = snprintf(NULL, 0, "    %zu", pos.row + 1);
        assert(count >= 0);
        fprintf(stderr, "%*s", count, "");

        if (is_terminal) fprintf(stderr, "\033[1;36m");
        fprintf(stderr, " | ");
        if (is_terminal) fprintf(stderr, "\033[0m");

        for (size_t i = 0; i < pos.col; i++) {
            fputc(sv.data[i] == '\t' ? '\t' : ' ', stderr);
        }

        if (is_terminal) fprintf(stderr, "\033[1;35m");
        fprintf(stderr, "^\n");
        if (is_terminal) fprintf(stderr, "\033[0m");
    }
}

void message_full(MessageKind kind, Pos pos, SV sv, const char *fmt, ...) {
    message_begin(kind, pos);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    message_end(pos, sv);
}

void message_standalone(MessageKind kind, const char *fmt, ...) {
    message_begin(kind, (Pos) {0});
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}
