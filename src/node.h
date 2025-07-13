#ifndef NODE_H
#define NODE_H

#include "qbe.h"
#include "token.h"

typedef struct Node Node;

typedef struct {
    Node *head;
    Node *tail;
} Nodes;

typedef enum {
    TYPE_UNIT,
    TYPE_BOOL,

    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,

    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,

    TYPE_INT,
    TYPE_RAWPTR,

    TYPE_FN,
    TYPE_STRUCT,

    COUNT_TYPES
} TypeKind;

typedef struct {
    TypeKind kind;
    size_t   ref;
    Node    *spec;
    QbeType  qbe;
} Type;

const char *type_to_cstr(Type type);

bool type_eq(Type a, Type b);
bool type_is_signed(Type type);
bool type_is_integer(Type type);
bool type_is_pointer(Type type);

Type type_remove_ref(Type type);

typedef enum {
    CONST_VALUE_ATOM,
    CONST_VALUE_OFFSET,
    CONST_VALUE_MEMORY,
    COUNT_CONST_VALUES
} ConstValueKind;

typedef struct {
    ConstValueKind kind;

    union {
        bool        boolean;
        size_t      integer;
        const void *memory;
    } as;
} ConstValue;

typedef enum {
    NODE_ATOM,
    NODE_CALL,
    NODE_CAST,
    NODE_UNARY,
    NODE_BINARY,
    NODE_MEMBER,
    NODE_SIZEOF,
    NODE_ASSERT,
    NODE_COMPOUND,

    NODE_IF,
    NODE_FOR,
    NODE_BLOCK,
    NODE_RETURN,

    NODE_FN,
    NODE_VAR,
    NODE_TYPE,
    NODE_CONST,
    NODE_FIELD,
    NODE_STRUCT,
    NODE_EXTERN,

    NODE_PRINT,
    COUNT_NODES
} NodeKind;

struct Node {
    NodeKind kind;
    Type     type;
    Token    token;
    Node    *next;
};

typedef struct {
    Node  node;
    Node *definition;
} NodeAtom;

typedef struct {
    Node  node;
    Node *fn;

    Nodes  args;
    size_t arity;
} NodeCall;

typedef struct {
    Node  node;
    Node *from;
    Node *to;
} NodeCast;

typedef struct {
    Node  node;
    Node *operand;
} NodeUnary;

typedef struct {
    Node  node;
    Node *lhs;
    Node *rhs;
} NodeBinary;

typedef struct {
    Node  node;
    Node *lhs;
    Node *definition;
} NodeMember;

typedef struct {
    Node  node;
    Node *expr;
    Node *type;
} NodeSizeof;

typedef struct {
    Node  node;
    Node *expr;
    bool  is_static;
} NodeAssert;

typedef struct {
    Node  node;
    Node *type;
    Nodes nodes;
} NodeCompound;

typedef struct {
    Node  node;
    Node *condition;
    Node *consequence;
    Node *antecedence;
} NodeIf;

typedef struct {
    Node  node;
    Node *init;
    Node *condition;
    Node *update;
    Node *body;
} NodeFor;

typedef struct {
    Node  node;
    Nodes body;
} NodeBlock;

typedef struct {
    Node  node;
    Node *value;
} NodeReturn;

typedef struct {
    Node node;

    Nodes  args;
    size_t arity;

    Node *ret;
    Node *body;
    bool  local;

    QbeNode *qbe;
} NodeFn;

Type node_fn_return_type(const NodeFn *fn);

typedef enum {
    NODE_VAR_ARG,
    NODE_VAR_LOCAL,
    NODE_VAR_GLOBAL,
} NodeVarKind;

typedef struct {
    Node node;

    Node *expr;
    Node *type;

    bool is_extern;
    bool is_static;

    NodeVarKind kind;
    ConstValue  const_value;

    QbeNode *qbe;
} NodeVar;

typedef struct {
    Node  node;
    Node *definition;
    bool  local;
} NodeType;

typedef struct {
    Node node;

    Node *expr;
    Node *type;
    bool  local;

    QbeNode   *qbe;
    ConstValue value;
} NodeConst;

typedef struct {
    Node      node;
    Node     *type;
    QbeField *qbe;
} NodeField;

typedef struct {
    Node  node;
    Nodes fields;
    bool  local;

    QbeStruct *qbe;
} NodeStruct;

typedef struct {
    Node  node;
    Nodes nodes;
} NodeExtern;

typedef struct {
    Node  node;
    Node *operand;
} NodePrint;

#endif // NODE_H
