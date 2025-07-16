#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

typedef struct {
    Context context;

    Qbe   *qbe;
    QbeFn *fn;

    QbeNode *print_fn;
    QbeNode *print_sfmt;
    QbeNode *print_ufmt;

    QbeType slice_type;
} Compiler;

void compiler_init(Compiler *c);
void compiler_build(Compiler *c, const char *object_file_path);

size_t compile_sizeof(Compiler *c, Type *type);

#endif // COMPILER_H
