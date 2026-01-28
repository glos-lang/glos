#ifndef COMPILER_H
#define COMPILER_H

#include "ast.h"
#include "llvm.h"

typedef struct {
    Cmd *cmd;
    LLVM llvm;

    // For debug information
    const char *path;
} Compiler;

void compiler_build(Compiler *c, AST_Nodes nodes, const char *output);

#endif // COMPILER_H
