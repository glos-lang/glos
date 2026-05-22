#ifndef CHECKER_H
#define CHECKER_H

#include "compiler.h"

Const_Value get_platform(Compiler *c, Type *type);
Const_Value get_const_definition_value(Compiler *c, Module *m, SV name, Type *type);

void check_nodes(Compiler *c);

#endif // CHECKER_H
