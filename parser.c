
#include "mycc.h"


// Grammer
// program      = stmt*
// stmt         = expr ";" | "return" expr ";"
// expr         = assign
// assign       = equality ("=" assign)?
// equality     = relational ("==" relational | "!=" relational)*
// relational   = add ("<" add | "<=" add | ">" add | ">=" add)*
// add          = mul ("+" mul | "-" mul)*
// mul          = unary ("*" unary | "/" unary)*
// unary        = ("+" | "-")? primary
// primary      = num | ident | "(" expr ")"


extern Node *code[];
extern Local *locals;

Local *find_local(char *ident)
{
    for(Local *var = locals; var; var = var->next)
    {
        if (var->len == strlen(ident) && !memcmp(ident, var->name, var->len))
            return var;
    }
    return NULL;
}


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
Node *new_node_ident(char *ident)
{
    Node *node      = calloc(1, sizeof(Node));
    node->kind      = ND_LOCAL;
    Local *local    = find_local(ident);
    if (local)
    {
        node->offset = local->offset;
        printf(";Ident %s exists at offset %d\n", ident, node->offset);
    }
    else
    {
        local           = calloc(1, sizeof(Local));
        local->next     = locals;
        local->name     = ident;
        local->len      = strlen(ident);
        local->offset   = locals ? locals->offset + 1 : 0;
        node->offset    = local->offset;
        node->ident     = ident;
        locals          = local;
        printf(";Ident %s created at offset %d\n", ident, node->offset);
    }
    return node;
}

Node *expr();

Node *primary()
{
    if (consume("("))
    {
        Node *node = expr();
        expect(")");
        return node;
    }
    if (is_ident())
    {
        return new_node_ident(expect_ident());
    }
    return new_node_num(expect_number());
}
Node *unary()
{
    if (consume("+"))
        return primary();
    if (consume("-"))
        return new_node(ND_SUB, new_node_num(0), primary());
    return primary();
}
Node *mul()
{
    Node *node = unary();
    while(1)
    {
        if (consume("*"))
            node = new_node(ND_MUL, node, unary());
        else if (consume("/"))
            node = new_node(ND_DIV, node, unary());
        else
            return node;
    }
}
Node *add()
{
    Node *node = mul();
    while(1)
    {
        if (consume("+"))
            node = new_node(ND_ADD, node, mul());
        else if (consume("-"))
            node = new_node(ND_SUB, node, mul());
        else
            return node;
    }
}
Node *relational()
{
    Node *node = add();
    while(1)
    {
        if (consume("<"))
            node = new_node(ND_LT, node, add());
        else if (consume("<="))
            node = new_node(ND_LE, node, add());
        else if (consume(">"))
            node = new_node(ND_GT, node, add());
        else if (consume(">="))
            node = new_node(ND_GE, node, add());
        else
            return node;
    }
}
Node *equality()
{
    Node *node = relational();
    while(1)
    {
        if (consume("=="))
            node = new_node(ND_EQ, node, relational());
        else if (consume("=="))
            node = new_node(ND_EQ, node, relational());
        else
            return node;
    }
}
Node *assign()
{
    Node *node =equality();
    if (consume("="))
        node = new_node(ND_ASSIGN, node, assign());
    return node;
}
Node *expr()
{
    Node *node = assign();
    return node;
}
Node *stmt()
{
    Node *node;
    if (consume_tk(TK_RETURN))
    {
        node = calloc(1, sizeof(Node));
        node->kind = ND_RETURN;
        node->lhs = expr();
    }
    else
    {
        node = expr();
    }
    expect(";");
    return node;
}
void program()
{
    int i = 0;
    while(!at_eof())
        code[i++] = stmt();
    code[i] = 0;
}

char *nodestr(Node_kind k)
{
    switch(k)
    {
        case    ND_ADD: return "ND_ADD";
        case    ND_SUB: return "ND_SUB";
        case    ND_MUL: return "ND_MUL";
        case    ND_DIV: return "ND_DIV";
        case    ND_NUM: return "ND_NUM";
        case    ND_LT: return "ND_LT";
        case    ND_LE: return "ND_LE";
        case    ND_GT: return "ND_GT";
        case    ND_GE: return "ND_GE";
        case    ND_EQ: return "ND_EQ";
        case    ND_NE: return "ND_NE";
        case    ND_ASSIGN: return "ND_ASSIGN";
        case    ND_LOCAL: return "ND_LOCAL";
        case    ND_RETURN: return "ND_RETURN";
        default:;
    }
}
void print_tree(Node *node, int depth)
{
    if (!node)
        return;
    for(int i = 0; i < depth; i++) 
        fprintf(stderr, "  ");
    fprintf(stderr, "Node:%s", nodestr(node->kind));
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
void print_locals()
{
    for(Local *l = locals; l; l = l->next)
        fprintf(stderr, "Name:%-10s offset:%d\n", l->name, l->offset);
}