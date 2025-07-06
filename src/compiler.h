#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

size_t compile_sizeof(Type *type);

void compile_nodes(Context *context, const char *output, const char **flags, size_t flags_count);

#endif // COMPILER_H
