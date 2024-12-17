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
    TK_NUM,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_COMMA,
    TK_SEMICOLON,
    TK_EQ,
    TK_NE,
    TK_GE,
    TK_GT,
    TK_LE,
    TK_LT,
    TK_ASSIGN,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_AMPERSAND,
    TK_TWIDDLE,
    TK_BANG,
    TK_EOF,


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
    TK_SHORT,
    TK_INT,
    TK_LONG,
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
    ND_ASSIGN,
    ND_IDENT,
    ND_LITERAL,
    ND_DECLARATION,
    ND_DECLARATOR,
    ND_DIRECT_DECL,
    ND_PTYPE_LIST,
    ND_ARRAY_DECL,
    ND_FUNC_DECL,
    ND_UNDEFINED,
} Node_kind;




typedef struct Type Type;
struct Type
{
    Token_kind      typespec;
    Token_kind      typequal;
    Token_kind      sclass;
    char            *derived;
    char            *name; // for struct, union, typedef
    Type            *next;
};

typedef struct Scope Scope;
struct Scope
{
    // Represent scope as location.
    // There are depth indices. Depth 0 is the global
    // scope. Each higher depth represents a new level of
    // compound statement. Each index is the location of that 
    // compound statement within the enclosing one. Indices
    // start at 1, 0 means no compound statements
    // 
    // The symbol table is also structured like this.
    //
    //                  Depth   Indices
    //                  0       -      
    //  {1              1       1
    //      {1          2       1,1
    //          {1      3       1,1,1
    //          }
    //      }
    //  }
    //  {2
    //  }
    //  {3
    //      {1          2       3,1
    //          {1      3       3,1,1
    //          }
    //          {2      3       3,1,2
    //              {1  4       3,1,2,1
    //              }
    //          }
    //      }
    //  }
    //  {4              1       3
    //  }
    //  {5              1       4
    //  }
    //
    int     depth;
    int     *indices;
};

typedef struct Node Node;
struct Node
{
    Node_kind       kind;
    char            val[64];
    Node            **children;
    int             child_count;
    int             offset;
    int             pointer_level;
    bool            is_function;
    bool            is_array;
    int             array_size;
    Token_kind      typespec;
    Token_kind      sclass;
    Token_kind      typequal;
    Scope           scope;

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

typedef struct Symbol Symbol;
struct Symbol
{
    char    *name;
    Type    *type;
    Symbol  *next;
};

typedef struct Symbol_table Symbol_table;
struct Symbol_table
{
    // 
    Scope           scope;
    int             symbol_count;
    Symbol          *symbols;
    int             child_count;
    Symbol_table    **children;
    Symbol_table    *parent;
};

// ---------------------------------------------------------------
// Code gen
// ---------------------------------------------------------------

void gen_code(Node *node);
void gen_preamble(int offset);
void gen_postamble();
void gen_pop(int r);
void gen_stmt(Node *node);
char *nodestr(Node_kind k);
void get_types_and_symbols(Node *node);
void print_symbol_table(Symbol_table *s, int depth);
void print_type_table();

char *token_str(Token_kind tk);
char *type_token_str(Token_kind tk);


#endif