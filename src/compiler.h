#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

typedef struct {
    QbeNode  *var;
    QbeBlock *block;
} CompilerYield;

typedef struct {
    Context context;

    Qbe   *qbe;
    QbeFn *fn;

    CompilerYield yield;
} Compiler;

void compiler_init(Compiler *c);
void compiler_run(Compiler *c, const char *output, const char **flags, size_t flags_count);

size_t compile_sizeof(Compiler *c, Type *type);

#endif // COMPILER_H
