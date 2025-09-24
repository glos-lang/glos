#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef enum {
    CWS_INLINE,
    CWS_NEWLINE,
    CWS_BLANKLINE,
} CommentWS;

typedef struct {
    CommentWS ws;
    bool      shebang;

    Pos pos;
    SV  sv;
} Comment;

typedef DynamicArray(Comment) Comments;

typedef struct {
    Pos    pos;
    SV     sv;
    size_t newlines;

    bool  peeked;
    Token buffer;

    Comments *comments;
} Lexer;

bool lexer_open(Lexer *l, const char *path, Arena *arena);

void lexer_buffer(Lexer *l, Token token);
void lexer_unbuffer(Lexer *l);

Token lexer_next(Lexer *l);
Token lexer_peek(Lexer *l);
bool  lexer_read(Lexer *l, TokenKind kind);

Token lexer_expect_impl(Lexer *l, const TokenKind *kinds);
#define lexer_expect(l, ...) lexer_expect_impl((l), (const TokenKind[]) {__VA_ARGS__, TOKEN_EOF})

Token lexer_split_token(Lexer *l, Token token);

#endif // LEXER_H
