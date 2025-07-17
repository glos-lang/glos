#include <stdarg.h>
#include <stdio.h>

void glos_show_panic_message(const char *fmt, ...);

// TODO: Switch to 'panic'
void glos_show_panic_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fflush(stderr);
    fflush(stdout);
}
