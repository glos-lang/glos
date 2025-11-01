#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

typedef DynamicArray(const char *) LinkFlags;

typedef struct {
    Context context;

    Qbe   *qbe;
    QbeFn *fn;

    Scope  defers;
    size_t defers_start;

    QbeBlock *loop_break;
    QbeBlock *loop_continue;
    size_t    loop_defers_start;

    QbeType slice_type;
    QbeType dslice_type;

    LinkFlags link_flags;
} Compiler;

void compiler_init(Compiler *c);
void compiler_build(Compiler *c, const char *object_file_path);

size_t compile_sizeof(Compiler *c, Type *type);

#endif // COMPILER_H
