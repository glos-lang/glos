#include "context.h"

static Node *scope_find_impl(Scope s, SV name, bool is_type, size_t base) {
    assert(base <= s.count);
    for (size_t i = s.count; i > 0; i--) {
        Node *it = s.data[i - 1];
        if (is_type != (it->kind == NODE_TYPE || it->kind == NODE_STRUCT)) {
            continue;
        }

        if (it->kind == NODE_VAR && i <= base) {
            // Local variables cannot spill into nested functions
            continue;
        }

        if (sv_eq(it->token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

Node *scope_find(Scope s, SV name, bool is_type) {
    return scope_find_impl(s, name, is_type, 0);
}

NodeFn *methods_find(Methods *ms, SV name) {
    for (NodeFn *it = ms->head; it; it = it->next_method) {
        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

void methods_push(Methods *ms, NodeFn *m) {
    if (!m) {
        return;
    }

    if (ms->tail) {
        ms->tail->next_method = m;
    } else {
        ms->head = m;
    }

    ms->tail = m;
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

Node *context_fn_find(ContextFn f, Scope s, SV name, bool is_type) {
    return scope_find_impl(s, name, is_type, f.base);
}

Methods *context_methods_find(Context *c, Type type) {
    for (Methods *it = c->methods; it; it = it->next) {
        if (type_eq(it->type, type)) {
            return it;
        }
    }

    return NULL;
}

Methods *context_methods_alloc(Context *c, Type type) {
    Methods *ms = arena_alloc(c->arena, sizeof(Methods));
    ms->type = type;
    ms->next = c->methods;
    c->methods = ms;
    return ms;
}

void context_free(Context *c) {
    da_free(&c->locals);
    da_free(&c->statics);
}
