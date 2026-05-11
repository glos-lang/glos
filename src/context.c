#include "context.h"
#include "basic.h"
#include "token.h"

void scope_push(Scope *scope, Node_Atom *node) {
    da_push(scope, node);
}

Node_Atom *scope_find(Scope scope, SV name) {
    for (size_t i = scope.count; i > 0; i--) {
        Node_Atom *it = scope.data[i - 1];
        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

Node_Atom *context_fn_find(const Context_Fn *fn, const Scope *locals, SV name, bool only_consts) {
    if (!fn) {
        return NULL;
    }

    assert(fn->begin <= locals->count);
    assert(fn->end <= locals->count);
    for (size_t i = fn->end; i > fn->begin; i--) {
        Node_Atom *it = locals->data[i - 1];
        if (!it->is_const && only_consts) {
            continue;
        }

        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return context_fn_find(fn->outer, locals, name, true);
}

void context_push_fn(Context *c, Context_Fn *fn) {
    assert(fn);
    fn->begin = c->locals.count;
    fn->end = c->locals.count;
    c->current = fn;
}

void context_pop_fn(Context *c) {
    assert(c->current);
    c->locals.count = c->current->begin;
    c->current = c->current->outer;
}

void context_restore_fn(Context *c, Context_Fn *save) {
    c->current = save;
    if (c->current) {
        c->locals.count = c->current->end;
    }
}

void context_push_local(Context *c, Node_Atom *atom) {
    assert(c->current);
    assert(c->current->end == c->locals.count);
    da_push(&c->locals, atom);
    c->current->end++;
}

Node_Atom *context_find_local(const Context *c, SV name) {
    return context_fn_find(c->current, &c->locals, name, false);
}

void context_set_end(Context *c, size_t end) {
    if (c->current) {
        assert(c->current->end == c->locals.count);
        c->current->end = end;
        c->locals.count = end;
    }
}
