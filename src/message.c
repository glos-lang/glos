#include <stdarg.h>
#include <unistd.h>

#include "message.h"

void write_message(FILE *f, MessageAttrib attrib, const char *fmt, ...) {
    static bool is_emacs;
    static bool is_emacs_checked;
    if (!is_emacs_checked) {
        is_emacs = getenv("INSIDE_EMACS");
        is_emacs_checked = true;
    }

    bool is_terminal = is_emacs ? false : isatty(fileno(f));
    if (is_terminal) {
        fprintf(f, "\033[0");

        if (attrib & MESSAGE_ATTRIB_BOLD) {
            fprintf(f, ";1");
        }

        if (attrib & MESSAGE_ATTRIB_ITALIC) {
            fprintf(f, ";3");
        }

        if (attrib & MESSAGE_ATTRIB_UNDERLINE) {
            fprintf(f, ";4");
        }

        switch (attrib & MESSAGE_FG_MASK) {
        case MESSAGE_FG_RED:
            fprintf(f, ";31");
            break;

        case MESSAGE_FG_GREEN:
            fprintf(f, ";32");
            break;

        case MESSAGE_FG_YELLOW:
            fprintf(f, ";33");
            break;

        case MESSAGE_FG_BLUE:
            fprintf(f, ";34");
            break;

        case MESSAGE_FG_MAGENTA:
            fprintf(f, ";35");
            break;

        case MESSAGE_FG_CYAN:
            fprintf(f, ";36");
            break;

        case MESSAGE_FG_WHITE:
            fprintf(f, ";37");
            break;

        case MESSAGE_FG_DEFAULT:
            break;

        default:
            unreachable();
        }

        fprintf(f, "m");
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    if (is_terminal) {
        fprintf(f, "\033[0m");
    }
}

void error_begin(ErrorKind kind, Pos pos) {
    if (pos.path) {
        eprint_message(MESSAGE_ATTRIB_BOLD | MESSAGE_ATTRIB_UNDERLINE, PosFmt, PosArg(pos));
        fprintf(stderr, " ");
    }

    switch (kind) {
    case NOTE:
        eprint_message(MESSAGE_ATTRIB_BOLD | MESSAGE_FG_YELLOW, "NOTE: ");
        break;

    case ERROR:
        eprint_message(MESSAGE_ATTRIB_BOLD | MESSAGE_FG_RED, "ERROR: ");
        break;
    }
}

void error_end(Pos pos) {
    fprintf(stderr, "\n");
    if (pos.path) {
        fprintf(stderr, "\n    ");

        eprint_message(MESSAGE_ATTRIB_BOLD | MESSAGE_FG_CYAN, "%zu | ", pos.row + 1);
        fprintf(stderr, SVFmt "\n", SVArg(pos.line));

        const int count = snprintf(NULL, 0, "    %zu", pos.row + 1);
        assert(count >= 0);
        fprintf(stderr, "%*s", count, "");

        eprint_message(MESSAGE_ATTRIB_BOLD | MESSAGE_FG_CYAN, " | ");
        for (size_t i = 0; i < pos.col; i++) {
            fputc(pos.line.data[i] == '\t' ? '\t' : ' ', stderr);
        }

        eprint_message(MESSAGE_ATTRIB_BOLD | MESSAGE_FG_MAGENTA, "^\n");
    }
}

void error_full(ErrorKind kind, Pos pos, const char *fmt, ...) {
    error_begin(kind, pos);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    error_end(pos);
}

void error_standalone(ErrorKind kind, const char *fmt, ...) {
    error_begin(kind, (Pos) {0});
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}
