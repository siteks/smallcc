
#include "mycc.h"


// Grammer
// program              ::      { func_def }
// func_def             ::=     "int" ident "(" [ param_list ] ")" "{" { stmt } "}"
// param_list           ::=     "int" ident { "," "int" ident }
// stmt                 ::=     decl_stmt
//                          |   expr_stmt
//                          |   comp_stmt
//                          |   if_stmt
//                          |   while_stmt
//                          |   return_stmt
// decl_stmt            ::=     "int" ident [ "=" expr ] ";"
// comp_stmt            ::=     "{" { stmt } "}"
// expr_stmt            ::=     [ expr ] ";"
// if_stmt              ::=     "if" "(" expr ")" stmt [ "else" stmt ]
// while_stmt           ::=     "while" "(" expr ")" stmt
// return_stmt          ::=     "return" [ expr ] ";"
// expr                 ::=     assign_expr
// assign_expr          ::=     equal_expr [ "=" assign_expr ]
// equal_expr           ::=     rel_expr
//                          |   equal_expr "==" rel_expr
//                          |   equal_expr "!=" rel_expr
// rel_expr             ::=     add_expr
//                          |   rel_expr "<" add_expr
//                          |   rel_expr ">" add_expr
//                          |   rel_expr "<=" add_expr
//                          |   rel_expr ">=" add_expr
// add_expr             ::=     mult_expr
//                          |   add_expr "+" mult_expr
//                          |   add_expr "-" mult_expr
// mult_expr            ::=     primary_expr
//                          |   mult_expr "*" primary_expr
//                          |   mult_expr "/" primary_expr
// primary_expr         ::=     ident
//                          |   integer_literal
//                          |   "(" expr ")"
// ident                ::=     ident_start { letter | digit | "_" }
// ident_start          ::=     letter | "_"
// integer_literal      ::=     digit { digit }
// letter               ::=     "a" | "b" | "c" | ... | "z" | "A" | "B" | "C" | ... | "Z"
// digit                ::=     "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"

extern Token *token;
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



Node *new_node(Node_kind kind, char *val)
{
    Node *node = calloc(1, sizeof(Node));
    node->kind = kind;
    if (val)
        strncpy(node->val, val, 63);
    return node;
}

void add_child(Node *parent, Node *child)
{
    parent->child_count++;
    parent->children = (Node **)realloc(parent->children, parent->child_count * sizeof(Node *));
    parent->children[parent->child_count - 1] = child;
}


Node *primary_expr();
Node *mult_expr();
Node *add_expr();
Node *assign_expr();
Node *expr();
Node *stmt();
Node *param_list();
Node *func_def();

Node *primary_expr()
{
    Node *node;
    if (token->kind == TK_IDENT)
    {
        node = new_node(ND_IDENT, expect(TK_IDENT));
    }
    else if (token->kind == TK_NUM)
    {
        node = new_node(ND_LITERAL, expect(TK_NUM));
    }
    else
    {
        expect(TK_LPAREN);
        node = expr();
        expect(TK_RPAREN);
    }
    return node;
}
Node *mult_expr()
{
    Node *node = primary_expr();
    while (token->kind == TK_STAR || token->kind == TK_SLASH)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        enode->lhs = node;
        enode->rhs = primary_expr();
        node = enode;
    }
    return node;
}
Node *add_expr()
{
    Node *node = mult_expr();
    while (token->kind == TK_PLUS || token->kind == TK_MINUS)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        enode->lhs = node;
        enode->rhs = mult_expr();
        node = enode;
    }
    return node;
}
Node *rel_expr()
{
    Node *node = add_expr();
    while (token->kind == TK_LT || token->kind == TK_LE || token->kind == TK_GT || token->kind == TK_GE)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        enode->lhs = node;
        enode->rhs = add_expr();
        node = enode;
    }
    return node;
}
Node *equal_expr()
{
    Node *node = rel_expr();
    while (token->kind == TK_EQ || token->kind == TK_NE)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        enode->lhs = node;
        enode->rhs = rel_expr();
        node = enode;
    }
    return node;
}
Node *assign_expr()
{
    Node *node = equal_expr();
    if (token->kind == TK_ASSIGN)
    {
        Node *anode = new_node(ND_ASSIGN, expect(TK_ASSIGN));
        anode->lhs = node;
        anode->rhs = assign_expr();
        return anode;
    }
    return node;
}
Node *expr()
{
    return assign_expr();
}
Node *stmt()
{
    Node *node = new_node(ND_STMT, 0);
    if (token->kind == TK_LBRACE)
    {
        node->kind = ND_COMPSTMT;
        expect(TK_LBRACE);
        while (token->kind != TK_RBRACE)
            add_child(node, stmt());
        expect(TK_RBRACE);
    }
    else if (token->kind == TK_INT)
    {
        node->kind = ND_DECLSTMT;
        expect(TK_INT);
        add_child(node, new_node(ND_IDENT, expect(TK_IDENT)));
        if (token->kind == TK_ASSIGN)
        {
            expect(TK_ASSIGN);
            add_child(node, expr());
        }
        expect(TK_SEMICOLON);
    }
    else if (token->kind == TK_IF)
    {
        node->kind = ND_IFSTMT;
        expect(TK_IF);
        expect(TK_LPAREN);
        add_child(node, expr());
        expect(TK_RPAREN);
        add_child(node, stmt());
        if (token->kind == TK_ELSE)
        {
            expect(TK_ELSE);
            add_child(node, stmt());
        }
    }
    else if (token->kind == TK_WHILE)
    {
        node->kind = ND_WHILESTMT;
        expect(TK_WHILE);
        expect(TK_LPAREN);
        add_child(node, expr());
        expect(TK_RPAREN);
        add_child(node, stmt());
    }
    else if (token->kind == TK_RETURN)
    {
        node->kind = ND_RETURNSTMT;
        expect(TK_RETURN);
        if (token->kind != TK_SEMICOLON)
        {
            add_child(node, expr());
        }
        expect(TK_SEMICOLON);
    }
    else if (token->kind != TK_SEMICOLON)
    {
        node->kind = ND_EXPRSTMT;
        add_child(node, expr());
        expect(TK_SEMICOLON);
    }
    return node;
}
Node *param_list()
{
    Node *node = new_node(ND_PARAM_LIST, 0);
    if (token->kind == TK_INT)
    {
        expect(TK_INT);
        add_child(node, new_node(ND_IDENT, expect(TK_IDENT)));
        while(token->kind == TK_COMMA)
        {
            expect(TK_COMMA);
            expect(TK_INT);
            add_child(node, new_node(ND_IDENT, expect(TK_IDENT)));
        }
    }
    return node;
}
Node *func_def()
{
    expect(TK_INT);
    Node *node = new_node(ND_FUNCTION, expect(TK_IDENT));
    expect(TK_LPAREN);
    add_child(node, param_list());
    expect(TK_RPAREN);
    expect(TK_LBRACE);
    while(token->kind != TK_RBRACE)
        add_child(node, stmt());
    expect(TK_RBRACE);
    return node;
}
Node *program()
{
    Node *node = new_node(ND_PROGRAM, 0);
    while(!at_eof())
    {
        add_child(node, func_def());
    }
    return node;
}

char *nodestr(Node_kind k)
{
    switch(k)
    {
        case ND_PROGRAM :   return "PROGRAM     ";
        case ND_FUNCTION:   return "FUNCTION    ";
        case ND_PARAM_LIST: return "PARAM_LIST  ";
        case ND_STMT:       return "STMT        ";
        case ND_DECLSTMT:   return "DECLSTMT    ";
        case ND_EXPRSTMT:   return "EXPRSTMT    ";
        case ND_COMPSTMT:   return "COMPSTMT    ";
        case ND_IFSTMT:     return "IFSTMT      ";
        case ND_WHILESTMT:  return "WHILESTMT   ";
        case ND_RETURNSTMT: return "RETURNSTMT  ";
        case ND_EXPR:       return "EXPR        ";
        case ND_BINOP:      return "BINOP       ";
        case ND_ASSIGN:     return "ASSIGN      ";
        case ND_IDENT:      return "IDENT       ";
        case ND_LITERAL:    return "LITERAL     ";
        default:            return "unknown     ";
    }
}
void print_tree(Node *node, int depth)
{
    if (!node)
        return;
    for(int i = 0; i < depth; i++) 
        fprintf(stderr, "  ");
    fprintf(stderr, "%s: %s ch:%d\n", nodestr(node->kind), node->val, node->child_count);
    if (node->lhs)
    {
        print_tree(node->lhs, depth + 1);
        print_tree(node->rhs, depth + 1);
    }
    for(int i = 0; i < node->child_count; i++)
        print_tree(node->children[i], depth + 1);
}
void print_locals()
{
    for(Local *l = locals; l; l = l->next)
        fprintf(stderr, "Name:%-10s offset:%d\n", l->name, l->offset);
}