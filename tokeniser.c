
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mycc.h"


extern Token *token;

bool consume(char *op)
{
    if (token->kind != TK_RESERVED || 
        strlen(op) != token->len ||
        memcmp(token->str, op, token->len))
        return false;
    token = token->next;
    return true;
}
bool consume_tk(Token_kind tk)
{
    if (token->kind != tk)
        return false;
    token = token->next;
    return true;
}
bool is_ident()
{
    return token->kind == TK_IDENT;
}
void expect(char *op)
{
    if (token->kind != TK_RESERVED || 
        strlen(op) != token->len ||
        memcmp(token->str, op, token->len))
        error("Expecting '%s' got '%s'\n", op, token->str);
    token = token->next;
}

int expect_number()
{
    if (token->kind != TK_NUM)
        error("Expecting number, got kind:%d, str:%s\n", token->kind, token->str);
    int val = token->val;
    token = token->next;
    return val;
}

char *expect_ident()
{
    if (token->kind != TK_IDENT)
        error("Expecting ident, got kind:%d, str:%s\n", token->kind, token->str);
    char *ident = token->str;
    token = token->next;
    return ident;

}

bool at_eof()
{
    return token->kind == TK_EOF;
}

Token *new_token(Token_kind kind, Token *cur, char *str, int len)
{
    Token *tok  = calloc(1, sizeof(Token));
    tok->kind   = kind;
    tok->str    = calloc(1, len + 1);
    if (len)
        memcpy(tok->str, str, len);
    tok->len    = len;
    cur->next   = tok;
    return tok;
}
bool istkchar(char c)
{
    return isalpha(c) || c == '_';
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
            p++;
            continue;
        }
        if ((strlen(p) >= 2) && !strncmp(p, "==", 2) || !strncmp(p, "!=", 2) || !strncmp(p, ">=", 2) || !strncmp(p, "<=", 2))
        {
            cur = new_token(TK_RESERVED, cur, p, 2);
            p += 2;
            continue;
        }
        if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '(' || *p == ')' ||
            *p == '>' || *p == '<' || *p == '=' || *p == ';')
        {
            cur = new_token(TK_RESERVED, cur, p++, 1);
            continue;
        }
        if (isdigit(*p))
        {
            cur = new_token(TK_NUM, cur, p, 0);
            cur->val = (int)strtol(p, &p, 10);
            continue;
        }
        if (strlen(p) >= 6 && !strncmp(p, "return", 6) && !istkchar(p[6]))
        {
            cur = new_token(TK_RETURN, cur, p, 6);
            p += 6;
            continue;
        }
        if (isalpha(*p) || *p == '_')
        {
            fprintf(stderr, "%s\n", p);
            int len = 1;
            char *start = p;
            for(p++; *p; p++, len++)
            {
                if (!istkchar(*p))
                    break;
            }
            cur = new_token(TK_IDENT, cur, start, len);
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
        fprintf(stderr, "Kind:%d str:%s val:%d\n", p->kind, p->str, p->val);
    }
}