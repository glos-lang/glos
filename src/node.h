#ifndef NODE_H
#define NODE_H

#include "token.h"
#include <llvm-c/Types.h>

typedef struct Context_Fn      Context_Fn;
typedef struct Context_Replace Context_Replace;

typedef struct Node Node;

typedef struct Node_Atom   Node_Atom;
typedef struct Node_Define Node_Define;

typedef struct Node_Fn     Node_Fn;
typedef struct Node_Enum   Node_Enum;
typedef struct Node_Union  Node_Union;
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
    SV name;

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
    TYPE_UNION,
    TYPE_STRUCT,

    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_STRING,
    TYPE_ANY,

    TYPE_GROUP,
    TYPE_MODULE,

    TYPE_UNKNOWN_ENUM,     // .Enum_Value
    TYPE_UNKNOWN_COMPOUND, // {Foo, bar}
    COUNT_TYPES,
} Type_Kind;

typedef struct Type               Type;
typedef struct Type_Fn_Arg        Type_Fn_Arg;
typedef struct Type_Union_Variant Type_Union_Variant;
typedef struct Type_Struct_Field  Type_Struct_Field;

typedef enum {
    VARIADICS_NONE,
    VARIADICS_TYPED,
    VARIADICS_UNTYPED,
} Variadics_Kind;

typedef struct {
    Type_Fn_Arg *args;
    size_t       args_count;
    size_t       args_count_min;

    Variadics_Kind variadics_kind;
    size_t         variadics_index;

    Type  *returns;
    size_t returns_count;

    Type *return_type;

    LLVMTypeRef llvm;
} Type_Fn;

typedef struct {
    Type_Kind  underlying;
    Node_Enum *definition;
} Type_Enum;

typedef struct {
    Type_Union_Variant *variants;
    size_t              variants_count;

    size_t variants_size_max;
    size_t variants_align_max;

    Node_Union *definition;

    LLVMTypeRef     llvm;
    LLVMMetadataRef debug;
} Type_Union;

typedef struct {
    Type_Struct_Field *fields;
    size_t             fields_count;

    Node_Struct *definition;

    LLVMTypeRef     llvm;
    LLVMMetadataRef debug;
} Type_Struct;

typedef struct {
    Type  *element;
    size_t count;
} Type_Array;

typedef struct {
    Type *element;
} Type_Slice;

typedef struct {
    Type   *data;
    size_t *offsets;
    size_t  count;

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
        Type_Fn     *fn;
        Type_Enum    enumm;
        Type_Union  *unionn;
        Type_Struct *structt;

        Type_Array array;
        Type_Slice slice;

        Type_Group group;
        Module    *module;

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

struct Type_Union_Variant {
    Pos    pos;
    Type   type;
    size_t size;
    size_t align;
};

struct Type_Struct_Field {
    Pos    pos;
    SV     name;
    Type   type;
    size_t offset;
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

    CONST_VALUE_UNION,
    CONST_VALUE_STRUCT,

    CONST_VALUE_ARRAY,
    CONST_VALUE_STRING,
    CONST_VALUE_ANY,

    CONST_VALUE_MODULE,
    COUNT_CONST_VALUES
} Const_Value_Kind;

typedef struct {
    Type_Union  *spec;
    size_t       index;
    Const_Value *real;
} Const_Value_Union;

typedef struct {
    Type_Struct *spec;
    Const_Value *fields;
} Const_Value_Struct;

typedef struct {
    Type        *element_type;
    Const_Value *data;
    size_t       count;

    bool is_slice;
} Const_Value_Array;

typedef struct {
    Type        *type;
    Const_Value *value;
} Const_Value_Any;

struct Const_Value {
    Const_Value_Kind kind;
    union {
        i64      integer;
        Type     type;
        Node_Fn *fn;

        Const_Value_Union  unionn;
        Const_Value_Struct structt;

        Const_Value_Array array;
        SV                string;
        Const_Value_Any   any;

        Module *module;
    } as;
};

static_assert(COUNT_CONST_VALUES == 9, "");
#define const_value_int(v)  ((Const_Value) {.kind = CONST_VALUE_INT, .as.integer = (v)})
#define const_value_fn(v)   ((Const_Value) {.kind = CONST_VALUE_FN, .as.fn = (v)})
#define const_value_type(v) ((Const_Value) {.kind = CONST_VALUE_TYPE, .as.type = (v)})

#define const_value_struct(v) ((Const_Value) {.kind = CONST_VALUE_STRUCT, .as.structt = (v)})
#define const_value_union(v)  ((Const_Value) {.kind = CONST_VALUE_UNION, .as.unionn = (v)})

#define const_value_array(v)  ((Const_Value) {.kind = CONST_VALUE_ARRAY, .as.array = (v)})
#define const_value_string(v) ((Const_Value) {.kind = CONST_VALUE_STRING, .as.string = (v)})
#define const_value_any(v)    ((Const_Value) {.kind = CONST_VALUE_ANY, .as.any = (v)})

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
    NODE_UNARY,
    NODE_BINARY,
    NODE_MEMBER,
    NODE_ASSERT,
    NODE_IMPORT,
    NODE_DISTINCT,

    NODE_FN,
    NODE_ENUM,
    NODE_UNION,
    NODE_STRUCT,
    NODE_COMPOUND,

    NODE_CALL,
    NODE_INDEX,
    NODE_INDEXABLE,

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
    COUNT_NODES
} Node_Kind;

typedef enum {
    AUTO_CAST_TO_ANY,
    AUTO_CAST_TO_UNION,
    AUTO_CAST_ARRAY_TO_SLICE,
    COUNT_AUTO_CASTS
} Auto_Cast_Kind;

struct Node {
    Node_Kind kind;
    Token     token;
    Type      type;
    Node     *next;

    Type *emit_type_info;

    Type          *auto_cast_from;
    Auto_Cast_Kind auto_cast_kind;
    size_t         auto_cast_data;
};

Node *node_alloc(Arena *a, Node_Kind kind, Token token);
Node *node_iter(Node *it, Node *ll);

// When the concrete type of an abstract value is resolved inside conditional statements
//
// This is only used for compile time if statements and switch statements. During runtime, it can be done simply using
// ghost definitions in the stack-like lexical scope, and thus does not require use of this.
struct Context_Replace {
    Node_Atom *from;
    Node_Atom *to;

    Context_Replace *outer;
};

typedef struct {
    bool is_local;
    bool is_extern;
    bool is_private;
    bool is_assigned;

    Node_Fn *static_var_fn;

    // This is 0 for variables which are not arguments. For arguments, counting starts from 1
    size_t arg_index;

    // For multiple definition
    size_t group_index;

    Node_Define *definition_node;
    Node        *assignment_node;

    Context_Fn      *fn_context;
    Context_Replace *replace_context;

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

    bool         is_ghost;
    LLVMValueRef ghost_llvm;
};

typedef struct {
    Node   node;
    Nodes  nodes;
    size_t count;
} Node_Group;

typedef struct {
    Node  node;
    Node *value;
    bool  is_postfix; // For '++' and '--'
} Node_Unary;

typedef struct {
    Node  node;
    Node *lhs;
    Node *rhs;

    Node *any_check;
    Type *any_check_type;

    Node  *union_check;
    size_t union_check_index;
} Node_Binary;

typedef struct {
    Node  node;
    Node *lhs;

    Token field; // Value.field
    Node *rhs;   // Union.(Type)

    union {
        size_t field_index;
        size_t enum_value;
        size_t union_index;
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
    Nodes   libraries;
} Node_Import;

typedef struct {
    Node       node;
    Node_Atom *defined_as;
    Node      *value;
} Node_Distinct;

struct Node_Fn {
    Node node;

    Nodes  args;
    size_t args_count;     // Actual
    size_t args_count_min; // Minimum

    Variadics_Kind variadics_kind;

    Nodes  returns;
    size_t returns_count;

    Node *body;

    bool is_type;
    bool is_extern;
    bool is_inline;

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
struct Node_Union {
    Node   node;
    Nodes  variants;
    size_t variants_count; // Calculated at parse time

    Node_Atom *defined_as;
    size_t     defined_as_anon_iota;

    // The module this was parsed in
    Module *module;

    Node_Fn *defined_in;
};

// This represents a type
struct Node_Struct {
    Node  node;
    Nodes fields;

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
    bool is_designated;
} Node_Compound;

typedef enum {
    TYPE_CAST_NOP,
    TYPE_CAST_NORMAL,
    TYPE_CAST_TO_BOOL,
    TYPE_CAST_TO_UNION,
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

    Node *spread;
    Pos   spread_pos;

    bool   do_not_allocate_typed_variadic_array;
    size_t typed_variadics_array_count;

    bool      is_type_cast;
    Type_Cast type_cast;
    size_t    type_cast_union_index;

    bool is_stmt;
} Node_Call;

typedef struct {
    Node  node;
    Node *lhs;
    Node *a;
    Node *b;
    bool  is_ranged;
} Node_Index;

// This represents a type
typedef struct {
    Node  node;
    Node *element;
    Node *count;
} Node_Indexable;

struct Node_Define {
    Node  node;
    Node *name;

    Node *expr;
    Node *type;

    bool has_spread;

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
    Node *compile_time_real;

    Context_Replace context_replace;
} Node_If;

typedef struct {
    Node  node;
    Node *init;
    Node *condition;
    Node *update;
    Node *body;
} Node_For;

typedef struct {
    Node   node;
    Nodes  preds;
    size_t preds_count;

    Node *body;

    Context_Replace context_replace;
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

    Node_Enum  *enumeration;
    Node_Union *unionn;

    bool is_expr_any;
    bool is_expr_type_info;

    bool       is_compile_time;
    Node_Case *compile_time_real;
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

void node_debug(FILE *f, Node *n);
void nodes_debug(FILE *f, Nodes ns);

#endif // NODE_H
