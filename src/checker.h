#ifndef CHECKER_H
#define CHECKER_H

#include "compiler.h"
#include "package.h"

void check_int_limit(Node *n, size_t value);
void check_packages(Compiler *c, Packages ps);

#endif // CHECKER_H
