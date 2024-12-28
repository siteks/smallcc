
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mycc.h"


extern Token *token;
extern char *user_input;

struct Keyword keywords[] = 
{
    "auto",     TK_AUTO,
    "break",    TK_BREAK,
    "case",     TK_CASE,
    "char",     TK_CHAR,
    "const",    TK_CONST,
    "continue", TK_CONTINUE,
    "default",  TK_DEFAULT,
    "do",       TK_DO,
    "double",   TK_DOUBLE,
    "else",     TK_ELSE,
    "enum",     TK_ENUM,
    "extern",   TK_EXTERN,
    "float",    TK_FLOAT,
    "for",      TK_FOR,
    "goto",     TK_GOTO,
    "if",       TK_IF,
    "int",      TK_INT,
    "long",     TK_LONG,
    "register", TK_REGISTER,
    "return",   TK_RETURN,
    "short",    TK_SHORT,
    "signed",   TK_SIGNED,
    "sizeof",   TK_SIZEOF,
    "static",   TK_STATIC,
    "struct",   TK_STRUCT,
    "switch",   TK_SWITCH,
    "typedef",  TK_TYPEDEF,
    "union",    TK_UNION,
    "unsigned", TK_UNSIGNED,
    "void",     TK_VOID,
    "volatile", TK_VOLATILE,
    "while",    TK_WHILE,
    "",         TK_INVALID,
};


char *token_str(Token_kind tk)
{
    return 
        tk == TK_EMPTY      ? "EMPTY    " :
        tk == TK_IDENT      ? "IDENT    " :
        tk == TK_NUM        ? "NUM      " :
        tk == TK_LPAREN     ? "LPAREN   " :
        tk == TK_RPAREN     ? "RPAREN   " :
        tk == TK_LBRACE     ? "LBRACE   " :
        tk == TK_RBRACE     ? "RBRACE   " :
        tk == TK_LBRACKET   ? "LBRACKET " :
        tk == TK_RBRACKET   ? "RBRACKET " :
        tk == TK_COMMA      ? "COMMA    " :
        tk == TK_SEMICOLON  ? "SEMICOLON" :
        tk == TK_EQ         ? "EQ       " :
        tk == TK_NE         ? "NE       " :
        tk == TK_GE         ? "GE       " :
        tk == TK_GT         ? "GT       " :
        tk == TK_LE         ? "LE       " :
        tk == TK_LT         ? "LT       " :
        tk == TK_INC        ? "INC      " :
        tk == TK_DEC        ? "DEC      " :
        tk == TK_DOT        ? "DOT      " :
        tk == TK_ASSIGN     ? "ASSIGN   " :
        tk == TK_PLUS       ? "PLUS     " :
        tk == TK_MINUS      ? "MINUS    " :
        tk == TK_STAR       ? "STAR     " :
        tk == TK_SLASH      ? "SLASH    " :
        tk == TK_AMPERSAND  ? "AMPERSAND" :
        tk == TK_TWIDDLE    ? "TWIDDLE  " :
        tk == TK_BANG       ? "BANG     " :
        tk == TK_EOF        ? "EOF      " :
        tk == TK_AUTO       ? "auto     " :
        tk == TK_BREAK      ? "break    " :
        tk == TK_CASE       ? "case     " :
        tk == TK_CHAR       ? "char     " :
        tk == TK_CONST      ? "const    " :
        tk == TK_CONTINUE   ? "continue " :
        tk == TK_DEFAULT    ? "default  " :
        tk == TK_DO         ? "do       " :
        tk == TK_DOUBLE     ? "double   " :
        tk == TK_ELSE       ? "else     " :
        tk == TK_ENUM       ? "enum     " :
        tk == TK_EXTERN     ? "extern   " :
        tk == TK_FLOAT      ? "float    " :
        tk == TK_FOR        ? "for      " :
        tk == TK_GOTO       ? "goto     " :
        tk == TK_IF         ? "if       " :
        tk == TK_INT        ? "int      " :
        tk == TK_LONG       ? "long     " :
        tk == TK_REGISTER   ? "register " :
        tk == TK_RETURN     ? "return   " :
        tk == TK_SHORT      ? "short    " :
        tk == TK_SIGNED     ? "signed   " :
        tk == TK_SIZEOF     ? "sizeof   " :
        tk == TK_STATIC     ? "static   " :
        tk == TK_STRUCT     ? "struct   " :
        tk == TK_SWITCH     ? "switch   " :
        tk == TK_TYPEDEF    ? "typedef  " :
        tk == TK_UNION      ? "union    " :
        tk == TK_UNSIGNED   ? "unsigned " :
        tk == TK_VOID       ? "void     " :
        tk == TK_VOLATILE   ? "volatile " :
        tk == TK_WHILE      ? "while    " :
        tk == TK_INVALID    ? "INVALID  " : 
                              "UNKNOWN  ";
}
char *type_token_str(Token_kind tk)
{
    return 
        tk == TK_AUTO       ? "auto " :
        tk == TK_CHAR       ? "char " :
        tk == TK_CONST      ? "const " :
        tk == TK_DOUBLE     ? "double " :
        tk == TK_ENUM       ? "enum " :
        tk == TK_EXTERN     ? "extern " :
        tk == TK_FLOAT      ? "float " :
        tk == TK_INT        ? "int " :
        tk == TK_LONG       ? "long " :
        tk == TK_REGISTER   ? "register " :
        tk == TK_SHORT      ? "short " :
        tk == TK_SIGNED     ? "signed " :
        tk == TK_STATIC     ? "static " :
        tk == TK_STRUCT     ? "struct " :
        tk == TK_TYPEDEF    ? "typedef " :
        tk == TK_UNION      ? "union " :
        tk == TK_UNSIGNED   ? "unsigned " :
        tk == TK_VOID       ? "void " :
        tk == TK_VOLATILE   ? "volatile " :
                              "";
}

char *expect(Token_kind tk)
{
    if (token->kind == tk)
    {
        char *val   = token->val;
        token       = token->next;
        return val;
    }
    else
    {
        char space[1024];
        memset(space, 0x20, 1024);
        space[token->loc] = 0;
        fprintf(stderr, "%s\n", user_input);
        fprintf(stderr, "%s^\n", space);
        error("Expecting '%s' got '%s'\n", token_str(tk), token_str(token->kind));
    }
    return 0;
}


bool at_eof()
{
    return token->kind == TK_EOF;
}

Token *new_token(Token_kind kind, Token *cur, char *str, int len)
{
    Token *tok  = calloc(1, sizeof(Token));
    tok->kind   = kind;
    tok->val    = calloc(1, len + 1);
    tok->loc    = str - user_input;
    if (len)
        memcpy(tok->val, str, len);
    cur->next   = tok;
    return tok;
}
Token_kind find_token(char *str, int l)
{
    // fprintf(stderr, "%s %s %d\n", __func__, str, l);
    for(int i = 0; keywords[i].token != TK_INVALID; i++)
    {
        if (strlen(keywords[i].keyword) == l && !strncmp(str, keywords[i].keyword, l))
            return keywords[i].token;
    }
    return TK_IDENT;
}
Token *tokenise(char *p)
{
    Token head;
    head.next   = NULL;
    Token *cur  = &head;

    while (*p)
    {
        if (isspace(*p))
        {
            // Skip white space
            p++;
            continue;
        }
        if (strlen(p) >= 2)
        {
            // Two character tokens
            if (!strncmp(p, "==", 2)) {cur = new_token(TK_EQ, cur, p, 2); p += 2; continue;}
            if (!strncmp(p, "!=", 2)) {cur = new_token(TK_NE, cur, p, 2); p += 2; continue;}
            if (!strncmp(p, ">=", 2)) {cur = new_token(TK_GE, cur, p, 2); p += 2; continue;}
            if (!strncmp(p, "<=", 2)) {cur = new_token(TK_LE, cur, p, 2); p += 2; continue;}
            if (!strncmp(p, "++", 2)) {cur = new_token(TK_INC, cur, p, 2); p += 2; continue;}
            if (!strncmp(p, "--", 2)) {cur = new_token(TK_DEC, cur, p, 2); p += 2; continue;}
            if (!strncmp(p, "&&", 2)) {cur = new_token(TK_LOGAND, cur, p, 2); p += 2; continue;}
            if (!strncmp(p, "||", 2)) {cur = new_token(TK_LOGOR, cur, p, 2); p += 2; continue;}
        }
        // Single character tokens
        switch (*p) 
        {
            case '+': cur = new_token(TK_PLUS,      cur, p++, 1); continue;
            case '-': cur = new_token(TK_MINUS,     cur, p++, 1); continue;
            case '*': cur = new_token(TK_STAR,      cur, p++, 1); continue;
            case '/': cur = new_token(TK_SLASH,     cur, p++, 1); continue;
            case '(': cur = new_token(TK_LPAREN,    cur, p++, 1); continue;
            case ')': cur = new_token(TK_RPAREN,    cur, p++, 1); continue;
            case '[': cur = new_token(TK_LBRACKET,  cur, p++, 1); continue;
            case ']': cur = new_token(TK_RBRACKET,  cur, p++, 1); continue;
            case '>': cur = new_token(TK_GT,        cur, p++, 1); continue;
            case '<': cur = new_token(TK_LT,        cur, p++, 1); continue;
            case '=': cur = new_token(TK_ASSIGN,    cur, p++, 1); continue;
            case ';': cur = new_token(TK_SEMICOLON, cur, p++, 1); continue;
            case '{': cur = new_token(TK_LBRACE,    cur, p++, 1); continue;
            case '}': cur = new_token(TK_RBRACE,    cur, p++, 1); continue;
            case '&': cur = new_token(TK_AMPERSAND, cur, p++, 1); continue;
            case '~': cur = new_token(TK_TWIDDLE,   cur, p++, 1); continue;
            case '!': cur = new_token(TK_BANG,      cur, p++, 1); continue;
            case ',': cur = new_token(TK_COMMA,     cur, p++, 1); continue;
            case '.': cur = new_token(TK_DOT,       cur, p++, 1); continue;
            // case '&': cur = new_token(TK_BITAND,    cur, p++, 1); continue;
            case '|': cur = new_token(TK_BITOR,     cur, p++, 1); continue;
            case '^': cur = new_token(TK_BITXOR,    cur, p++, 1); continue;
        }
        // Keywords and identifiers
        if (isalpha(*p) || *p == '_')
        {
            // Scan forwards until non ident char
            char *q = p;
            int i   = 0;
            while(*q && (isalnum(*q) || *q == '_')) q++;
            int l   = q - p;
            // Keywords and identifiers
            Token_kind tk = find_token(p, l);
            cur = new_token(tk, cur, p, l);
            p = q;
            continue;
        }
        if (isdigit(*p))
        {
            char *q;
            strtol(p, &q, 0);
            cur = new_token(TK_NUM, cur, p, q - p);
            p = q;
            continue;
        }

        error("Unexpected input %s\n", p);
    }
    new_token(TK_EOF, cur, p, 0);
    return head.next;
}

void print_tokens()
{
    for(Token *p = token; p; p = p->next)
    {
        fprintf(stderr, "Kind:%s val:%s\n", token_str(p->kind), p->val);
    }
}