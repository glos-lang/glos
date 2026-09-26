#include <fcntl.h>
#include <stdio.h>

#ifndef O_TEXT
#define O_TEXT 0
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

int main(void) {
    printf("RDWR   = %d\n", O_RDWR);
    printf("RDONLY = %d\n", O_RDONLY);
    printf("WRONLY = %d\n", O_WRONLY);
    printf("CREAT  = %d\n", O_CREAT);
    printf("TRUNC  = %d\n", O_TRUNC);
    printf("APPEND = %d\n", O_APPEND);
    printf("EXCL   = %d\n", O_EXCL);
    printf("TEXT   = %d\n", O_TEXT);
    printf("BINARY = %d\n", O_BINARY);
}
