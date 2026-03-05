#ifndef _MYCC_H
#define _MYCC_H

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void error(char *fmt, ...);

// ---------------------------------------------------------------
// Tokeniser
// ---------------------------------------------------------------
typedef enum
{
    TK_EMPTY,
    TK_IDENT,
    TK_CONSTFLT,
    TK_CONSTINT,
    TK_CHARACTER,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_COMMA,
    TK_SEMICOLON,
    TK_COLON,
    TK_EQ,
    TK_NE,
    TK_GE,
    TK_GT,
    TK_LE,
    TK_LT,
    TK_SHIFTR,
    TK_SHIFTL,
    TK_ASSIGN,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_AMPERSAND,
    TK_TWIDDLE,
    TK_BANG,
    TK_EOF,
    TK_INC,
    TK_DEC,
    TK_DOT,
    TK_ARROW,
    TK_LOGAND,
    TK_LOGOR,
    TK_BITOR,
    TK_BITXOR,
    TK_PLUS_ASSIGN,
    TK_MINUS_ASSIGN,
    TK_STAR_ASSIGN,
    TK_SLASH_ASSIGN,
    TK_AMP_ASSIGN,
    TK_BITOR_ASSIGN,
    TK_BITXOR_ASSIGN,
    TK_SHIFTL_ASSIGN,
    TK_SHIFTR_ASSIGN,
    TK_AUTO,
    TK_BREAK,
    TK_CASE,
    TK_CONST,
    TK_CONTINUE,
    TK_DEFAULT,
    TK_DO,
    TK_ELSE,
    TK_EXTERN,
    TK_FOR,
    TK_GOTO,
    TK_IF,
    TK_REGISTER,
    TK_RETURN,
    TK_SIZEOF,
    TK_STATIC,
    TK_SWITCH,
    TK_VOLATILE,
    TK_WHILE,
// Basic types
    TK_VOID,
    TK_CHAR,
    TK_UCHAR,
    TK_SHORT,
    TK_USHORT,
    TK_INT,
    TK_UINT,
    TK_LONG,
    TK_ULONG,
    TK_FLOAT,
    TK_DOUBLE,
    TK_SIGNED,
    TK_UNSIGNED,
    TK_STRUCT,
    TK_UNION,
    TK_ENUM,
    TK_TYPEDEF,
//
    TK_INVALID,
} Token_kind;


struct Keyword
{
    char        *keyword;
    Token_kind  token;
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
};

bool consume(char *op);
bool consume_tk(Token_kind tk);
bool is_ident();
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
    ND_PROGRAM,
    ND_FUNCTION,
    ND_PARAM_LIST,
    ND_DECLSTMT,
    ND_EXPRSTMT,
    ND_COMPSTMT,
    ND_IFSTMT,
    ND_WHILESTMT,
    ND_RETURNSTMT,
    ND_STMT,
    ND_EXPR,
    ND_CONSTEXPR,
    ND_BINOP,
    ND_UNARYOP,
    ND_CAST,
    ND_ASSIGN,
    ND_IDENT,
    ND_LITERAL,
    ND_INITLIST,
    ND_DECLARATION,
    ND_DECLARATOR,
    ND_DIRECT_DECL,
    ND_PTYPE_LIST,
    ND_TYPE_NAME,
    ND_ARRAY_DECL,
    ND_FUNC_DECL,
    ND_STRUCT,
    ND_UNION,
    ND_MEMBER,
    ND_FORSTMT,
    ND_DOWHILESTMT,
    ND_SWITCHSTMT,
    ND_CASESTMT,
    ND_DEFAULTSTMT,
    ND_BREAKSTMT,
    ND_CONTINUESTMT,
    ND_EMPTY,
    ND_LABELSTMT,
    ND_GOTOSTMT,
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

// Type_base is kept as a parser-internal accumulator for type specifier keywords.
// It is NOT the type representation — that is Type2.
typedef enum
{
    TB_DERIVED  = 0,
    TB_VOID     = 0x1,
    TB_CHAR     = 0x2,
    TB_SHORT    = 0x4,
    TB_ENUM     = 0x8,
    TB_INT      = 0x10,
    TB_LONG     = 0x20,
    TB_FLOAT    = 0x40,
    TB_DOUBLE   = 0x80,
    TB_SIGNED   = 0x100,
    TB_UNSIGNED = 0x200,
    TB_STRUCT   = 0x400,
    TB_UNION    = 0x800,
} Type_base;

// --------------------------------------------------------
// Type system (Type2): the sole type representation.
// All types are stored once in a global linked list.
// Factory functions (get_pointer_type etc.) intern types by
// pointer equality for derived types over the same base.
typedef enum
{
    TB2_VOID,
    TB2_CHAR,
    TB2_UCHAR,
    TB2_SHORT,
    TB2_USHORT,
    TB2_INT,
    TB2_UINT,
    TB2_LONG,
    TB2_ULONG,
    TB2_FLOAT,
    TB2_DOUBLE,
    TB2_POINTER,
    TB2_ARRAY,
    TB2_FUNCTION,
    TB2_STRUCT,
    TB2_ENUM
} Type2_base;

// Type qualifiers as bit flags
#define TQ_CONST    0x1
#define TQ_VOLATILE 0x2
typedef int Type2_qual;

typedef struct Type2 Type2;
typedef struct Field Field;
typedef struct Param Param;
typedef struct Symbol Symbol;

// Linked list of structure members
struct Field
{
    char    *name;
    Type2   *type;
    int     offset;
    Field   *next;
};

// Linked list of function parameters
struct Param
{
    char    *name;
    Type2   *type;
    Param   *next;
};

struct Type2
{
    Type2_base   base;
    Type2_qual   qual;
    int         size;
    int         align;
    union
    {
        struct
        {
            Type2    *pointee;
        }       ptr;
        struct
        {
            Type2    *elem;
            int     count;
        }       arr;
        struct
        {
            Type2    *ret;
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
    Type2        *next;
};

typedef enum
{
    ST_COMPSTMT,
    ST_STRUCT,
} Scope_type;
typedef enum
{
    NS_IDENT,
    NS_TAG,
    NS_MEMBER,
    NS_LABEL
} Namespace;

typedef struct Scope Scope;
struct Scope
{
    int         depth;
    int         *indices;
    Scope_type  scope_type;
};

struct Symbol
{
    char    *name;
    Type2   *type;
    int     offset;
    bool    is_param;
    Symbol  *next;
};

typedef struct Symbol_table Symbol_table;
struct Symbol_table
{
    Scope           scope;
    Symbol          *idents;
    Symbol          *tags;
    Symbol          *members;
    Symbol          *labels;
    int             size;
    int             global_offset;
    int             child_count;
    Symbol_table    **children;
    Symbol_table    *parent;
};


typedef struct Node Node;
struct Node
{
    Node_kind       kind;
    char            val[64];
    long long       ival;
    double          fval;
    Node            **children;
    int             child_count;
    int             offset;
    int             pointer_level;
    int             struct_depth;
    bool            is_expr;
    bool            is_func_defn;
    bool            is_function;
    bool            is_struct;
    bool            is_array_deref;
    bool            is_array;
    int             array_size;
    bool            size_mult;
    Node            *array_ident;
    char            *typetag;
    Type_base       typespec;
    Token_kind      sclass;
    Token_kind      typequal;
    Scope           scope;
    Symbol_table    *symtable;
    Symbol          *symbol;
    Type2           *type;
};

typedef struct Local Local;
struct Local
{
    Local   *next;
    char    *name;
    int     len;
    int     offset;
};


Node *program();
void print_tree(Node *node, int depth);
void print_locals();

// ---------------------------------------------------------------
// Code gen
// ---------------------------------------------------------------

void gen_code(Node *node);
void gen_preamble(int offset);
void gen_postamble();
void gen_pop();
void gen_stmt(Node *node);
char *nodestr(Node_kind k);
char *fulltype_str(Type2 *t);
char *token_str(Token_kind tk);
bool is_type_name(Token_kind tk);

bool is_sc_spec(Token_kind tk);
bool is_typespec(Token_kind tk);
bool is_typequal(Token_kind tk);
void propagate_types(Node *p, Node *n);
char *tstr_compact(Node *node);
Node *new_node(Node_kind kind, char *val, bool is_expr);


bool istype_float(Type2 *t);
bool istype_double(Type2 *t);
bool istype_char(Type2 *t);
bool istype_uchar(Type2 *t);
bool istype_short(Type2 *t);
bool istype_ushort(Type2 *t);
bool istype_enum(Type2 *t);
bool istype_int(Type2 *t);
bool istype_uint(Type2 *t);
bool istype_long(Type2 *t);
bool istype_ulong(Type2 *t);
bool istype_ptr(Type2 *t);
bool istype_array(Type2 *t);
bool istype_function(Type2 *t);
bool istype_intlike(Type2 *t);

void unget_token();
char *get_decl_ident(Node *node);

void dcl(Node *node);
void dirdcl(Node *node);
void d(Node *node);
void dd(Node *node);

// -----------------------------------------------------
// Interface to types.c

// Populate type table
void make_basic_types();

// Starting at scope of node, search for symbol in given namespace.
Symbol *find_symbol(Node *node, char *name, Namespace nspace);

void set_scope(Scope *sc);
void leave_scope();
Symbol_table *enter_new_scope(bool use_last_scope);
char *scope_str(Scope sc);
char *curr_scope_str();

// Build a Type2* from a declaration node's typespec + a derivation string ts.
// For struct types, pass the struct's Type2* as base (or NULL for non-struct).
Type2 *type2_from_ts(Node *node, char *ts);

void add_types_and_symbols(Node *node, bool is_param, int depth);
Type2 *elem_type(Type2 *t);
int find_offset(Type2 *t, char *field, Type2 **it);

// Array helpers
int array_dimensions(Type2 *t);
Type2 *array_elem_type(Type2 *t);

void print_symbol_table(Symbol_table *s, int depth);
void print_type_table();
char *typespec_str(Type_base tb);
char *type_token_str(Token_kind tk);

Type_base to_typespec(Token_kind tk);

// Factory functions for Type2
Type2 *get_basic_type(Type2_base base);
Type2 *get_pointer_type(Type2 *pointee);
Type2 *get_array_type(Type2 *elem, int count);
Type2 *get_function_type(Type2 *ret, Param *params, bool is_variadic);
Type2 *get_struct_type(Symbol *tag, Field *members, bool is_union);

// Basic type globals
extern Type2 *t_void;
extern Type2 *t_char;
extern Type2 *t_uchar;
extern Type2 *t_short;
extern Type2 *t_ushort;
extern Type2 *t_int;
extern Type2 *t_uint;
extern Type2 *t_long;
extern Type2 *t_ulong;
extern Type2 *t_float;
extern Type2 *t_double;


#endif
