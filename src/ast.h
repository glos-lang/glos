#ifndef AST_H
#define AST_H

#include "llvm.h"
#include "token.h"

typedef struct AST_Node AST_Node;

typedef struct {
    AST_Node *head;
    AST_Node *tail;
} AST_Nodes;

void ast_nodes_push(AST_Nodes *ns, AST_Node *n);

typedef enum {
    AST_TYPE_UNIT,
    AST_TYPE_BOOL,
    AST_TYPE_I64,
    COUNT_AST_TYPES,
} AST_Type_Kind;

typedef struct {
    AST_Type_Kind kind;
    LLVM_Type     llvm;
} AST_Type;

const char *ast_type_to_cstr(AST_Type type);

bool ast_type_eq(AST_Type a, AST_Type b);
bool ast_type_is_numeric(AST_Type type);

typedef enum {
    AST_NODE_ATOM,
    AST_NODE_UNARY,
    AST_NODE_BINARY,

    AST_NODE_PRINT,
    COUNT_AST_NODES
} AST_Node_Kind;

struct AST_Node {
    AST_Node_Kind kind;

    Token    token;
    AST_Type type;

    AST_Node *next;
};

typedef struct {
    AST_Node node;
} AST_Node_Atom;

typedef struct {
    AST_Node  node;
    AST_Node *value;
} AST_Node_Unary;

typedef struct {
    AST_Node  node;
    AST_Node *lhs;
    AST_Node *rhs;
} AST_Node_Binary;

typedef struct {
    AST_Node  node;
    AST_Node *value;
} AST_Node_Print;

void ast_node_debug(FILE *f, AST_Node *n);

#endif // AST_H
