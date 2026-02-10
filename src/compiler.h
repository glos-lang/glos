#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

typedef struct {
    Cmd *cmd;
    LLVM llvm;

    Decls globals;

    // For debug information
    const char *path;
} Compiler;

void compiler_build(Compiler *c, AST_Nodes nodes, const char *output);

#endif // COMPILER_H
