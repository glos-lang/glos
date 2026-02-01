#include "basic.h"
#include <fcntl.h>
#include <stdarg.h>

#ifdef PLATFORM_X86_64_WINDOWS
#include <io.h>
#else
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif // PLATFORM_X86_64_WINDOWS

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

// String Builder
void sb_sprintf(SB *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    sb_grow(sb, n + 1);

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

// FS
bool read_file(const char *path, SV *out, Arena *arena) {
    char *data = NULL;
    bool  result = true;

    FILE *f = fopen(path, "r");
    if (!f) {
        return_defer(false);
    }

    if (fseek(f, 0, SEEK_END)) {
        return_defer(false);
    }

    const long count = ftell(f);
    if (count < 0) {
        return_defer(false);
    }
    rewind(f);

    data = arena_alloc(arena, count + 1);
    const size_t bytes = fread(data, 1, count, f);
    data[bytes] = '\0';

    out->data = data;
    out->count = bytes;

defer:
    if (f) {
        fclose(f);
    }

    if (!result) {
        free(data);
    }

    return result;
}

bool delete_file(const char *path) {
#ifdef PLATFORM_X86_64_WINDOWS
    return DeleteFileA(path);
#else
    return unlink(path) == 0;
#endif // PLATFORM_X86_64_WINDOWS
}

// Processes
Proc cmd_run_async(Cmd *c, CmdStdio stdio) {
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

int cmd_run_sync(Cmd *c, CmdStdio stdio) {
    return cmd_wait(cmd_run_async(c, stdio));
}

// Others
double get_time(void) {
#ifdef PLATFORM_X86_64_WINDOWS
    static LARGE_INTEGER freq;
    static int           initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double) counter.QuadPart / (double) freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
#endif // PLATFORM_X86_64_WINDOWS
}
