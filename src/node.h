#ifndef NODE_H
#define NODE_H

#include "token.h"
#include <llvm-c/Types.h>

typedef struct Context_Fn Context_Fn;

typedef struct Node Node;

typedef struct Node_Atom   Node_Atom;
typedef struct Node_Define Node_Define;

typedef struct Node_Fn     Node_Fn;
typedef struct Node_Enum   Node_Enum;
typedef struct Node_Struct Node_Struct;

typedef struct {
    Node *head;
    Node *tail;
} Nodes;

void nodes_push(Nodes *ns, Node *n);

typedef DA(Node_Atom *) Local_Scope;
typedef HT(SV, Node_Atom *) Global_Scope;

typedef struct Module Module;

struct Module {
    const char *name;
    const char *absolute_path;
    const char *relative_path;

    Nodes        nodes;
    Global_Scope globals;

    Module *next;
};

typedef struct {
    HT(const char *, Module *) table;
    Module *head;
    Module *tail;
} Modules;

void modules_free(Modules *m);

typedef enum {
    TYPE_UNIT,
    TYPE_BOOL,
    TYPE_CHAR,

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
    TYPE_ENUM,
    TYPE_STRUCT,

    TYPE_SLICE,
    TYPE_STRING,

    TYPE_GROUP,
    TYPE_MODULE,

    TYPE_UNKNOWN_ENUM,     // .Enum_Value
    TYPE_UNKNOWN_COMPOUND, // {Foo, bar}
    COUNT_TYPES,
} Type_Kind;

typedef struct Type              Type;
typedef struct Type_Fn_Arg       Type_Fn_Arg;
typedef struct Type_Struct_Field Type_Struct_Field;

typedef struct {
    Type_Fn_Arg *args;
    size_t       args_count;
    size_t       args_count_min;
    bool         is_variadic;

    Type  *returns;
    size_t returns_count;

    Type *return_type;
} Type_Fn;

typedef struct {
    Type_Kind  underlying;
    Node_Enum *definition;
} Type_Enum;

typedef struct {
    Type_Struct_Field *fields;
    size_t             fields_count;

    Node_Struct *definition;

    LLVMTypeRef     llvm;
    LLVMMetadataRef debug;
} Type_Struct;

typedef struct {
    Type *element;
} Type_Slice;

typedef struct {
    Type  *data;
    size_t count;

    LLVMTypeRef     llvm;
    LLVMMetadataRef debug;
} Type_Group;

typedef struct {
    Type  *children;
    size_t count;
} Type_Unknown_Compound;

struct Type {
    Type_Kind kind;
    size_t    ref;

    // A :: 69  // typeof(A) => Type { kind = TYPE_I64, is_meta = false }
    // B :: i64 // typeof(B) => Type { kind = TYPE_I64, is_meta = true  }
    bool is_meta;

    Node_Atom *distinct;

    union {
        Type_Fn      fn;
        Type_Slice   slice;
        Type_Enum    enumm;
        Type_Struct *structt;
        Type_Group   group;
        Module      *module;

        Type_Unknown_Compound unknown_compound;
    } spec;

    LLVMTypeRef llvm;
};

typedef struct Const_Value Const_Value;

struct Type_Fn_Arg {
    Pos  pos;
    SV   name;
    Type type;

    // For default arguments
    Const_Value *default_value;
    LLVMValueRef default_value_llvm;

    bool default_value_is_caller_location;
};

struct Type_Struct_Field {
    Pos  pos;
    SV   name;
    Type type;
};

const char *type_to_cstr_raw(Type type);
const char *type_to_cstr(Type type);

bool type_eq(Type a, Type b);
bool type_kind_eq(Type type, Type_Kind kind);
bool type_is_numeric(Type type);
bool type_is_integer(Type type);
bool type_is_pointer(Type type);
bool type_is_scalar(Type type);
bool type_is_signed(Type type);
bool type_is_unknown(Type type);

typedef enum {
    CONST_VALUE_INT,
    CONST_VALUE_FN,
    CONST_VALUE_TYPE,
    CONST_VALUE_STRUCT,
    CONST_VALUE_STRING,
    CONST_VALUE_MODULE,
    COUNT_CONST_VALUES
} Const_Value_Kind;

typedef struct {
    Type_Struct *spec;
    Const_Value *fields;
} Const_Value_Struct;

struct Const_Value {
    Const_Value_Kind kind;
    union {
        i64                integer;
        Type               type;
        Node_Fn           *fn;
        Const_Value_Struct structt;
        SV                 string;
        Module            *module;
    } as;
};

#define const_value_int(v)    ((Const_Value) {.kind = CONST_VALUE_INT, .as.integer = (v)})
#define const_value_fn(v)     ((Const_Value) {.kind = CONST_VALUE_FN, .as.fn = (v)})
#define const_value_type(v)   ((Const_Value) {.kind = CONST_VALUE_TYPE, .as.type = (v)})
#define const_value_struct(v) ((Const_Value) {.kind = CONST_VALUE_STRUCT, .as.structt = (v)})
#define const_value_string(v) ((Const_Value) {.kind = CONST_VALUE_STRING, .as.string = (v)})
#define const_value_module(v) ((Const_Value) {.kind = CONST_VALUE_MODULE, .as.module = (v)})

bool const_value_eq(Const_Value a, Const_Value b);

typedef enum {
    UNCHECKED,
    CHECKING,
    CHECKED,
} Check_Status;

typedef enum {
    NODE_ATOM,
    NODE_GROUP,
    NODE_GHOST,
    NODE_UNARY,
    NODE_BINARY,
    NODE_MEMBER,
    NODE_ASSERT,
    NODE_IMPORT,

    NODE_FN,
    NODE_ENUM,
    NODE_STRUCT,
    NODE_COMPOUND,

    NODE_CALL,

    NODE_SLICE,
    NODE_INDEX,

    NODE_DEFINE,
    NODE_BLOCK,
    NODE_IF,
    NODE_FOR,

    NODE_CASE,
    NODE_SWITCH,

    NODE_JUMP,
    NODE_DEFER,
    NODE_RETURN,

    NODE_EXTERN,

    NODE_PRINT,
    COUNT_NODES
} Node_Kind;

struct Node {
    Node_Kind kind;
    Token     token;
    Type      type;
    Node     *next;
};

Node *node_alloc(Arena *a, Node_Kind kind, Token token);
Node *node_iter(Node *it, Node *ll);

typedef struct {
    bool is_local;
    bool is_extern;
    bool is_assigned;

    // This is 0 for variables which are not arguments. For arguments, counting starts from 1
    size_t arg_index;

    // For multiple definition
    size_t group_index;

    Node_Define *definition_node;
    Node        *assignment_node;

    Context_Fn  *context;
    Check_Status check_status;

    bool        is_const;
    bool        is_const_value_evaluated;
    Const_Value const_value;

    // If this is non-empty, then use this as the linker symbol
    SV link_as;

    LLVMValueRef llvm;
} Definition_Spec;

struct Node_Atom {
    Node node;

    // The module this atom was parsed in
    Module *module;

    // When this atom is a definition
    Definition_Spec *definition_spec;

    // When this atom is a reference to another defining atom
    Node_Atom *definition;
};

typedef struct {
    Node   node;
    Nodes  nodes;
    size_t count;
} Node_Group;

// TODO: This is the solution I came up with so far for default arguments.
// This feels like a bad solution, but better than a non-existent perfect one.
typedef struct {
    Node         node;
    Type_Fn_Arg *arg;
} Node_Ghost;

typedef struct {
    Node  node;
    Node *value;
} Node_Unary;

typedef struct {
    Node  node;
    Node *lhs;
    Node *rhs;
} Node_Binary;

typedef struct {
    Node  node;
    Node *lhs;
    Token field;

    union {
        size_t field_index;
        size_t enum_value;
    };

    // Foo :: #import "Foo"
    // Foo.bar
    //     ^
    Node_Atom *module_access_definition;

    bool is_enum;
} Node_Member;

typedef struct {
    Node  node;
    Node *expr;
    Node *message;
} Node_Assert;

typedef struct {
    Node    node;
    Token   path;
    Module *module;
} Node_Import;

struct Node_Fn {
    Node node;

    Nodes  args;
    size_t args_count;     // Actual
    size_t args_count_min; // Minimum

    Nodes  returns;
    size_t returns_count;

    Node *body;

    bool is_type;
    bool is_extern;
    bool is_inline;
    bool is_variadic;

    Node_Fn *outer_fn;

    Node_Atom *defined_as;
    size_t     defined_as_anon_iota;

    // The module this function was parsed in
    Module *module;

    LLVMValueRef    llvm;
    LLVMMetadataRef llvm_debug_scope;
};

// This represents a type
struct Node_Enum {
    Node  node;
    Node *underlying;

    // Each node of values is as follows:
    //
    // Node_Unary(<name>) {
    //     token.as.integer = <value>
    //     value = Maybe(<constant expression which evaluates to the value of this>)
    // }
    //
    // TODO: Consider adding a new node, so comments like this are not necessary
    Nodes  values;
    size_t values_count;

    Node_Atom *defined_as;
    size_t     defined_as_anon_iota;

    // The module this was parsed in
    Module  *module;
    Node_Fn *defined_in;

    LLVMTypeRef     llvm;
    LLVMMetadataRef debug;
};

// This represents a type
struct Node_Struct {
    Node node;

    Nodes  fields;
    size_t fields_count;

    Node_Atom *defined_as;
    size_t     defined_as_anon_iota;

    // The module this was parsed in
    Module *module;

    Node_Fn *defined_in;
};

typedef struct {
    Node  node;
    Node *lhs;

    Nodes  children;
    size_t children_count;

    // For designated initializers, each node of children is as follows:
    //
    // Node_Binary('=') {
    //     token.as.integer = <index>
    //     lhs = <key>
    //     lhs = <value>
    // }
    //
    // TODO: Consider adding a designated initializer node, so comments like this are not necessary
    bool is_designated;
} Node_Compound;

typedef enum {
    TYPE_CAST_NOP,
    TYPE_CAST_NORMAL,
    TYPE_CAST_TO_BOOL,
    COUNT_TYPE_CASTS,
} Type_Cast;

typedef struct {
    Node  node;
    Node *fn;

    Nodes args;

    // Calculated at checking phase. The reason this is done like this is because in the future functions with
    // multiple return values will be implemented. In such a case, when one of the elements of a call is another
    // call to such a function, the actual argument count will be different from the apparent one, and thus cannot
    // be calculated at parse time.
    size_t args_count;

    Pos end;

    bool      is_type_cast;
    Type_Cast type_cast;

    bool is_stmt;
} Node_Call;

// This *will* represent the following types:
// - Slices
// - Arrays
// - Dynamic Arrays
//
// TODO: Come up with a better name for this
typedef struct {
    Node  node;
    Node *element;
} Node_Slice;

typedef struct {
    Node  node;
    Node *lhs; // TODO: Think of a better name
    Node *a;
    Node *b;
    bool  is_ranged;
} Node_Index;

struct Node_Define {
    Node  node;
    Node *name; // TODO: Rename to 'lhs'

    Node *expr;
    Node *type;

    bool   is_const;
    bool   is_value_known_at_compile_time;
    size_t count;
};

typedef struct {
    Node  node;
    Nodes body;
    Pos   end;
} Node_Block;

typedef struct {
    Node  node;
    Node *condition;
    Node *consequence;
    Node *antecedence;

    bool  is_compile_time;
    Node *compile_time_real_block;
} Node_If;

typedef struct {
    Node  node;
    Node *init;
    Node *condition;
    Node *update;
    Node *body;
} Node_For;

typedef struct {
    Node  node;
    Nodes preds;
    Node *body;
} Node_Case;

typedef struct {
    Node  node;
    Node *expr;
    Nodes cases;
    Node *fallback;

    struct {
        Node       *pred;
        Const_Value value;
    }     *preds;
    size_t preds_count;

    Node_Enum *enumeration;

    bool  is_compile_time;
    Node *compile_time_real_block;
} Node_Switch;

typedef struct {
    Node node;
} Node_Jump;

typedef struct {
    Node  node;
    Node *stmt;
} Node_Defer;

typedef struct {
    Node  node;
    Node *value;
} Node_Return;

typedef struct {
    Node  node;
    Nodes nodes;
} Node_Extern;

typedef struct {
    Node  node;
    Node *value;
} Node_Print;

void node_debug(FILE *f, Node *n);
void nodes_debug(FILE *f, Nodes ns);

#endif // NODE_H
