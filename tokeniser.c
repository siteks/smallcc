
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mycc.h"


extern Token *token;
extern char *user_input;


char *token_str(Token_kind tk)
{
    return 
        tk == TK_INT       ? "INT      " :
        tk == TK_IDENT     ? "IDENT    " :
        tk == TK_NUM       ? "NUM      " :
        tk == TK_LPAREN    ? "LPAREN   " :
        tk == TK_RPAREN    ? "RPAREN   " :
        tk == TK_LBRACE    ? "LBRACE   " :
        tk == TK_RBRACE    ? "RBRACE   " :
        tk == TK_COMMA     ? "COMMA    " :
        tk == TK_SEMICOLON ? "SEMICOLON" :
        tk == TK_EQ        ? "EQ       " :
        tk == TK_NE        ? "NE       " :
        tk == TK_GE        ? "GE       " :
        tk == TK_GT        ? "GT       " :
        tk == TK_LE        ? "LE       " :
        tk == TK_LT        ? "LT       " :
        tk == TK_RETURN    ? "RETURN   " :
        tk == TK_IF        ? "IF       " :
        tk == TK_ELSE      ? "ELSE     " :
        tk == TK_WHILE     ? "WHILE    " :
        tk == TK_ASSIGN    ? "ASSIGN   " :
        tk == TK_PLUS      ? "PLUS     " :
        tk == TK_MINUS     ? "MINUS    " :
        tk == TK_STAR      ? "STAR     " :
        tk == TK_SLASH     ? "SLASH    " :
        tk == TK_EOF       ? "EOF      " :
        tk == TK_INVALID   ? "INVALID  " : 
                             "UNKNOWN  ";
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
            case '>': cur = new_token(TK_GT,        cur, p++, 1); continue;
            case '<': cur = new_token(TK_LT,        cur, p++, 1); continue;
            case '=': cur = new_token(TK_ASSIGN,    cur, p++, 1); continue;
            case ';': cur = new_token(TK_SEMICOLON, cur, p++, 1); continue;
            case '{': cur = new_token(TK_LBRACE,    cur, p++, 1); continue;
            case '}': cur = new_token(TK_RBRACE,    cur, p++, 1); continue;
        }
        // Keywords and identifiers
        if (isalpha(*p) || *p == '_')
        {
            // Scan forwards until non ident char, assume no longer than 64 chars
            char *q = p;
            int i   = 0;
            while(*q && (isalnum(*q) || *q == '_')) q++;
            int l   = q - p;
            // Keywords
            if (!strncmp(p, "int", l))    {cur = new_token(TK_INT,     cur, p, l); p = q; continue;}
            if (!strncmp(p, "return", l)) {cur = new_token(TK_RETURN,  cur, p, l); p = q; continue;}
            if (!strncmp(p, "if", l))     {cur = new_token(TK_IF,      cur, p, l); p = q; continue;}
            if (!strncmp(p, "else", l))   {cur = new_token(TK_ELSE,    cur, p, l); p = q; continue;}
            if (!strncmp(p, "while", l))  {cur = new_token(TK_WHILE,   cur, p, l); p = q; continue;}
            // Must be an identifier
            cur = new_token(TK_IDENT, cur, p, l);
            p = q;
            continue;
        }
        if (isdigit(*p))
        {
            char *q;
            strtol(p, &q, 10);
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