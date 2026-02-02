#ifndef BASIC_H
#define BASIC_H

// Platforms
#if defined(__x86_64__) && defined(__linux__)
#define PLATFORM_X86_64_LINUX
#elif defined(__x86_64__) && defined(_WIN64)
#define PLATFORM_X86_64_WINDOWS
#elif defined(__aarch64__) && defined(__APPLE__)
#define PLATFORM_ARM64_MACOS
#else
#error "Unsupported platform"
#endif

#ifdef PLATFORM_X86_64_WINDOWS
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif // PLATFORM_X86_64_WINDOWS

#include <assert.h>
#include <stdbool.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper Macros
#define len(a)    (sizeof(a) / sizeof(*(a)))
#define unused(v) (void) (v)

#define panic(...)     (fprintf(stderr, __VA_ARGS__), fflush(stdout), fflush(stderr), abort())
#define Printf_Like(n) __attribute__((format(printf, (n), (n) + 1)))

#ifdef unreachable
#undef unreachable
#endif

#define todo()        (panic("%s:%d: TODO\n", __FILE__, __LINE__))
#define unreachable() (panic("%s:%d: Unreachable\n", __FILE__, __LINE__))

#define return_defer(value)                                                                                            \
    do {                                                                                                               \
        result = (value);                                                                                              \
        goto defer;                                                                                                    \
    } while (0)

// Dynamic Array
#define DA_INIT_CAP 128

#define Dynamic_Array(T)                                                                                               \
    struct {                                                                                                           \
        T     *data;                                                                                                   \
        size_t count;                                                                                                  \
        size_t capacity;                                                                                               \
    }

#define da_free(l)                                                                                                     \
    do {                                                                                                               \
        free((l)->data);                                                                                               \
        memset((l), 0, sizeof(*(l)));                                                                                  \
    } while (0)

#define da_push(l, v)                                                                                                  \
    do {                                                                                                               \
        if ((l)->count >= (l)->capacity) {                                                                             \
            (l)->capacity = (l)->capacity == 0 ? DA_INIT_CAP : (l)->capacity * 2;                                      \
            (l)->data = realloc((l)->data, (l)->capacity * sizeof(*(l)->data));                                        \
            assert((l)->data);                                                                                         \
        }                                                                                                              \
                                                                                                                       \
        (l)->data[(l)->count++] = (v);                                                                                 \
    } while (0)

#define da_grow(l, c)                                                                                                  \
    do {                                                                                                               \
        if ((l)->count + (c) > (l)->capacity) {                                                                        \
            if ((l)->capacity == 0) {                                                                                  \
                (l)->capacity = DA_INIT_CAP;                                                                           \
            }                                                                                                          \
                                                                                                                       \
            while ((l)->count + (c) > (l)->capacity) {                                                                 \
                (l)->capacity *= 2;                                                                                    \
            }                                                                                                          \
                                                                                                                       \
            (l)->data = realloc((l)->data, (l)->capacity * sizeof(*(l)->data));                                        \
            assert((l)->data);                                                                                         \
        }                                                                                                              \
    } while (0)

#define da_push_many(l, v, c)                                                                                          \
    do {                                                                                                               \
        da_grow(l, c);                                                                                                 \
        memcpy((l)->data + (l)->count, (v), (c) * sizeof(*(l)->data));                                                 \
        (l)->count += (c);                                                                                             \
    } while (0)

// String View
typedef struct {
    const char *data;
    size_t      count;
} SV;

#define SV_Fmt    "%.*s"
#define SV_Arg(s) (int) ((s).count), ((s).data)

SV sv_from_cstr(const char *cstr);
SV sv_strip_suffix(SV a, SV b);

bool sv_eq(SV a, SV b);
bool sv_match(SV a, const char *b);
bool sv_has_suffix(SV a, SV b);
bool sv_find(SV s, char ch, size_t *index);

SV sv_trim(SV s, char ch);
SV sv_drop(SV *s, size_t count);
SV sv_split(SV *s, char ch);

// String Builder
typedef Dynamic_Array(char) SB;

#define sb_free      da_free
#define sb_grow      da_grow
#define sb_push      da_push
#define sb_push_many da_push_many

void sb_sprintf(SB *sb, const char *fmt, ...) Printf_Like(2);
void sb_push_cstr(SB *sb, const char *cstr);

// Temporary Allocator
void  temp_reset(const void *p);
void *temp_alloc(size_t n);
char *temp_sprintf(const char *fmt, ...) Printf_Like(1);
char *temp_sv_to_cstr(SV sv);

// Arena Allocator
typedef struct Arena_Region Arena_Region;

typedef struct {
    SB            sb;
    Arena_Region *head;
} Arena;

void  arena_free(Arena *a);
void *arena_alloc(Arena *a, size_t size);
void  arena_reset(Arena *a, const void *ptr);
void *arena_clone(Arena *a, const void *data, size_t size);

// FS
bool read_fp(FILE *f, SV *out, SB *sb);
bool read_fp_into_arena(FILE *f, SV *out, Arena *arena);

bool read_file(const char *path, SV *out, SB *sb);
bool read_file_into_arena(const char *path, SV *out, Arena *arena);

bool   delete_file(const char *path);
size_t get_modified_time(const char *path);

// Processes
typedef Dynamic_Array(const char *) Cmd;

#define cmd_free      da_free
#define cmd_grow      da_grow
#define cmd_push      da_push
#define cmd_push_many da_push_many

typedef struct {
    FILE **in;
    FILE **out;
    FILE **err;
} Cmd_Stdio;

#ifdef PLATFORM_X86_64_WINDOWS
typedef HANDLE Proc;
#define PROC_INVALID INVALID_HANDLE_VALUE
#else
typedef int Proc;
#define PROC_INVALID -1
#endif // PLATFORM_X86_64_WINDOWS

Proc cmd_run_async(Cmd *c, Cmd_Stdio stdio);
int  cmd_run_sync(Cmd *c, Cmd_Stdio stdio);
int  cmd_wait(Proc proc);

typedef struct {
    Proc  *data;
    size_t count;
    size_t capacity;
    size_t nprocs;
} Procs;

bool procs_push(Procs *ps, Proc p);
bool procs_flush(Procs *ps);

#endif // BASIC_H
