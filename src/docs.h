#ifndef DOCUMENTATION_H
#define DOCUMENTATION_H

#include "lexer.h"
#include "package.h"

void docs_generate(Packages packages, Comments *comments, const char *dir, const char *std);

#endif // DOCUMENTATION_H
