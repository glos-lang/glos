#include "module.h"

void imports_push(Imports *is, Import *i) {
    if (!i) {
        return;
    }

    if (is->tail) {
        is->tail->next = i;
    } else {
        is->head = i;
    }

    is->tail = i;
}

void modules_free(Modules *ps) {
    for (Module *p = ps->head; p; p = p->next) {
        da_free(&p->globals);
    }
}

void modules_push(Modules *ps, Module *p) {
    if (!p) {
        return;
    }

    if (ps->tail) {
        ps->tail->next = p;
    } else {
        ps->head = p;
    }

    ps->tail = p;
    ps->current = p;
}

Module *modules_find_by_name(Modules ps, SV name) {
    for (Module *it = ps.head; it; it = it->next) {
        if (sv_eq(it->name.sv, name)) {
            return it;
        }
    }

    return NULL;
}

Module *modules_find_by_path(Modules ps, SV path) {
    for (Module *it = ps.head; it; it = it->next) {
        if (sv_eq(it->path, path)) {
            return it;
        }
    }

    return NULL;
}
