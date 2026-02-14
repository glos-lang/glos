#include "basic.h"
#include <fcntl.h>
#include <stdarg.h>

#ifdef PLATFORM_X86_64_WINDOWS
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif // PLATFORM_X86_64_WINDOWS

// Dynamic Array
void da_resize(void **data, size_t *capacity, size_t size, size_t count) {
    if (!count) {
        free(*data);
        *data = NULL;
        *capacity = 0;
        return;
    }

    if (count > *capacity) {
        if (*capacity == 0) {
            *capacity = DA_INIT_CAP;
        }

        while (count > *capacity) {
            *capacity *= 2;
        }

        *data = realloc(*data, *capacity * size);
        assert(data);
    }
}

// String View
SV sv_from_cstr(const char *cstr) {
    return (SV) {.data = cstr, .count = strlen(cstr)};
}

SV sv_strip_suffix(SV a, SV b) {
    while (sv_has_suffix(a, b)) {
        a.count -= b.count;
    }
    return a;
}

bool sv_eq(SV a, SV b) {
    return a.count == b.count && memcmp(a.data, b.data, b.count) == 0;
}

bool sv_match(SV a, const char *b) {
    return a.count == strlen(b) && memcmp(b, a.data, a.count) == 0;
}

bool sv_has_suffix(SV a, SV b) {
    if (b.count == 0) {
        return true;
    }

    return a.count >= b.count && memcmp(&a.data[a.count - b.count], b.data, b.count) == 0;
}

bool sv_find(SV s, char ch, size_t *index) {
    const char *p = memchr(s.data, ch, s.count);
    if (!p) {
        return false;
    }

    if (index) {
        *index = p - s.data;
    }
    return true;
}

SV sv_trim(SV s, char ch) {
    while (s.count && *s.data == ch) {
        s.data++;
        s.count--;
    }

    while (s.count && s.data[s.count - 1] == ch) {
        s.count--;
    }

    return s;
}

SV sv_drop(SV s, size_t count) {
    return sv_drop_mut(&s, count);
}

SV sv_drop_mut(SV *s, size_t count) {
    const SV result = (SV) {.data = s->data, .count = count};
    s->data += count;
    s->count -= count;
    return result;
}

SV sv_split(SV s, char ch) {
    return sv_split_mut(&s, ch);
}

SV sv_split_mut(SV *s, char ch) {
    const char *p = memchr(s->data, ch, s->count);
    if (!p) {
        const SV result = *s;
        s->data += s->count;
        s->count = 0;
        return result;
    }

    const SV result = (SV) {.data = s->data, .count = p - s->data};
    s->data = p + 1;
    s->count -= result.count + 1;
    return result;
}

// String Builder
void sb_sprintf(SB *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    sb_grow(sb, sb->count + n + 1);

    va_start(args, fmt);
    vsnprintf(sb->data + sb->count, n + 1, fmt, args);
    sb->count += n;
    va_end(args);
}

void sb_push_cstr(SB *sb, const char *cstr) {
    const size_t n = strlen(cstr);
    sb_push_many(sb, cstr, n);
}

// Temporary Allocator
static char   temp_data[16 * 1024 * 1024];
static size_t temp_count;

void temp_reset(const void *p) {
    assert((const char *) p >= temp_data && (const char *) p <= temp_data + temp_count);
    temp_count = (const char *) p - temp_data;
}

void *temp_alloc(size_t n) {
    assert(temp_count + n <= len(temp_data));
    char *result = &temp_data[temp_count];
    temp_count += n;
    return result;
}

char *temp_sprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    char *result = temp_alloc(n + 1);

    va_start(args, fmt);
    vsnprintf(result, n + 1, fmt, args);
    va_end(args);

    return result;
}

char *temp_sv_to_cstr(SV sv) {
    char *p = memcpy(temp_alloc(sv.count + 1), sv.data, sv.count);
    p[sv.count] = '\0';
    return p;
}

void temp_remove_null(void) {
    if (temp_count && temp_data[temp_count - 1] == '\0') {
        temp_count--;
    }
}

// Arena Allocator
#define ARENA_MINIMUM_CAPACITY 16000

struct Arena_Region {
    Arena_Region *next;
    size_t        count;
    size_t        capacity;
    char          data[];
};

void arena_free(Arena *a) {
    Arena_Region *it = a->head;
    while (it) {
        Arena_Region *next = it->next;
        free(it);
        it = next;
    }
    sb_free(&a->sb);
    memset(a, 0, sizeof(*a));
}

void *arena_alloc(Arena *a, size_t size) {
    Arena_Region *region = NULL;
    for (Arena_Region *it = a->head; it; it = it->next) {
        if (it->count + size <= it->capacity) {
            region = it;
            break;
        }
    }

    size = (size + 7) & -8; // Alignment
    if (!region) {
        size_t capacity = size;
        if (capacity < ARENA_MINIMUM_CAPACITY) {
            capacity = ARENA_MINIMUM_CAPACITY;
        }

        region = malloc(sizeof(Arena_Region) + capacity);
        region->next = a->head;
        region->count = 0;
        region->capacity = capacity;
        a->head = region;
    }

    void *ptr = &region->data[region->count];
    if (size) {
        memset(ptr, 0, size);
    }
    region->count += size;
    return ptr;
}

void arena_reset(Arena *a, const void *ptr) {
    for (Arena_Region *it = a->head; it; it = it->next) {
        if ((const char *) ptr >= it->data && (const char *) ptr <= it->data + it->capacity) {
            it->count = (const char *) ptr - it->data;
            for (Arena_Region *p = a->head; p != it;) {
                Arena_Region *next = p->next;
                free(p);
                p = next;
            }
            a->head = it;
            return;
        }
    }

    unreachable();
}

void *arena_clone(Arena *a, const void *data, size_t size) {
    return memcpy(arena_alloc(a, size), data, size);
}

char *arena_sprintf(Arena *a, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    char *result = arena_alloc(a, n + 1);

    va_start(args, fmt);
    vsnprintf(result, n + 1, fmt, args);
    va_end(args);

    return result;
}

// FS
bool read_fp(FILE *f, SV *out, SB *sb) {
    bool         result = true;
    const size_t start = sb->count;

    if (!f) {
        return_defer(false);
    }

    while (true) {
#define CHUNK_SIZE 4096
        sb_grow(sb, sb->count + CHUNK_SIZE);
        const size_t n = fread(sb->data + sb->count, sizeof(*sb->data), CHUNK_SIZE, f);
        sb->count += n;

        if (n < CHUNK_SIZE) {
            if (feof(f)) {
                break;
            }

            if (ferror(f)) {
                return_defer(false);
            }
        }
#undef CHUNK_SIZE
    }

    size_t j = start;
    for (size_t i = start; i < sb->count; i++) {
        char it = sb->data[i];
        if (it == '\r' && i + 1 < sb->count && sb->data[i + 1] == '\n') {
            it = sb->data[++i];
        }
        sb->data[j++] = it;
    }
    sb->count = j;

    out->data = sb->data + start;
    out->count = sb->count - start;

defer:
    if (!result) {
        sb->count = start;
    }
    return result;
}

bool read_fp_into_arena(FILE *f, SV *out, Arena *arena) {
    if (!read_fp(f, out, &arena->sb)) {
        return false;
    }

    out->data = arena_clone(arena, out->data, out->count);
    arena->sb.count -= out->count;
    return true;
}

bool read_file(const char *path, SV *out, SB *sb) {
    bool result = true;

    FILE *f = fopen(path, "r");
    if (!f) {
        return_defer(false);
    }

    if (!read_fp(f, out, sb)) {
        return_defer(false);
    }

defer:
    if (f) {
        fclose(f);
    }

    return result;
}

bool read_file_into_arena(const char *path, SV *out, Arena *arena) {
    if (!read_file(path, out, &arena->sb)) {
        return false;
    }

    out->data = arena_clone(arena, out->data, out->count);
    arena->sb.count -= out->count;
    return true;
}

bool delete_file(const char *path) {
#ifdef PLATFORM_X86_64_WINDOWS
    return DeleteFileA(path);
#else
    return unlink(path) == 0;
#endif // PLATFORM_X86_64_WINDOWS
}

size_t get_modified_time(const char *path) {
#ifdef PLATFORM_X86_64_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        return 0;
    }

    ULARGE_INTEGER ft;
    ft.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return ft.QuadPart;

#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }

    return st.st_mtime;
#endif // PLATFORM_X86_64_WINDOWS
}

// Processes
Proc cmd_run_async(Cmd *c, Cmd_Stdio stdio) {
#ifdef PLATFORM_X86_64_WINDOWS
    STARTUPINFOA siStartInfo;
    ZeroMemory(&siStartInfo, sizeof(siStartInfo));
    siStartInfo.cb = sizeof(STARTUPINFO);

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE stdout_pipe_read = NULL, stdout_pipe_write = NULL;
    if (stdio.out) {
        if (!CreatePipe(&stdout_pipe_read, &stdout_pipe_write, &saAttr, 0)) {
            return PROC_INVALID;
        }
        SetHandleInformation(stdout_pipe_read, HANDLE_FLAG_INHERIT, 0);
    } else {
        stdout_pipe_write = GetStdHandle(STD_OUTPUT_HANDLE);
    }

    HANDLE stderr_pipe_read = NULL, stderr_pipe_write = NULL;
    if (stdio.err) {
        if (!CreatePipe(&stderr_pipe_read, &stderr_pipe_write, &saAttr, 0)) {
            return PROC_INVALID;
        }
        SetHandleInformation(stderr_pipe_read, HANDLE_FLAG_INHERIT, 0);
    } else {
        stderr_pipe_write = GetStdHandle(STD_ERROR_HANDLE);
    }

    HANDLE stdin_pipe_read = NULL, stdin_pipe_write = NULL;
    if (stdio.in) {
        if (!CreatePipe(&stdin_pipe_read, &stdin_pipe_write, &saAttr, 0)) {
            return PROC_INVALID;
        }
        SetHandleInformation(stdin_pipe_write, HANDLE_FLAG_INHERIT, 0);
    } else {
        stdin_pipe_read = GetStdHandle(STD_INPUT_HANDLE);
    }

    siStartInfo.hStdOutput = stdout_pipe_write;
    siStartInfo.hStdError = stderr_pipe_write;
    siStartInfo.hStdInput = stdin_pipe_read;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    SB sb = {0};
    for (size_t i = 0; i < c->count; i++) {
        if (i != 0) {
            da_push(&sb, ' ');
        }

        const SV   it = sv_from_cstr(c->data[i]);
        const bool need_quoting =
            it.count == 0 || sv_find(it, '\t', NULL) || sv_find(it, '\v', NULL) || sv_find(it, ' ', NULL);

        if (need_quoting) {
            da_push(&sb, '"');
        }

        for (size_t j = 0; j < it.count; j++) {
            switch (it.data[j]) {
            default:
                break;

            case '\\':
                if (j + 1 < it.count && it.data[j + 1] == '"') {
                    da_push(&sb, '\\');
                }
                break;

            case '"':
                da_push(&sb, '\\');
                break;
            }

            if (!i && it.data[j] == '/') {
                da_push(&sb, '\\');
            } else {
                da_push(&sb, it.data[j]);
            }
        }

        if (need_quoting) {
            da_push(&sb, '"');
        }
    }

    c->count = 0;
    da_push(&sb, '\0');

    if (!CreateProcessA(NULL, sb.data, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo)) {
        return PROC_INVALID;
    }

    sb_free(&sb);
    CloseHandle(piProcInfo.hThread);

    if (stdio.out) {
        CloseHandle(stdout_pipe_write);
        *stdio.out = _fdopen(_open_osfhandle((intptr_t) stdout_pipe_read, _O_RDONLY), "r");
    }

    if (stdio.err) {
        CloseHandle(stderr_pipe_write);
        *stdio.err = _fdopen(_open_osfhandle((intptr_t) stderr_pipe_read, _O_RDONLY), "r");
    }

    if (stdio.in) {
        CloseHandle(stdin_pipe_read);
        *stdio.in = _fdopen(_open_osfhandle((intptr_t) stdin_pipe_write, _O_WRONLY), "w");
    }

    return piProcInfo.hProcess;
#else
    int in[2] = {0};
    int out[2] = {0};
    int err[2] = {0};
    int fail[2] = {0};
    if (pipe(fail) < 0 || fcntl(fail[1], F_SETFD, FD_CLOEXEC) < 0) {
        return PROC_INVALID;
    }

    if (stdio.in && pipe(in) < 0) {
        return PROC_INVALID;
    }

    if (stdio.out && pipe(out) < 0) {
        return PROC_INVALID;
    }

    if (stdio.err && pipe(err) < 0) {
        return PROC_INVALID;
    }

    Proc proc = fork();
    if (proc < 0) {
        return PROC_INVALID;
    }

    if (!proc) {
        if (stdio.in) {
            close(in[1]);
            dup2(in[0], STDIN_FILENO);
            close(in[0]);
        }

        if (stdio.out) {
            close(out[0]);
            dup2(out[1], STDOUT_FILENO);
            close(out[1]);
        }

        if (stdio.err) {
            close(err[0]);
            dup2(err[1], STDERR_FILENO);
            close(err[1]);
        }

        da_push(c, NULL);

        close(fail[0]);
        execvp(*c->data, (char *const *) c->data);
        write(fail[1], "E", 1);
        close(fail[1]);
        exit(127);
    }

    c->count = 0;

    close(fail[1]);
    char       buffer[1];
    const long count = read(fail[0], buffer, sizeof(buffer));
    close(fail[0]);

    if (count > 0) {
        waitpid(proc, NULL, 0); // Wait for the child to kill itself so it doesn't become a zombie
        return PROC_INVALID;
    }

    if (stdio.in) {
        close(in[0]);
        *stdio.in = fdopen(in[1], "w");
        if (!*stdio.in) {
            close(in[1]);
        }
    }

    if (stdio.out) {
        close(out[1]);
        *stdio.out = fdopen(out[0], "r");
        if (!*stdio.out) {
            close(out[0]);
        }
    }

    if (stdio.err) {
        close(err[1]);
        *stdio.err = fdopen(err[0], "r");
        if (!*stdio.err) {
            close(err[0]);
        }
    }

    return proc;
#endif // PLATFORM_X86_64_WINDOWS
}

int cmd_wait(Proc proc) {
#ifdef PLATFORM_X86_64_WINDOWS
    if (proc == PROC_INVALID) {
        return 1;
    }

    if (WaitForSingleObject(proc, INFINITE) == WAIT_FAILED) {
        return 1;
    }

    DWORD exit_code;
    if (!GetExitCodeProcess(proc, &exit_code)) {
        return 1;
    }

    CloseHandle(proc);
    return exit_code;
#else
    if (proc == PROC_INVALID) {
        return 1;
    }

    int status = 0;
    if (waitpid(proc, &status, 0) < 0) {
        return 1;
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return WEXITSTATUS(status);
#endif // PLATFORM_X86_64_WINDOWS
}

int cmd_run_sync(Cmd *c, Cmd_Stdio stdio) {
    return cmd_wait(cmd_run_async(c, stdio));
}

bool procs_push(Procs *ps, Proc p) {
    if (p == PROC_INVALID) {
        return false;
    }

    da_push(ps, p);
    if (ps->count <= ps->nprocs) {
        return true;
    }

    return procs_flush(ps);
}

bool procs_flush(Procs *ps) {
    bool ok = true;
    for (size_t i = 0; i < ps->count; i++) {
        const int code = cmd_wait(ps->data[i]);
        if (code != 0) {
            ok = false;
        }
    }

    ps->count = 0;
    return ok;
}
