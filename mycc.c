
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void error(char *fmt, ...) 
{
    va_list ap;
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    exit(1);
}

char *user_input;
void error_at(char *loc, char *fmt, ...) 
{
    va_list ap;
    va_start(ap, fmt);
    int pos = loc - user_input;
    fprintf(stderr, "%s\n", user_input);
    fprintf(stderr, "%*s", pos, " ");
    fprintf(stderr, "^ ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    exit(1);
}

typedef enum{
    TK_RESERVED,
    TK_NUM,
    TK_EOF,
} Token_kind;

typedef struct Token Token;

struct Token
{
    Token_kind  kind;
    Token       *next;
    int         val;
    char        *str;
};

Token *token;

bool consume(char op)
{
    if (token->kind != TK_RESERVED || token->str[0] != op)
        return false;
    token = token->next;
    return true;
}

void expect(char op)
{
    if (token->kind != TK_RESERVED || token->str[0] != op)
        error_at(token->str, "Expecting '%c' got '%c'\n", op, token->str[0]);
    token = token->next;
}

int expect_number()
{
    if (token->kind != TK_NUM)
        error_at(token->str, "Expecting number, got kind:%d, str:%s\n", token->kind, token->str);
    int val = token->val;
    token = token->next;
    return val;
}

bool at_eof()
{
    return token->kind == TK_EOF;
}

Token *new_token(Token_kind kind, Token *cur, char *str)
{
    Token *tok  = calloc(1, sizeof(Token));
    tok->kind   = kind;
    tok->str    = str;
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
            p++;
            continue;
        }
        if (*p == '+' || *p == '-')
        {
            cur = new_token(TK_RESERVED, cur, p++);
            continue;
        }
        if (isdigit(*p))
        {
            cur = new_token(TK_NUM, cur, p);
            cur->val = (int)strtol(p, &p, 10);
            continue;
        }
        error_at(token->str, "Unexpected input\n");
    }
    new_token(TK_EOF, cur, p);
    return head.next;
}

void print_tokens()
{
    for(Token *p = token; p; p = p->next)
    {
        fprintf(stderr, "Kind:%d str:%c val:%d\n", p->kind, p->str[0], p->val);
    }
}


int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: num\n");
        return 1;
    }
    
    user_input = argv[1];
    token = tokenise(user_input);
    print_tokens();

    printf("main:\n");
    printf("    ldi     %d\n", expect_number());
    while (!at_eof())
    {
        if (consume('+'))
        {
            printf("    ldir    $1 %d\n", expect_number());
            printf("    add     $1\n");
            continue;
        }
        expect('-');
        printf("    ldir    $1 %d\n", expect_number());
        printf("    sub     $1\n");
    }
    printf("    halt\n");
    return 0;
}


