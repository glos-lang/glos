#include "package.h"

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

void packages_free(Packages *ps) {
    for (Package *p = ps->head; p; p = p->next) {
        da_free(&p->globals);
    }
}

void packages_push(Packages *ps, Package *p) {
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

Package *packages_find_by_name(Packages ps, SV name) {
    for (Package *it = ps.head; it; it = it->next) {
        if (sv_eq(it->name.sv, name)) {
            return it;
        }
    }

    return NULL;
}

Package *packages_find_by_path(Packages ps, SV path) {
    for (Package *it = ps.head; it; it = it->next) {
        if (sv_eq(it->path, path)) {
            return it;
        }
    }

    return NULL;
}
