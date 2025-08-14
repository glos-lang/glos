#ifndef MESSAGE_H
#define MESSAGE_H

#include "token.h"

typedef enum {
    MESSAGE_NOTE,
    MESSAGE_ERROR,
} MessageKind;

void message_begin(MessageKind kind, Pos pos);
void message_end(Pos pos);

void message_full(MessageKind kind, Pos pos, const char *fmt, ...) PrintfLike(3);
void message_standalone(MessageKind kind, const char *fmt, ...) PrintfLike(2);

#endif // MESSAGE_H
