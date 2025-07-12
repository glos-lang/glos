#include "context.h"

Node *scope_find(Scope s, SV name) {
    for (size_t i = s.count; i > 0; i--) {
        Node *it = s.data[i - 1];
        if (sv_eq(it->token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

ContextFn context_fn_begin(Context *c, NodeFn *fn) {
    const ContextFn save = c->fn;
    c->fn.base = c->locals.count;
    c->fn.fn = fn;
    return save;
}

void context_fn_end(Context *c, ContextFn save) {
    c->locals.count = c->fn.base;
    c->fn = save;
}

Node *context_fn_find(ContextFn f, Scope s, SV name) {
    assert(f.base <= s.count);
    s.data += f.base;
    s.count -= f.base;
    return scope_find(s, name);
}

size_t context_memory_alloc(Context *c, size_t size) {
    da_grow(&c->memory, size);
    memset(&c->memory.data[c->memory.count], 0, size);
    return c->memory.count;
}

void *context_memory_read(Context *c, size_t offset) {
    return &c->memory.data[offset];
}

void context_memory_write(Context *c, size_t offset, const void *src, size_t size) {
    memcpy(&c->memory.data[offset], src, size);
}

void context_free(Context *c) {
    da_free(&c->types);
    da_free(&c->locals);
    da_free(&c->globals);
    da_free(&c->memory);
}
