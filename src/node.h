#ifndef NODE_H
#define NODE_H

#include "qbe.h"
#include "token.h"

typedef struct Node    Node;
typedef struct Package Package;

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
    TYPE_SLICE,
    TYPE_ARRAY,
    TYPE_STRUCT,

    COUNT_TYPES
} TypeKind;

typedef struct Type Type;

struct Type {
    TypeKind kind;
    size_t   ref;

    // For:
    //   TYPE_FN
    //   TYPE_STRUCT
    Node *spec_node;

    // For:
    //   TYPE_SLICE
    //   TYPE_ARRAY
    Type *spec_type;

    // For:
    //   TYPE_ARRAY
    size_t spec_count;

    QbeType qbe;
};

const char *type_to_cstr(Type type);

bool type_eq(Type a, Type b);
bool type_is_signed(Type type);
bool type_is_integer(Type type);
bool type_is_pointer(Type type);

Type type_remove_ref(Type type);

typedef enum {
    CHECK_STATUS_TODO,
    CHECK_STATUS_DOING,
    CHECK_STATUS_DONE,
} CheckStatus;

typedef struct {
    bool is_string;
    union {
        bool   boolean;
        size_t integer;
        SV     sv;
    } as;
} ConstValue;

typedef enum {
    NODE_ATOM,
    NODE_CALL,
    NODE_CAST,
    NODE_UNARY,
    NODE_INDEX,
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
    Node     node;
    Node    *definition;
    Package *package;
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
    Node *base;
    Node *from;
    Node *to;
    bool  ranged;
} NodeIndex;

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
    Node *message;
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
    Node *link;
    bool  local;

    Package    *package;
    CheckStatus check_status;

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
    Node *link;

    bool is_extern;
    bool is_static;

    Package    *package;
    NodeVarKind kind;
    CheckStatus check_status;

    QbeNode *qbe;
} NodeVar;

typedef struct {
    Node node;

    size_t ref;
    Node  *definition;

    bool local;

    Package    *package;
    CheckStatus check_status;
} NodeType;

typedef struct {
    Node node;

    Node *expr;
    Node *type;
    bool  local;

    Package    *package;
    ConstValue  value;
    CheckStatus check_status;

    QbeNode *qbe;
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

    Package    *package;
    CheckStatus check_status;

    QbeStruct *qbe;
} NodeStruct;

typedef struct {
    Node  node;
    Nodes libraries;
    Nodes definitions;
} NodeExtern;

typedef struct {
    Node  node;
    Node *operand;
} NodePrint;

#endif // NODE_H
