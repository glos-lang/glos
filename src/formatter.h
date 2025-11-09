#ifndef FORMATTER_H
#define FORMATTER_H

#include "lexer.h"
#include "package.h"

typedef struct {
    SB sb;

    Comments *comments;
    size_t    comments_synced;

    size_t depth;
} Formatter;

void formatter_free(Formatter *f);

bool format_file(Formatter *f, const char *path, Token package, Import *imports, Node *nodes);

#endif // FORMATTER_H
