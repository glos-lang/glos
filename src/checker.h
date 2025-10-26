#ifndef CHECKER_H
#define CHECKER_H

#include "compiler.h"
#include "module.h"

void check_int_limit(Node *n, size_t value);
void check_modules(Compiler *c, Modules ps);

#endif // CHECKER_H
