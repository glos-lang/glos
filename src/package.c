#include "package.h"

void imports_push(Imports *is, Import *i) {
    if (!i) {
        return;
    }

    if (is->tail) {
        is->tail->next = i;
        is->tail = i;
    } else {
        is->head = i;
        is->tail = i;
    }
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
        ps->tail = p;
    } else {
        ps->head = p;
        ps->tail = p;
    }

    ps->current = p;
}
