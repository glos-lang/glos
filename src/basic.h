#ifndef BASIC_H
#define BASIC_H

// Platforms
#if defined(__x86_64__) && defined(__linux__)
#define PLATFORM_X86_64_LINUX
#elif (defined(__x86_64__) || defined(_M_X64)) && (defined(_WIN32) || defined(_WIN64))
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

#define OBJ_FILE_EXTENSION ".obj"
#define EXE_FILE_EXTENSION ".exe"
#else
#include <unistd.h>

#define OBJ_FILE_EXTENSION ".o"
#define EXE_FILE_EXTENSION ""

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

#define panic(...) (fprintf(stderr, __VA_ARGS__), fflush(stdout), fflush(stderr), abort())

#ifdef _MSC_VER
#define Printf_Like(n)
#else
#define Printf_Like(n) __attribute__((format(printf, (n), (n) + 1)))
#endif // _MSC_VER

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

#define da_grow(l, c) (da_resize((void **) &(l)->data, &(l)->capacity, sizeof(*(l)->data), c))
#define da_free(l)    (da_grow(l, 0), (l)->count = 0)
#define da_push(l, v) (da_grow(l, (l)->count + 1), (l)->data[(l)->count] = (v), (l)->count++)
#define da_push_many(l, v, c)                                                                                          \
    (da_grow(l, (l)->count + c), memcpy((l)->data + (l)->count, (v), (c) * sizeof(*(l)->data)), (l)->count += (c))

void da_resize(void **data, size_t *capacity, size_t size, size_t count);

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
SV sv_drop(SV s, size_t count);
SV sv_drop_mut(SV *s, size_t count);
SV sv_split(SV s, char ch);
SV sv_split_mut(SV *s, char ch);
SV sv_split_by(SV s, bool (*f)(char ch));
SV sv_split_by_mut(SV *s, bool (*f)(char ch));

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
void  temp_remove_null(void);

void *temp_clone(const void *data, size_t size);

// Arena Allocator
typedef struct Arena_Region Arena_Region;

typedef struct {
    SB            sb;
    Arena_Region *head;
} Arena;

void  arena_free(Arena *a);
void *arena_alloc(Arena *a, size_t size);
void  arena_reset(Arena *a, const void *ptr);
char *arena_sprintf(Arena *a, const char *fmt, ...) Printf_Like(2);

void *arena_clone(Arena *a, const void *data, size_t size);
void *arena_clone_from_temp(Arena *a, const void *p); // TODO: No more needed

// FS
bool read_fp(FILE *f, SV *out, SB *sb);
bool read_fp_into_arena(FILE *f, SV *out, Arena *arena);

bool read_file(const char *path, SV *out, SB *sb);
bool read_file_into_arena(const char *path, SV *out, Arena *arena);

bool delete_file(const char *path);
bool create_directory(const char *path);
bool directory_exists(const char *path);

size_t get_modified_time(const char *path);

bool is_cmd_available_in_path(const char *cmd);
bool is_lld_available_in_path(void);

const char *temp_replace_suffix(const char *path, const char *old, const char *new);

void temp_paths_push(const char *path);
void temp_paths_cleanup(void);

// Processes
typedef Dynamic_Array(const char *) Cmd;

#define cmd_free da_free
#define cmd_grow da_grow

#define cmd_push(cmd, ...) da_push_many(cmd, ((const char *[]) {__VA_ARGS__}), len(((const char *[]) {__VA_ARGS__})))
#define cmd_push_many      da_push_many

void cmd_show(Cmd cmd, FILE *f);

typedef struct {
    FILE **in;
    FILE **out;
    FILE **err;
} Cmd_Stdio;

#ifdef PLATFORM_X86_64_WINDOWS
typedef struct {
    HANDLE id;
    FILE  *in;
    FILE  *out;
    FILE  *err;
} Proc;

#define PROC_INVALID INVALID_HANDLE_VALUE
#else
typedef struct {
    int   id;
    FILE *in;
    FILE *out;
    FILE *err;
} Proc;

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
    void (*callback_before_wait)(Proc proc);
} Procs;

bool procs_push(Procs *ps, Proc p);
bool procs_flush(Procs *ps);

#endif // BASIC_H
