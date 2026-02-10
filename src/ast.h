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

    AST_TYPE_TYPE,
    COUNT_AST_TYPES,
} AST_Type_Kind;

typedef struct AST_Type AST_Type;

struct AST_Type {
    AST_Type_Kind kind;

    union {
        AST_Type *type;
    } spec;

    LLVM_Type llvm;
};

const char *ast_type_to_cstr(AST_Type type);

bool ast_type_eq(AST_Type a, AST_Type b);
bool ast_type_is_numeric(AST_Type type);

typedef enum {
    AST_NODE_ATOM,
    AST_NODE_UNARY,
    AST_NODE_BINARY,

    AST_NODE_DECL,
    AST_NODE_BLOCK,
    AST_NODE_IF,

    AST_NODE_PRINT,
    COUNT_AST_NODES
} AST_Node_Kind;

struct AST_Node {
    AST_Node_Kind kind;

    Token    token;
    AST_Type type;

    bool allow_ref;

    AST_Node *next;
};

typedef struct {
    AST_Node   node;
    AST_Node  *definition;
    LLVM_Node *llvm;
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
    AST_Node *name;
    AST_Node *type;
    AST_Node *expr;
} AST_Node_Decl;

typedef struct {
    AST_Node  node;
    AST_Nodes body;
} AST_Node_Block;

typedef struct {
    AST_Node  node;
    AST_Node *condition;
    AST_Node *consequence;
    AST_Node *antecedence;
} AST_Node_If;

typedef struct {
    AST_Node  node;
    AST_Node *value;
} AST_Node_Print;

void ast_node_debug(FILE *f, AST_Node *n);
void ast_nodes_debug(FILE *f, AST_Nodes ns);

#endif // AST_H
