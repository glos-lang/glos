#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void glos_panic(const char *fmt, ...);

void glos_panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fflush(stderr);
    fflush(stdout);
    abort();
}
