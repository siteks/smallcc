#ifndef SMALLCC_H
#define SMALLCC_H

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void error(const char *fmt, ...);
void src_error(int line, int col, const char *fmt, ...) __attribute__((noreturn));

// ===============================================================
// Arena Allocator
// ===============================================================
// Chunked bump-pointer arena.  Grows on demand by prepending a new
// chunk of at least `chunk_size` bytes.  All memory is pre-zeroed
// (calloc'd chunks) so callers can rely on zero-initialised structs.
typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t             size;  // capacity of this chunk's data[]
    size_t             used;  // bytes consumed in this chunk
    char               data[];
} ArenaChunk;

typedef struct
{
    ArenaChunk *head;        // current chunk (allocations bump here)
    size_t      used;        // cumulative bytes allocated across all chunks
    size_t      cap;         // cumulative capacity across all chunks
    size_t      chunk_size;  // default size for a fresh chunk
} Arena;
extern Arena arena;
void  *arena_alloc(size_t size);
char  *arena_strdup(const char *s);

// Target architecture constants
#define WORD_SIZE      2   // size of int and pointer (16-bit target)
#define FRAME_OVERHEAD 8   // enter saves lr+bp (4 bytes each); params start at bp+8

// ===============================================================
// Debug Output Control
// ===============================================================

// Uncomment to enable debug output
// #define DEBUG_ENABLED 1

#ifdef DEBUG_ENABLED
    #define DBG_PRINT(...) fprintf(stderr, __VA_ARGS__)
    #define DBG_FUNC() fprintf(stderr, "%s\n", __func__)
    #define DBG_FUNC_TOKEN(tok) fprintf(stderr, "%s %s\n", __func__, (tok)->val)
#else
    #define DBG_PRINT(...) ((void)0)
    #define DBG_FUNC() ((void)0)
    #define DBG_FUNC_TOKEN(tok) ((void)0)
#endif

// ===============================================================
// Module Context Structs - groups related global state
// ===============================================================

// Forward declarations for context structs (defined at end of header)
struct TypeContext;
struct TokenContext;
struct ParserContext;
struct CodegenContext;

// ---------------------------------------------------------------
// Tokeniser
// ---------------------------------------------------------------
typedef enum
{
    TK_EMPTY, TK_IDENT, TK_CONSTFLT, TK_CONSTINT, TK_CHARACTER, TK_STRING, TK_LPAREN, TK_RPAREN, TK_LBRACE,
    TK_RBRACE, TK_LBRACKET, TK_RBRACKET, TK_COMMA, TK_SEMICOLON, TK_COLON, TK_EQ, TK_NE,
    TK_GE, TK_GT, TK_LE, TK_LT, TK_SHIFTR, TK_SHIFTL, TK_ASSIGN, TK_PLUS,
    TK_MINUS, TK_STAR, TK_SLASH, TK_AMPERSAND, TK_TILDE, TK_BANG, TK_EOF, TK_INC,
    TK_DEC, TK_DOT, TK_ARROW, TK_LOGAND, TK_LOGOR, TK_BITOR, TK_BITXOR, TK_PLUS_ASSIGN,
    TK_MINUS_ASSIGN, TK_STAR_ASSIGN, TK_SLASH_ASSIGN, TK_AMP_ASSIGN, TK_BITOR_ASSIGN, TK_BITXOR_ASSIGN, TK_SHIFTL_ASSIGN, TK_SHIFTR_ASSIGN,
    TK_PERCENT, TK_QUESTION, TK_PERCENT_ASSIGN, TK_AUTO, TK_BREAK, TK_CASE, TK_CONST, TK_CONTINUE,
    TK_DEFAULT, TK_DO, TK_ELSE, TK_EXTERN, TK_FOR, TK_GOTO, TK_IF, TK_REGISTER,
    TK_RETURN, TK_SIZEOF, TK_STATIC, TK_SWITCH, TK_VOLATILE, TK_WHILE,
// Basic types
    TK_VOID, TK_CHAR, TK_SHORT, TK_INT, TK_LONG,
    TK_FLOAT, TK_DOUBLE, TK_SIGNED, TK_UNSIGNED, TK_STRUCT, TK_UNION, TK_ENUM,
    TK_TYPEDEF, TK_INVALID, TK_ELLIPSIS,
// Variadic intrinsic keywords
    TK_VA_START, TK_VA_ARG, TK_VA_END,
// Pseudo-tokens used in Node.op_kind only; never emitted by the tokeniser
    TK_POST_INC, TK_POST_DEC,
} Token_kind;


struct Keyword
{
    char        *keyword;
    Token_kind  kind;   // renamed from 'token' to avoid conflict with token_ctx.current macro
};
typedef struct Token Token;

extern struct Keyword keywords[];

struct Token
{
    Token_kind  kind;
    Token       *next;
    char        *val;
    double      fval;
    long long   ival;
    int         loc;
    int         line;           // 1-based logical line number (original source file)
    int         col;            // 1-based column number
    const char *filename;       // original source file for this token
};

// ---------------------------------------------------------------
// Preprocessor
// ---------------------------------------------------------------
void  reset_preprocessor(void);
void  pp_define(const char *name, const char *body);
void  add_include_dir(const char *dir);
char *preprocess(const char *src, const char *filename);

char *expect(Token_kind tk);
int expect_number();
char *expect_ident();
bool at_eof();
Token *new_token(Token_kind kind, Token *cur, char *str, int len);
Token *tokenise(char *p);
void print_tokens();



// ---------------------------------------------------------------
// Parser
// ---------------------------------------------------------------
typedef enum
{
    ND_PROGRAM, ND_EXPRSTMT, ND_COMPSTMT, ND_IFSTMT, ND_WHILESTMT,
    ND_RETURNSTMT, ND_STMT, ND_BINOP, ND_UNARYOP, ND_CAST, ND_ASSIGN, ND_COMPOUND_ASSIGN,
    ND_IDENT, ND_LITERAL, ND_INITLIST, ND_DECLARATION, ND_DECLARATOR, ND_DIRECT_DECL, ND_PTYPE_LIST, ND_TYPE_NAME,
    ND_ARRAY_DECL, ND_FUNC_DECL, ND_STRUCT, ND_MEMBER, ND_FORSTMT, ND_DOWHILESTMT, ND_SWITCHSTMT,
    ND_CASESTMT, ND_DEFAULTSTMT, ND_BREAKSTMT, ND_CONTINUESTMT, ND_EMPTY, ND_LABELSTMT, ND_GOTOSTMT, ND_TERNARY,
    ND_VA_START, ND_VA_ARG, ND_VA_END,
    ND_UNDEFINED,
} Node_kind;
typedef enum
{
    CS_NONE,
    CS_U,
    CS_L,
    CS_UL,
    CS_F,
    CS_OX = 0x100,
} Const_suffix;

// Decl_spec is kept as a parser-internal accumulator for type specifier keywords.
// It is NOT the type representation — that is Type.
typedef enum
{
    DS_NONE     = 0,     // initial value; no specifiers collected yet
    DS_VOID     = 0x1,
    DS_CHAR     = 0x2,
    DS_SHORT    = 0x4,
    DS_ENUM     = 0x8,
    DS_INT      = 0x10,
    DS_LONG     = 0x20,
    DS_FLOAT    = 0x40,
    DS_DOUBLE   = 0x80,
    DS_SIGNED   = 0x100,
    DS_UNSIGNED = 0x200,
    DS_STRUCT   = 0x400,
    DS_UNION    = 0x800,
    DS_TYPEDEF  = 0x1000,
} Decl_spec;

// --------------------------------------------------------
// Type system (Type): the sole type representation.
// All types are stored once in a global linked list.
// Factory functions (get_pointer_type etc.) intern types by
// pointer equality for derived types over the same base.
typedef enum
{
    TB_VOID, TB_CHAR, TB_UCHAR, TB_SHORT, TB_USHORT, TB_INT, TB_UINT, TB_LONG,
    TB_ULONG, TB_FLOAT, TB_DOUBLE, TB_POINTER, TB_ARRAY, TB_FUNCTION, TB_STRUCT, TB_ENUM
} Type_base;

// Type qualifiers as bit flags
#define TQ_CONST    0x1
#define TQ_VOLATILE 0x2
typedef int Type_qual;

typedef struct Type Type;
typedef struct Field Field;
typedef struct Param Param;
typedef struct Symbol Symbol;

typedef enum {
    SC_NONE     = 0,   // no storage class specified
    SC_AUTO,
    SC_REGISTER,
    SC_STATIC,
    SC_EXTERN,
    SC_TYPEDEF,
} StorageClass;

// Parser-local accumulator for declaration specifiers.
// Passed as a parameter to add_types_and_symbols / type2_from_decl_node.
typedef struct {
    Decl_spec   typespec;      // accumulated type-specifier bitmask
    StorageClass sclass;       // storage class
    Type        *typedef_type; // resolved typedef type (set when DS_TYPEDEF is present)
} DeclParseState;

// Linked list of structure members
struct Field
{
    const char *name;
    Type   *type;
    int     offset;
    Field   *next;
};

// Linked list of function parameters
struct Param
{
    const char *name;
    Type   *type;
    Param   *next;
};

struct Type
{
    Type_base   base;
    Type_qual   qual;
    int         size;
    int         align;
    union
    {
        struct
        {
            Type    *pointee;
        }       ptr;
        struct
        {
            Type    *elem;
            int     count;
        }       arr;
        struct
        {
            Type    *ret;
            Param   *params;
            bool    is_variadic;
        }       fn;
        struct
        {
            Symbol  *tag;
            Field   *members;
            bool    is_union;
        }       composite;
        struct
        {
            Symbol  *tag;
        }       enu;

    }           u;
    Type        *next;
};

typedef enum
{
    ST_COMPSTMT,
    ST_STRUCT,  // also used for union scopes
} Scope_type;
typedef enum
{
    NS_IDENT,
    NS_TAG,
    NS_TYPEDEF
} Namespace;

typedef enum {
    SYM_LOCAL,         // stack-allocated local  (lea -offset)
    SYM_PARAM,         // function parameter     (lea +offset)
    SYM_GLOBAL,        // non-static global      (immw name)
    SYM_STATIC_GLOBAL, // file-scope static      (immw _s{tu}_{name})
    SYM_STATIC_LOCAL,  // local-scope static     (immw _ls{id})
    SYM_EXTERN,        // extern decl            (no data allocated)
    SYM_ENUM_CONST,    // enum constant          (offset = integer value)
    SYM_BUILTIN,       // compiler-pre-declared (putchar etc.)
} SymbolKind;

struct Symbol
{
    const char *name;
    Type   *type;
    int     offset;     // Meaning depends on symbol kind:
                        //   SYM_GLOBAL:        byte position in global data area
                        //   SYM_LOCAL:         accumulated local size (lea -(go+offset))
                        //   SYM_PARAM:         bytes above bp (lea +(go+offset))
                        //   SYM_ENUM_CONST:    integer value (not an address)
                        //   SYM_STATIC_LOCAL:  local_static_counter ID (_ls{id})
    SymbolKind kind;
    Namespace  ns;          // NS_IDENT | NS_TAG | NS_TYPEDEF
    int     tu_index;       // TU that defined a static (SYM_STATIC_GLOBAL / SYM_STATIC_LOCAL)
    bool    address_taken;  // true if &sym appears anywhere in the function (set by mark_address_taken)
    Symbol  *next;
};

typedef struct Symbol_table Symbol_table;
struct Symbol_table
{
    int             depth;       // scope nesting depth (0 = global)
    Scope_type      scope_type;  // ST_COMPSTMT or ST_STRUCT
    int             scope_id;    // monotonic ID assigned at creation (for debug)
    Symbol          *symbols;      // unified list: idents + tags + typedefs
    Symbol          *symbols_tail; // tail of symbols list (O(1) append)
    int             size;        // total bytes of locals declared in this scope
    int             local_offset; // running byte offset for the next SYM_LOCAL
    int             param_offset; // running byte offset for the next SYM_PARAM (init: FRAME_OVERHEAD)
    Symbol_table    *parent;
};


typedef struct Node Node;
struct Node
{
    Node_kind       kind;
    // Positional child slots — all Node* children live here.
    // ch[0..3] are NULL by default (arena_alloc zeroes memory).
    Node            *ch[4];
    union {
        // ND_IDENT: variable/function name + call-flag
        struct { char *name; bool is_function; } ident;
        // ND_GOTOSTMT: goto target label name
        char        *label;
        // ND_UNARYOP: flags
        struct { bool is_function; bool is_array_deref; } unaryop;
        // ND_MEMBER: field name + flags
        struct { char *field_name; bool is_function; int offset; } member;
        // ND_LABELSTMT: label name
        struct { char *name; } labelstmt;
        // ND_DECLARATOR: pointer indirection count + init (init stored in ch[1])
        struct { int pointer_level; } declarator;
        // ND_PTYPE_LIST: variadic flag
        struct { bool is_variadic; } ptype_list;
        // ND_DECLARATION: type specifier bitmask, storage class, func-defn flag, typedef type
        struct { Decl_spec typespec; StorageClass sclass; bool is_func_defn; Type *typedef_type; } declaration;
        // ND_STRUCT: union-vs-struct flag
        struct { bool is_union; } struct_spec;
        // ND_LITERAL: integer/float/string value
        struct { long long ival; double fval; char *strval; int strval_len; } literal;
        // ND_CASESTMT: case constant value + assigned label id
        struct { long long value; int label_id; } casestmt;
        // ND_DEFAULTSTMT: assigned label id
        struct { int label_id; } defaultstmt;
    } u;
    Node            *next;       // sibling link (for linked-list children)
    int             line;        // source line where this node was parsed
    int             col;         // source column
    bool            is_expr;
    Token_kind      op_kind;    // for ND_BINOP/UNARYOP/MEMBER: identifies the operator
    Symbol_table    *st;
    Symbol_table    *symtable;  // ND_COMPSTMT: its own scope.
                                // ND_FORSTMT: hidden for-init scope (NULL if init is not a decl).
                                // ND_STRUCT: the struct-member parsing scope.
    Symbol          *symbol;
    Type           *type;
    int             su_label;   // Sethi-Ullman stack-depth label (set by label_su pass)
};

Node *program();
void print_tree(Node *node, int depth);
// Walk a node's logical children. All nodes store children in u.* union fields
// or sibling linked lists (->next). fn is called for each child in declaration order.
void for_each_child(Node *node, void (*fn)(Node *child, void *ctx), void *ctx);

const char *nodestr(Node_kind k);
const char *fulltype_str(Type *t);
const char *token_str(Token_kind tk);
const char *sc_str(StorageClass sc);
bool is_type_name(Token_kind tk);

bool is_sc_spec(Token_kind tk);
bool is_typespec(Token_kind tk);
bool is_typequal(Token_kind tk);
void resolve_symbols(Node *root);
void derive_types(Node *root);
void insert_coercions(Node *root);
Node *new_node(Node_kind kind, char *val, bool is_expr);


static inline bool istype(Type *t, Type_base b) { return t && t->base == b; }

#define istype_float(t)    istype(t, TB_FLOAT)
#define istype_char(t)     istype(t, TB_CHAR)
#define istype_uchar(t)    istype(t, TB_UCHAR)
#define istype_short(t)    istype(t, TB_SHORT)
#define istype_ushort(t)   istype(t, TB_USHORT)
#define istype_enum(t)     istype(t, TB_ENUM)
#define istype_int(t)      istype(t, TB_INT)
#define istype_uint(t)     istype(t, TB_UINT)
#define istype_long(t)     istype(t, TB_LONG)
#define istype_ulong(t)    istype(t, TB_ULONG)
#define istype_ptr(t)      istype(t, TB_POINTER)
#define istype_array(t)    istype(t, TB_ARRAY)
#define istype_function(t) istype(t, TB_FUNCTION)

static inline bool istype_signed(Type *t)
{
    return t && (t->base == TB_CHAR || t->base == TB_SHORT ||
                 t->base == TB_INT  || t->base == TB_LONG);
}
static inline bool istype_fp(Type *t)
{
    return t && (t->base == TB_FLOAT || t->base == TB_DOUBLE);
}
static inline bool istype_intlike(Type *t)
{
    return t && (t->base == TB_CHAR  || t->base == TB_UCHAR  ||
                 t->base == TB_SHORT || t->base == TB_USHORT ||
                 t->base == TB_INT   || t->base == TB_UINT   ||
                 t->base == TB_LONG  || t->base == TB_ULONG  ||
                 t->base == TB_ENUM);
}

const char *get_decl_ident(Node *node);


// -----------------------------------------------------
// Interface to types.c

// Populate type table
void make_basic_types();

void reset_types_state(void);
void finalize_local_offsets(void);
void shift_param_offsets_for_struct_ret(Symbol_table *param_scope);
void insert_extern_sym(const char *name, Type *type);
void reset_parser(void);

extern int current_global_tu;

// Starting at scope of node, search for symbol in given namespace.
Symbol *find_symbol(Node *node, const char *name, Namespace nspace);

void leave_scope();
Symbol_table *enter_new_scope(void);
Symbol_table *reenter_last_scope(void);
const char *curr_scope_str();

// Build a Type* from a declaration-context node (typespec + optional declarator child).
Type *type2_from_decl_node(Node *node, DeclParseState ds);

void add_types_and_symbols(Node *node, DeclParseState ds, bool is_param, bool is_struct_member);
Type *elem_type(Type *t);
int find_offset(Type *t, char *field, Type **it);

// Array helpers
int array_dimensions(Type *t);
Type *array_elem_type(Type *t);

void print_symbol_table(Symbol_table *s, int depth);
void print_type_table();

Decl_spec to_typespec(Token_kind tk);

bool   is_typedef_name(char *name);
Type *find_typedef_type(char *name);

// Factory functions for Type
Type *get_basic_type(Type_base base);
Type *get_pointer_type(Type *pointee);
Type *get_array_type(Type *elem, int count);
Type *get_function_type(Type *ret, Param *params, bool is_variadic);
Type *get_struct_type(Symbol *tag, Field *members, bool is_union);
Type *get_enum_type(Symbol *tag);
Symbol *insert_tag(Symbol_table *st, char *ident);
Symbol *insert_enum_const(Symbol_table *st, Type *ety, char *ident, int value);
Symbol *find_symbol_st(Symbol_table *st, const char *name, Namespace nspace);

// Return the assembly label for a global/static/extern symbol.
const char *sym_label(Symbol *sym);

// ===============================================================
// Capacity constants for fixed-size arrays in context structs
#define MAX_STRLITS         512
#define MAX_LOCAL_STATICS   512
#define MAX_LABEL_TABLE     64
#define MAX_SCOPES          2048

// Context Struct Definitions (must come after type definitions)
// ===============================================================

typedef struct TypeContext {
    Type *type_list;            // linked list of all interned types
    Type *type_list_tail;       // tail of type_list (O(1) append)
    // Basic type singletons
    Type *t_void;
    Type *t_char;
    Type *t_uchar;
    Type *t_short;
    Type *t_ushort;
    Type *t_int;
    Type *t_uint;
    Type *t_long;
    Type *t_ulong;
    Type *t_float;
    Type *t_double;
    // Per-TU state
    int current_tu;
    int local_static_counter;   // NOT reset between TUs
    // Scope tracking
    int scope_depth;
    // Symbol table hierarchy
    Symbol_table *symbol_table;
    Symbol_table *curr_scope_st;
    Symbol_table *last_symbol_table;
    // Flat list of all scopes created this TU (replaces children[] tree)
    Symbol_table *scope_list[MAX_SCOPES];
    int           scope_count;
    // Type formatting buffer
    char ft_buf[512];
} TypeContext;

typedef struct TokenContext {
    Token *current;             // current token
    char *user_input;           // input source string
    const char *filename;       // current source file (set per TU)
    int last_line;              // line of most recently consumed token
    int last_col;               // col  of most recently consumed token
    // Logical (original) location — updated by tokeniser from linemarkers
    const char *logical_filename; // most recent linemarker filename
} TokenContext;

typedef struct ParserContext {
    Node *current_function;     // function being parsed
    int anon_index;             // anonymous label counter (monotonic across TUs)
} ParserContext;

// Global context instances (defined in respective .c files)
extern TypeContext type_ctx;
extern TokenContext token_ctx;
extern ParserContext parser_ctx;

// Convenience macros for basic types (for backward compatibility)
#define t_void   (type_ctx.t_void)
#define t_char   (type_ctx.t_char)
#define t_uchar  (type_ctx.t_uchar)
#define t_short  (type_ctx.t_short)
#define t_ushort (type_ctx.t_ushort)
#define t_int    (type_ctx.t_int)
#define t_uint   (type_ctx.t_uint)
#define t_long   (type_ctx.t_long)
#define t_ulong  (type_ctx.t_ulong)
#define t_float  (type_ctx.t_float)
#define t_double (type_ctx.t_double)

// Convenience alias used throughout braun.c and types.c.
#define current_global_tu (type_ctx.current_tu)

// Preprocessor
void set_include_dir(const char *dir);

#endif
