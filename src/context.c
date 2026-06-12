#include "context.h"
#include "basic.h"
#include "token.h"

void local_scope_push(Local_Scope *scope, Node_Atom *atom) {
    da_push(scope, atom);
}

Node_Atom *local_scope_find(Local_Scope scope, SV name) {
    for (size_t i = scope.count; i > 0; i--) {
        Node_Atom *it = scope.data[i - 1];
        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

void global_scope_push(Global_Scope *scope, Node_Atom *atom) {
    ht_set(scope, atom->node.token.sv, atom);
}

Node_Atom *global_scope_find(Global_Scope *scope, SV name) {
    if (!scope->hasheq) {
        scope->hasheq = ht_hasheq_sv;
    }

    Node_Atom **p = ht_get(scope, name);
    return p ? *p : NULL;
}

Node_Atom *context_fn_find(const Context_Fn *fn, const Local_Scope *locals, SV name, bool only_consts) {
    if (!fn) {
        return NULL;
    }

    assert(fn->begin <= locals->count);
    assert(fn->end <= locals->count);
    for (size_t i = fn->end; i > fn->begin; i--) {
        Node_Atom *it = locals->data[i - 1];
        if (!it->definition_spec->is_const && only_consts) {
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
    c->fn = fn;
}

void context_pop_fn(Context *c) {
    assert(c->fn);
    c->locals.count = c->fn->begin;
    c->fn = c->fn->outer;
}

void context_restore_fn(Context *c, Context_Fn *save) {
    c->fn = save;
    if (c->fn) {
        c->locals.count = c->fn->end;
    }
}

void context_push_local(Context *c, Node_Atom *atom) {
    assert(c->fn);
    assert(c->fn->end == c->locals.count);
    da_push(&c->locals, atom);
    c->fn->end++;
}

Node_Atom *context_find_local(const Context *c, SV name) {
    return context_fn_find(c->fn, &c->locals, name, false);
}

void context_set_end(Context *c, size_t end) {
    if (c->fn) {
        assert(c->fn->end == c->locals.count);
        c->fn->end = end;
        c->locals.count = end;
    }
}
