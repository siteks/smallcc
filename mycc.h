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
    TK_INT,
    TK_IDENT,
    TK_NUM,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE,
    TK_COMMA,
    TK_SEMICOLON,
    TK_EQ,
    TK_NE,
    TK_GE,
    TK_GT,
    TK_LE,
    TK_LT,
    TK_RETURN,
    TK_IF,
    TK_ELSE,
    TK_WHILE,
    TK_ASSIGN,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_AMPERSAND,
    TK_TWIDDLE,
    TK_BANG,
    TK_EOF,
    TK_INVALID,
} Token_kind;

typedef struct Token Token;

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
    ND_BINOP,
    ND_UNARYOP,
    ND_ASSIGN,
    ND_IDENT,
    ND_LITERAL,
} Node_kind;

typedef struct Node Node;

struct Node
{
    Node_kind   kind;
    char        val[64];
    Node        *lhs;
    Node        *rhs;
    Node        **children;
    int         child_count;
    int         offset;
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
void gen_pop(int r);
void gen_stmt(Node *node);
char *nodestr(Node_kind k);

#endif