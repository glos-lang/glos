#ifndef FORMATTER_H
#define FORMATTER_H

#include "package.h"

bool format_file(const char *path, SV package, Import *imports, Node *nodes, SB *sb);

#endif // FORMATTER_H
