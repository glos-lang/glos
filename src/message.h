#ifndef MESSAGE_H
#define MESSAGE_H

#include "token.h"

typedef enum {
    MESSAGE_FG_DEFAULT,
    MESSAGE_FG_RED,
    MESSAGE_FG_GREEN,
    MESSAGE_FG_YELLOW,
    MESSAGE_FG_BLUE,
    MESSAGE_FG_MAGENTA,
    MESSAGE_FG_CYAN,
    MESSAGE_FG_WHITE,
    MESSAGE_FG_MASK = 0xFF,

    MESSAGE_ATTRIB_BOLD = 1 << 8,
    MESSAGE_ATTRIB_ITALIC = 1 << 9,
    MESSAGE_ATTRIB_UNDERLINE = 1 << 10,
} MessageAttrib;

void write_message(FILE *f, MessageAttrib attrib, const char *fmt, ...) PrintfLike(3);

#define print_message(...)  write_message(stdout, __VA_ARGS__)
#define eprint_message(...) write_message(stderr, __VA_ARGS__)

typedef enum {
    NOTE,
    ERROR,
} ErrorKind;

void error_begin(ErrorKind kind, Pos pos);
void error_end(Pos pos);

void error_full(ErrorKind kind, Pos pos, const char *fmt, ...) PrintfLike(3);
void error_standalone(ErrorKind kind, const char *fmt, ...) PrintfLike(2);

#endif // MESSAGE_H
