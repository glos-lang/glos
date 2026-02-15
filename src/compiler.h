#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

typedef struct {
    Cmd *cmd;
    LLVM llvm;

    Scope locals;
    Scope globals;

    AST_Node_Fn *fn_current;
    size_t       fn_base;

    LLVM_Node_Block *loop_break;
    LLVM_Node_Block *loop_continue;

    // TODO: Temporary solutions to permanent problems
    size_t      iota_fn;
    const char *path;
} Compiler;

void compiler_build(Compiler *c, const char *output);

#endif // COMPILER_H
