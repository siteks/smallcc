
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

// ---------------------------------------------------------------
// Tokeniser
// ---------------------------------------------------------------
typedef enum{
    TK_RESERVED,    // Reserved word
    TK_NUM,         // Number
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
        if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '(' || *p == ')')
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

} Node_kind;

typedef struct Node Node;

struct Node
{
    Node_kind   kind;
    Node        *lhs;
    Node        *rhs;
    int         val;
};

// Grammer
// expr     = mul ("+" mul | "-" mul)*
// mul      = primary ("*" primary | "/" primary)*
// primary  = num | "(" expr ")"


Node *new_node(Node_kind kind, Node *lhs, Node *rhs)
{
    Node *node  = calloc(1, sizeof(Node));
    node->kind  = kind;
    node->lhs   = lhs;
    node->rhs   = rhs;
    return node;
}

Node *new_node_num(int val)
{
    Node *node  = calloc(1, sizeof(Node));
    node->kind  = ND_NUM;
    node->val   = val;
    return node;
}

Node *expr();
Node *primary()
{
    if (consume('('))
    {
        Node *node = expr();
        expect(')');
        return node;
    }
    return new_node_num(expect_number());
}

Node *mul()
{
    Node *node = primary();
    while(1)
    {
        if (consume('*'))
            node = new_node(ND_MUL, node, primary());
        else if (consume('/'))
            node = new_node(ND_DIV, node, primary());
        else
            return node;
    }
}

Node *expr()
{
    Node *node = mul();
    while(1)
    {
        if (consume('+'))
            node = new_node(ND_ADD, node, mul());
        else if (consume('-'))
            node = new_node(ND_SUB, node, mul());
        else
            return node;
    }
}

void print_tree(Node *node, int depth)
{
    if (!node)
        return;
    for(int i = 0; i < depth; i++) 
        fprintf(stderr, "  ");
    fprintf(stderr, "Node:%d", node->kind);
    if (node->kind == ND_NUM)
        fprintf(stderr, " val:%d\n", node->val);
    else 
        fprintf(stderr,"\n");
    if (node->lhs)
    {
        print_tree(node->lhs, depth+1);
        print_tree(node->rhs, depth+1);
    }
}


void gen_pushi(int val)
{
    if (val & 0xff00) 
    {
        printf("    ldih    r0 %02x\n", val >> 8);
        printf("    ldil    r0 %02x\n", val & 0xff);
    }
    else
        printf("    ldix    r0 %d\n", val);
    printf("    adjm2\n");
    printf("    stw     r0 r6(0)\n");
}
void gen_pushr(int r)
{
    printf("    adjm2\n");
    printf("    stw     r%d r6(0)\n", r);
}
void gen_pop(int r)
{
    printf("    ldw     r%d r6(0)\n", r);
    printf("    adj2\n");
}

void gen_add()
{
    gen_pop(1);
    gen_pop(0);
    printf("    add     r0 r0 r1\n");
    gen_pushr(0);
}
void gen_sub()
{
    gen_pop(1);
    gen_pop(0);
    printf("    sub     r0 r0 r1\n");
    gen_pushr(0);
}
void gen_mul()
{
    gen_pop(1);
    gen_pop(0);
    printf("    ldix    r2 0\n");
    printf("l3: tst     r0\n");
    printf("    jz      l1\n");
    printf("    tst     r1\n");
    printf("    jz      l1\n");
    printf("    slsr    r1\n");
    printf("    jz      l2\n");
    printf("    add     r2 r2 r0\n");
    printf("l2: slsl    r0\n");
    printf("    j       l3\n");
    printf("l1:");
    gen_pushr(2);
}

void gen_div()
{

}

void gen_code(Node *node)
{
    
    if (node->kind == ND_NUM)
    {
        gen_pushi(node->val);
        return;
    }
    gen_code(node->lhs);
    gen_code(node->rhs);
    if (node->kind == ND_ADD)
    {
        gen_add();
    }
    else if (node->kind == ND_SUB)
    {
        gen_sub();
    }
    else if (node->kind == ND_MUL)
    {
        gen_mul();
    }
    else if (node->kind == ND_DIV)
    {
        gen_div();
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

    Node *node = expr();
    print_tree(node, 0);

    printf(".text=0\nmain:\n");
    printf("    ldih    r6 0x10\n");
    printf("    ldil    r6 0x00\n");

    gen_code(node);
    gen_pop(0);
    printf("    halt\n");

    return 0;
}


