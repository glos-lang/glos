#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

size_t compile_sizeof(Type *type);

void compile_nodes(Context *context, const char *output);

#endif // COMPILER_H
