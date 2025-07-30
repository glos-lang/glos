#ifndef MESSAGE_H
#define MESSAGE_H

#include "token.h"

typedef enum {
    MESSAGE_NOTE,
    MESSAGE_ERROR,
} MessageKind;

void message_begin(MessageKind kind, Pos pos);
void message_end(Pos pos, SV sv);

void message_full(MessageKind kind, Pos pos, SV sv, const char *fmt, ...) PrintfLike(4);
void message_standalone(MessageKind kind, const char *fmt, ...) PrintfLike(2);

#endif // MESSAGE_H
