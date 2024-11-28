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
    TK_RESERVED,    // Reserved word
    TK_IDENT,       // Identifier
    TK_NUM,         // Number
    TK_RETURN,
    TK_EOF,
} Token_kind;

typedef struct Token Token;

struct Token
{
    Token_kind  kind;
    Token       *next;
    int         val;
    char        *str;
    int         len;
};

bool consume(char *op);
bool consume_tk(Token_kind tk);
bool is_ident();
void expect(char *op);
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
    ND_ADD, // 0 
    ND_SUB, // 1
    ND_MUL, // 2
    ND_DIV, // 3
    ND_NUM, // 4
    ND_LT,
    ND_LE,
    ND_GT,
    ND_GE,
    ND_EQ,
    ND_NE,
    ND_ASSIGN,
    ND_LOCAL,
    ND_RETURN,
} Node_kind;

typedef struct Node Node;

struct Node
{
    Node_kind   kind;
    Node        *lhs;
    Node        *rhs;
    int         val;
    char        *ident;
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

void program();
void print_tree(Node *node, int depth);
void print_locals();


// ---------------------------------------------------------------
// Code gen
// ---------------------------------------------------------------

void gen_code(Node *node);
void gen_preamble(int offset);
void gen_postamble();
void gen_pop(int r);

#endif