#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"

typedef struct {
    Scope   globals;
    Context context;

    Cmd *cmd;
    LLVM llvm;

    LLVM_Node_Block *loop_break;
    LLVM_Node_Block *loop_continue;

    size_t iota_anonymous_fn;
    size_t iota_anonymous_struct;

    // TODO: Temporary solutions to permanent problems
    const char *path;
} Compiler;

size_t compile_sizeof(Compiler *c, AST_Type *type);
void   compiler_build(Compiler *c, const char *output);

#endif // COMPILER_H
