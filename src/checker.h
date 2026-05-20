#ifndef CHECKER_H
#define CHECKER_H

#include "compiler.h"
#include "parser.h" // TODO: For Modules. Consider whether to move `modules` into a separate file

void check_nodes(Compiler *c, Modules modules);

#endif // CHECKER_H
