#ifndef AST_H
#define AST_H

#include "llvm.h"
#include "token.h"

typedef struct AST_Node        AST_Node;
typedef struct AST_Node_Fn     AST_Node_Fn;
typedef struct AST_Node_Atom   AST_Node_Atom;
typedef struct AST_Node_Define AST_Node_Define;

typedef struct {
    AST_Node *head;
    AST_Node *tail;
} AST_Nodes;

void ast_nodes_push(AST_Nodes *ns, AST_Node *n);

typedef enum {
    AST_TYPE_UNIT,
    AST_TYPE_BOOL,

    AST_TYPE_I8,
    AST_TYPE_I16,
    AST_TYPE_I32,
    AST_TYPE_I64,

    AST_TYPE_U8,
    AST_TYPE_U16,
    AST_TYPE_U32,
    AST_TYPE_U64,

    AST_TYPE_INT,
    AST_TYPE_RAWPTR,

    AST_TYPE_FN,

    AST_TYPE_TYPE,
    COUNT_AST_TYPES,
} AST_Type_Kind;

typedef struct AST_Type AST_Type;

typedef struct {
    AST_Type *args;
    size_t    arity;
    AST_Type *returnn;
} AST_Type_Fn;

struct AST_Type {
    AST_Type_Kind kind;
    size_t        ref;

    union {
        AST_Type   *type;
        AST_Type_Fn fn;
    } spec;

    LLVM_Type llvm;
};

const char *ast_type_to_cstr(AST_Type type);

bool ast_type_eq(AST_Type a, AST_Type b);
bool ast_type_is_numeric(AST_Type type);
bool ast_type_is_integer(AST_Type type);
bool ast_type_is_pointer(AST_Type type);
bool ast_type_is_scalar(AST_Type type);

typedef enum {
    CONST_VALUE_INT,

    CONST_VALUE_FN,
    CONST_VALUE_TYPE,

    COUNT_CONST_VALUES
} Const_Value_Kind;

typedef struct {
    Const_Value_Kind kind;
    union {
        long         integer;
        AST_Type     type;
        AST_Node_Fn *fn;
    } as;
} Const_Value;

#define const_value_int(v) ((Const_Value) {.kind = CONST_VALUE_INT, .as.integer = (v)})

#define const_value_fn(v)   ((Const_Value) {.kind = CONST_VALUE_FN, .as.fn = (v)})
#define const_value_type(v) ((Const_Value) {.kind = CONST_VALUE_TYPE, .as.type = (v)})

typedef enum {
    UNINFERRED,
    INFERRING,
    INFERRED,
} Inference_Status;

typedef enum {
    AST_NODE_ATOM,
    AST_NODE_UNARY,
    AST_NODE_BINARY,

    AST_NODE_FN,
    AST_NODE_CALL,

    AST_NODE_DEFINE,
    AST_NODE_BLOCK,
    AST_NODE_IF,
    AST_NODE_FOR,

    AST_NODE_JUMP,
    AST_NODE_RETURN,

    AST_NODE_EXTERN,

    AST_NODE_PRINT,
    COUNT_AST_NODES
} AST_Node_Kind;

struct AST_Node {
    AST_Node_Kind kind;

    Token    token;
    AST_Type type;

    bool is_memory;

    AST_Node *next;
};

struct AST_Node_Atom {
    AST_Node node;

    // When this atom is a definition
    bool             is_const;
    bool             is_local;
    bool             is_extern;
    bool             is_assigned;
    Const_Value      const_value;
    AST_Node_Define *definition_stmt;
    Inference_Status inference_status;
    LLVM_Node       *llvm;

    // When this atom is a reference to another defining atom
    AST_Node_Atom *definition;
};

typedef struct {
    AST_Node  node;
    AST_Node *value;
} AST_Node_Unary;

typedef struct {
    AST_Node  node;
    AST_Node *lhs;
    AST_Node *rhs;
} AST_Node_Binary;

struct AST_Node_Fn {
    AST_Node node;

    AST_Nodes args;
    size_t    arity;

    AST_Node *returnn;
    AST_Node *body;

    bool is_type;
    bool is_extern;

    bool signature_checked;

    AST_Node_Fn   *outer_fn;
    AST_Node_Atom *defined_as;

    LLVM_Node *llvm;
};

typedef enum {
    TYPE_CAST_NOP,
    TYPE_CAST_NORMAL,
    TYPE_CAST_TO_BOOL,
    COUNT_TYPE_CASTS,
} Type_Cast;

typedef struct {
    AST_Node  node;
    AST_Node *fn;

    AST_Nodes args;
    size_t    arity; // Calculated at checking phase

    Pos end;

    bool      is_type_cast;
    Type_Cast type_cast;
} AST_Node_Call;

struct AST_Node_Define {
    AST_Node  node;
    AST_Node *name;
    AST_Node *type;
    AST_Node *expr;

    bool is_arg;
    bool is_const;
    bool is_local;
    bool is_extern;
};

typedef struct {
    AST_Node  node;
    AST_Nodes body;
    Pos       end;
} AST_Node_Block;

typedef struct {
    AST_Node  node;
    AST_Node *condition;
    AST_Node *consequence;
    AST_Node *antecedence;
} AST_Node_If;

typedef struct {
    AST_Node  node;
    AST_Node *init;
    AST_Node *condition;
    AST_Node *update;
    AST_Node *body;
} AST_Node_For;

typedef struct {
    AST_Node node;
} AST_Node_Jump;

typedef struct {
    AST_Node  node;
    AST_Node *value;
} AST_Node_Return;

typedef struct {
    AST_Node  node;
    AST_Nodes nodes;
} AST_Node_Extern;

typedef struct {
    AST_Node  node;
    AST_Node *value;
} AST_Node_Print;

void ast_node_debug(FILE *f, AST_Node *n);
void ast_nodes_debug(FILE *f, AST_Nodes ns);

#endif // AST_H
