#ifndef BASIC_H
#define BASIC_H

#include <assert.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper Macros
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

#define len(a)    (sizeof(a) / sizeof(*(a)))
#define unused(v) (void) (v)

#define panic(...) (fprintf(stderr, __VA_ARGS__), fflush(stdout), fflush(stderr), abort())

// Some systems define unreachable() in stddef.h
#ifdef unreachable
#    undef unreachable
#endif

#define todo()        (panic("%s:%d: TODO\n", __FILE__, __LINE__))
#define unreachable() (panic("%s:%d: Unreachable\n", __FILE__, __LINE__))

// Dynamic Array
#define DA_INIT_CAP 128

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

#define SVFmt    "%.*s"
#define SVArg(s) (int) ((s).count), ((s).data)

bool sv_eq(SV a, SV b);
bool sv_match(SV a, const char *b);
bool sv_has_prefix(SV a, SV b);
bool sv_has_suffix(SV a, SV b);

SV sv_from_cstr(const char *cstr);
SV sv_strip_suffix(SV a, SV b);

SV sv_trim(SV s, char ch);
SV sv_drop(SV *s, size_t count);
SV sv_split(SV *s, char ch);

// Encoding
bool resolve_escape_char(char *ch);
void resolve_escape_chars(char *buffer, SV *sv);

// Temporary Allocator
#define PrintfLike(n) __attribute__((format(printf, (n), (n) + 1)))

void  temp_reset(const void *p);
void *temp_alloc(size_t n);
char *temp_sprintf(const char *fmt, ...) PrintfLike(1);
char *temp_sv_to_cstr(SV sv);
void  temp_remove_null(void);

// Arena Allocator
typedef struct ArenaRegion ArenaRegion;

typedef struct {
    ArenaRegion *head;
} Arena;

void  arena_free(Arena *a);
void *arena_alloc(Arena *a, size_t size);
void *arena_clone(Arena *a, const void *data, size_t size);
char *arena_sprintf(Arena *a, const char *fmt, ...) PrintfLike(2);

// FS
const char *get_current_dir(Arena *arena);
const char *get_relative_path(const char *cwd, const char *path, Arena *arena);
const char *get_absolute_path(const char *cwd, const char *path, Arena *arena);

bool read_file(SV *out, const char *path, Arena *arena);

typedef struct {
    const char **data;
    size_t       count;
    size_t       capacity;
} Paths;

bool is_dir(const char *path);
bool read_dir(Paths *p, const char *cwd, const char *path, SV suffix, Arena *arena);

// OS
typedef struct {
    const char **data;
    size_t       count;
    size_t       capacity;
} Cmd;

typedef struct {
    FILE **in;
    FILE **out;
    FILE **err;
} CmdStdio;

typedef int Proc;

Proc cmd_run_async(Cmd *c, CmdStdio stdio);
int  cmd_run_sync(Cmd *c, CmdStdio stdio);
int  cmd_wait(Proc proc);

#endif // BASIC_H
