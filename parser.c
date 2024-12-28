
#include "mycc.h"

// Grammer
// program              ::      { func_def }
// func_def             ::=     "int" ident "(" [ param_list ] ")" "{" { stmt } "}"
// param_list           ::=     "int" ident { "," "int" ident }
// decl_spec            ::=     storage_class_spec
//                          |   type_spec
//                          |   type_qual
// storage_class_spec   ::=     "auto" | "regster" | "static" | "extern" | "typedef"
// type_spec            ::=     "void" 
//                          |   "char"
//                          |   "short"
//                          |   "int"
//                          |   "long"
//                          |   "float"
//                          |   "double"
//                          |   "signed"
//                          |   "unsigned"
// type_qual            ::=     "const" | "volatile"
// init_decl            ::=     declarator
//                          |   declarator "=" init
// declarator           ::=     { "*" }* direct_dcl
// direct_dcl           ::=     ident
//                          |   "(" declarator ")"
//                          |   direct_dcl "[" [ const_expr ] "]"
//                          |   direct_dcl "(" param_list ")"
//                          |   direct_dcl "(" { ident }* ")"
// declaration          ::=     { decl_spec }+ { init_decl }* ";"
// stmt                 ::=     expr_stmt
//                          |   comp_stmt
//                          |   if_stmt
//                          |   while_stmt
//                          |   return_stmt
// comp_stmt            ::=     "{" { declaration }* { stmt }* "}"
// expr_stmt            ::=     [ expr ] ";"
// if_stmt              ::=     "if" "(" expr ")" stmt [ "else" stmt ]
// while_stmt           ::=     "while" "(" expr ")" stmt
// return_stmt          ::=     "return" [ expr ] ";"
// expr                 ::=     assign_expr
// assign_expr          ::=     equal_expr 
//                          |   unary_expr [ "=" assign_expr ]
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
// mult_expr            ::=     unary_expr
//                          |   mult_expr "*" unary_expr
//                          |   mult_expr "/" unary_expr
// unary_expr           ::=     primary_expr
//                          |   '&' unary_expr
//                          |   '*' unary_expr
//                          |   '+' unary_expr
//                          |   '-' unary_expr
//                          |   '~' unary_expr
//                          |   '!' unary_expr
// primary_expr         ::=     ident
//                          |   integer_literal
//                          |   "(" expr ")"
// ident                ::=     ident_start { letter | digit | "_" }*
// ident_start          ::=     letter | "_"
// integer_literal      ::=     digit { digit }*
// letter               ::=     "a" | "b" | "c" | ... | "z" | "A" | "B" | "C" | ... | "Z"
// digit                ::=     "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"

extern Token        *token;
extern Local        *locals;
extern Type         *types;
extern Symbol_table *symbol_table;

Symbol_table        *last_symbol_table;

int depth;
int indices[100];


Symbol_table *enter_new_scope(bool use_last_scope);
void leave_scope();

void set_scope(Scope *sc)
{
    sc->depth   = depth;
    sc->indices = calloc(depth, sizeof(int));
    memcpy(sc->indices, indices, depth * sizeof(int));
}
char *scope_str(Scope sc)
{
    static char buf[1024];
    int l = 0;
    int d = sc.depth;
    l = sprintf(buf, "%d:", d);
    for(int i = 0; i < d; i++)
        l += sprintf(buf + l, "%d%c", sc.indices[i], i < d ? '.' : ' ');
    return buf;
}
Node *new_node(Node_kind kind, char *val)
{
    Node *node = calloc(1, sizeof(Node));
    node->kind = kind;
    set_scope(&node->scope);
    if (val)
        strncpy(node->val, val, 63);
    return node;
}

Node *add_child(Node *parent, Node *child)
{
    parent->child_count++;
    parent->children = (Node **)realloc(parent->children, parent->child_count * sizeof(Node *));
    parent->children[parent->child_count - 1] = child;
    return child;
}


Node *primary_expr();
Node *mult_expr();
Node *add_expr();
Node *assign_expr();
Node *expr();
Node *stmt();
// Node *param_list();
Node *func_def();

Node *primary_expr()
{
    fprintf(stderr, "%s\n", __func__);
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
bool is_postfix(Token_kind tk)
{
    return      tk == TK_LBRACKET 
            ||  tk == TK_LPAREN 
            ||  tk == TK_DOT 
            ||  tk == TK_INC 
            ||  tk == TK_DEC;
}
Node *unary_expr()
{
    fprintf(stderr, "%s\n", __func__);
    // Node *node = postfix_expr();
    Node *node = 0;
    if  (  token->kind == TK_INC || token->kind == TK_DEC 
            ||  token->kind == TK_AMPERSAND || token->kind == TK_STAR 
            ||  token->kind == TK_PLUS || token->kind == TK_MINUS)
    {
        node = new_node(ND_UNARYOP, expect(token->kind));
        add_child(node, unary_expr());
    }
    else if (token->kind == TK_IDENT || token->kind == TK_NUM || token->kind == TK_LPAREN)
    {
        node = primary_expr();
        if (node->kind == ND_IDENT)
        {
            // set the symbol
            node->symbol = find_symbol(node, node->val);
        }
    }
    // postfix-expr
    // Keep pointer to array or func ident, so we can reference later during fixup
    Symbol *s = node->symbol;
    int array_depth = 0;
    // Keep ref to primary expression node so we can mark as function call is necessary
    Node *pex_node = node;
    while(is_postfix(token->kind))
    {
        switch (token->kind)
        {
            case(TK_LBRACKET):
                // rewrite as per A.7.3.1: E1[E2] equiv *((E1) + (E2))
                // e1[e2][e3] as *(e1 + e2 * size(d2) + e3)
                {
                    if (!s)
                        error("No ident before left bracket\n");
                    expect(token->kind);
                    Node *e1    = node;
                    Node *add;
                    // Only dereference at the outer calculation, put
                    // unary '+' which has no effect in for symmetry
                    if (array_depth == s->type->dimensions - 1)
                        node    = new_node(ND_UNARYOP, "*");
                    else
                        node    = new_node(ND_UNARYOP, "+");
                    add         = add_child(node, new_node(ND_BINOP, "+"));
                    add_child(add, e1);
                    Node *mul   = new_node(ND_BINOP, "*");
                    // Get array dimension
                    if (array_depth >= s->type->dimensions)
                        error("Too many dimensions for array type %s\n", fulltype_str(s->type));
                    int mult = s->type->elem_size;
                    mult *= *s->type->elems_per_row[array_depth ];
                    array_depth++;
                    char buf[64]; 
                    sprintf(buf, "%d", mult);
                    Node *dsize     = add_child(mul, new_node(ND_LITERAL, buf));
                    add_child(mul, expr());
                    add_child(add, mul);
                    expect(TK_RBRACKET);
                    break;
                }
            case(TK_LPAREN):
                // Function call
                expect(TK_LPAREN);
                pex_node->is_function = true;
                if (token->kind != TK_RPAREN)
                    add_child(node, expr());
                expect(TK_RPAREN);
                break;
            case(TK_DOT):
                expect(token->kind);
                add_child(node, new_node(ND_IDENT, expect(TK_IDENT)));
                break;
            case(TK_INC):
            case(TK_DEC):
            default:break;
        }
    }
    return node;
}
Node *mult_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = unary_expr();
    while (token->kind == TK_STAR || token->kind == TK_SLASH)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, unary_expr());
        node = enode;
    }
    return node;
}
Node *add_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = mult_expr();
    while (token->kind == TK_PLUS || token->kind == TK_MINUS)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, mult_expr());
        node = enode;
    }
    return node;
}
Node *rel_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = add_expr();
    while (token->kind == TK_LT || token->kind == TK_LE || token->kind == TK_GT || token->kind == TK_GE)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, add_expr());
        node = enode;
    }
    return node;
}
Node *equal_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = rel_expr();
    while (token->kind == TK_EQ || token->kind == TK_NE)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, rel_expr());
        node = enode;
    }
    return node;
}
Node *bitand_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = equal_expr();
    while (token->kind == TK_AMPERSAND)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, equal_expr());
        node = enode;
    }
    return node;
}
Node *bitxor_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = bitand_expr();
    while (token->kind == TK_BITXOR)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, bitand_expr());
        node = enode;
    }
    return node;
}
Node *bitor_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = bitxor_expr();
    while (token->kind == TK_BITOR)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, bitxor_expr());
        node = enode;
    }
    return node;
}
Node *logand_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = bitor_expr();
    while (token->kind == TK_LOGAND)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, bitor_expr());
        node = enode;
    }
    return node;
}
Node *logor_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = logand_expr();
    while (token->kind == TK_LOGOR)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind));
        add_child(enode, node);
        add_child(enode, logand_expr());
        node = enode;
    }
    return node;
}
Node *assign_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = logor_expr();
    if (token->kind == TK_ASSIGN)
    {
        Node *anode = new_node(ND_ASSIGN, expect(TK_ASSIGN));
        add_child(anode, node);
        add_child(anode, logor_expr());
        return anode;
    }
    return node;
}
Node *expr()
{
    fprintf(stderr, "%s\n", __func__);
    return assign_expr();
}
bool is_sc_spec(Token_kind tk)
{
    return (tk == TK_AUTO) 
        || (tk == TK_REGISTER)
        || (tk == TK_STATIC)
        || (tk == TK_EXTERN)
        || (tk == TK_TYPEDEF);
}
bool is_typespec(Token_kind tk)
{
    return (tk == TK_VOID) 
        || (tk == TK_CHAR)
        || (tk == TK_SHORT)
        || (tk == TK_INT)
        || (tk == TK_LONG)
        || (tk == TK_FLOAT)
        || (tk == TK_DOUBLE)
        || (tk == TK_SIGNED)
        || (tk == TK_UNSIGNED);
}
bool is_typequal(Token_kind tk)
{
    return (tk == TK_CONST)
        || (tk == TK_VOLATILE);
}
Node *declarator();
Node *param_declaration()
{

    fprintf(stderr, "%s\n", __func__);
    // <parameter-list> ::= <parameter-declaration> <parameter-list-tail>*
    // <parameter-list-tail> ::= , <parameter-declaration>
    // <parameter-declaration> ::= {<declaration-specifier>}+ <declarator>
    //                           | {<declaration-specifier>}+ <abstract-declarator>
    //                           | {<declaration-specifier>}+
    Node *node = new_node(ND_DECLARATION, 0);
    // At least one decl_spec
    // TODO storage class defaults A.8.1
    while(is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind))
    {
        if (is_sc_spec(token->kind))    node->sclass = token->kind;
        if (is_typespec(token->kind))   node->typespec = token->kind;
        if (is_typequal(token->kind))   node->typequal = token->kind;
        expect(token->kind);
    }
    if (token->kind == TK_STAR || token->kind == TK_IDENT)
    {
        Node *n = add_child(node, declarator());
        // add_types_and_symbols(n);
    }
    else
        error("Expecting declarator in function param list\n");

    // At this point, we can add the symbols and types to the tables
    add_types_and_symbols(node, true);
    return node;
}
Node *param_type_list()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = new_node(ND_PTYPE_LIST, 0);
    node->symtable = enter_new_scope(false);
    while(token->kind != TK_RPAREN)
    {
        add_child(node, param_declaration());
        if (token->kind == TK_COMMA)
            expect(token->kind);
    }
    leave_scope();
    return node;
}
Node *constant_expr()
{
    fprintf(stderr, "%s\n", __func__);
    return equal_expr();
}
Node *direct_decl()
{
    fprintf(stderr, "%s\n", __func__);
    // <direct-declarator> ::= <identifier> <direct-decl-tail>*
    //                       | ( <declarator> ) <direct-decl-tail>*
    // <direct-decl-tail> ::= [ {<constant-expression>}? ]
    //                      | ( <parameter-type-list> )
    //                      | ( {<identifier>}* )
    Node *node = new_node(ND_DIRECT_DECL, 0);
    if (token->kind == TK_IDENT)
    {
        add_child(node, new_node(ND_IDENT, expect(TK_IDENT)));
    }
    else if (token->kind == TK_LPAREN)
    {
        expect(TK_LPAREN);
        add_child(node, declarator());
        expect(TK_RPAREN);
    }
    while(true)
    {
        if (token->kind == TK_LBRACKET)
        {
            add_child(node, new_node(ND_ARRAY_DECL, expect(TK_LBRACKET)));
            Node *n = node->children[node->child_count - 1];
            n->is_array = true;
            if (token->kind != TK_RBRACKET)
            {
                add_child(n, constant_expr());
            }
            expect(TK_RBRACKET);
        }
        else if (token->kind == TK_LPAREN)
        {
            add_child(node, new_node(ND_FUNC_DECL, expect(TK_LPAREN)));
            Node *n = node->children[node->child_count - 1];
            n->is_function = true;
            // TODO can also be identifier
            add_child(n, param_type_list());
            expect(TK_RPAREN);
        }
        else
            break;
    }
    return node;
}
Node *declarator()
{
    fprintf(stderr, "%s\n", __func__);
    // <declarator> ::= {<pointer>}? <direct-declarator>
    // <pointer> ::= * {<type-qualifier>}* {<pointer>}?
    // <type-qualifier> ::= const
    //                    | volatile
    Node *node = new_node(ND_DECLARATOR, 0);
    node->pointer_level = 0;
    while(token->kind == TK_STAR)
    {
        expect(TK_STAR);
        node->pointer_level++;
        if (is_typequal(token->kind))
        {
            // TODO record this somehow
            expect(token->kind);
        }
    }
    add_child(node, direct_decl());
    return node;
}
Node *initializer();
Node *initializer_list()
{
    Node *node = new_node(ND_INITLIST, 0);
    add_child(node, initializer());
    while (token->kind == TK_COMMA)
    {
        expect(TK_COMMA);
        add_child(node, initializer());
    }
    return node;
}
Node *initializer()
{
    fprintf(stderr, "%s\n", __func__);
    // <initializer> ::= <assignment-expression>
    //                 | { <initializer-list> }
    //                 | { <initializer-list> , }  
    Node *node;
    if (token->kind == TK_LBRACE)
    {
        expect(token->kind);
        node = initializer_list();
        if (token->kind == TK_COMMA)
            expect(token->kind);
        expect(TK_RBRACE);
    }
    else
        node = assign_expr();
    return node;
}
Node *init_declarator()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node;
    node = declarator();
    if (token->kind == TK_ASSIGN)
    {
        expect(TK_ASSIGN);
        add_child(node, initializer());
    }
    return node;
}
Node *comp_stmt(bool use_last_scope);
Node *declaration()
{
    fprintf(stderr, "%s\n", __func__);
    // <declaration> ::=  {<declaration-specifier>}+ {<init-declarator>}* ;
    Node *node = new_node(ND_DECLARATION, 0);
    // At least one decl_spec
    // TODO storage class defaults A.8.1
    while(is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind))
    {
        if (is_sc_spec(token->kind))    node->sclass = token->kind;
        if (is_typespec(token->kind))   node->typespec = token->kind;
        if (is_typequal(token->kind))   node->typequal = token->kind;
        expect(token->kind);
    }
    while(token->kind != TK_SEMICOLON)
    {
        add_child(node, init_declarator());
        if (token->kind != TK_SEMICOLON)
        {
            // We could get this far to find out this is a function definition.
            // If the next token is a '{' and we are not in a struct, union, enum, then
            // it is a func definition
            if (token->kind == TK_LBRACE)
            {
                add_types_and_symbols(node, false);
                // This is the first compound statement of a 
                // function, so we need to use the scope
                /// created in the parameter list
                add_child(node, comp_stmt(true));
                node->is_func_defn = true;
                return node;
            }
            expect(TK_COMMA);
        }
    }

    expect(TK_SEMICOLON);
    // At this point, we can add the symbols and types to the tables
    add_types_and_symbols(node, false);
    return node;
}
extern Symbol_table *curr_st_scope;
Symbol_table *new_st_scope()
{
    // Add a new scope to the symbol table
    curr_st_scope->child_count++;
    curr_st_scope->children = (Symbol_table **)realloc(curr_st_scope->children, 
                                curr_st_scope->child_count * sizeof(Symbol_table));
    Symbol_table *nt = calloc(1, sizeof(Symbol_table));
    curr_st_scope->children[curr_st_scope->child_count - 1] = nt;
    Scope *sc       = calloc(1, sizeof(Scope));
    set_scope(sc);
    nt->scope       = *sc;
    nt->parent      = curr_st_scope;
    curr_st_scope   = nt;
    return curr_st_scope;
}
Symbol_table *enter_new_scope(bool use_last_scope)
{
    if (use_last_scope)
    {
        depth++;
        curr_st_scope = last_symbol_table;
        return last_symbol_table;
    }
    indices[depth++]++;
    indices[depth]      = 0;
    last_symbol_table   = new_st_scope();
    curr_st_scope       = last_symbol_table;
    return last_symbol_table;
}
void leave_scope()
{
    depth--;
    curr_st_scope = curr_st_scope->parent;
}
Node *comp_stmt(bool use_last_scope)
{
    fprintf(stderr, "%s\n", __func__);
    Node *node      = new_node(ND_COMPSTMT, 0);
    node->symtable  = enter_new_scope(use_last_scope);
    fprintf(stderr, "%2d:", depth); 
    for(int i = 0; i < depth; i++)
        fprintf(stderr,"%2d.", indices[i]);
    fprintf(stderr, "\n");
    if (token->kind == TK_LBRACE)
    {
        // <compound-statement> ::= { {<declaration>}* {<statement>}* }
        expect(TK_LBRACE);
        while (is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind))
        {
            // TODO Add defined types
            add_child(node, declaration());
        }
        while (token->kind != TK_RBRACE)
            add_child(node, stmt());
        expect(TK_RBRACE);
    }
    leave_scope();
    return node;
}
Node *stmt()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = new_node(ND_STMT, 0);
    if (token->kind == TK_LBRACE)
    {
        add_child(node, comp_stmt(false));
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
// Node *param_list()
// {
//     fprintf(stderr, "%s\n", __func__);
//     Node *node = new_node(ND_PARAM_LIST, 0);
//     if (token->kind == TK_INT)
//     {
//         expect(TK_INT);
//         add_child(node, new_node(ND_IDENT, expect(TK_IDENT)));
//         while(token->kind == TK_COMMA)
//         {
//             expect(TK_COMMA);
//             expect(TK_INT);
//             add_child(node, new_node(ND_IDENT, expect(TK_IDENT)));
//         }
//     }
//     return node;
// }
// Node *func_def()
// {
//     fprintf(stderr, "%s\n", __func__);
//     expect(TK_INT);
//     Node *node = new_node(ND_FUNCTION, expect(TK_IDENT));
//     expect(TK_LPAREN);
//     add_child(node, param_list());
//     expect(TK_RPAREN);
//     add_child(node, comp_stmt());
//     return node;
// }
Node *program()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = new_node(ND_PROGRAM, 0);
    while(!at_eof())
    {

        add_child(node, declaration());
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
        case ND_CONSTEXPR:  return "CONSTEXPR   ";
        case ND_BINOP:      return "BINOP       ";
        case ND_UNARYOP:    return "UNARYOP     ";
        case ND_ASSIGN:     return "ASSIGN      ";
        case ND_IDENT:      return "IDENT       ";
        case ND_INITLIST:   return "INITLIST    ";
        case ND_LITERAL:    return "LITERAL     ";
        case ND_DECLARATION:return "DECLARATION ";
        case ND_DECLARATOR: return "DECLARATOR  ";
        case ND_DIRECT_DECL:return "DIRECT_DECL ";
        case ND_PTYPE_LIST: return "PTYPE_LIST  ";
        case ND_ARRAY_DECL: return "ARRAY_DECL  ";
        case ND_FUNC_DECL:  return "FUNC_DECL   ";
        case ND_UNDEFINED:  return "##FIXME##   ";
        default:            return "unknown     ";
    }
}

// This is lifted from K&R
char ts[1024];
char vn[1024];
void dirdcl(Node *node);
void dcl(Node *node)
{
    dirdcl(node->children[0]);
    for(int i = 0; i < node->pointer_level; i++)
        strcat(ts, " pointer to");
}
void dirdcl(Node *node)
{
    if (node->child_count && node->children[0]->kind == ND_DECLARATOR)
    {
        dcl(node->children[0]);
    }
    if (node->kind == ND_DIRECT_DECL)
    {
        for(int i = 0; i < node->child_count; i++)
        {
            Node *n = node->children[i];
            if (n->is_array)
            {
                char b[64];
                if (n->child_count == 1 && n->children[0]->kind == ND_LITERAL)
                    sprintf(b, " array %s of", n->children[0]->val);
                else
                    sprintf(b, " array of");
                strcat(ts, b);
            }
            else if (n->is_function)
            {
                strcat(ts, " function returning");
            }
        }
    }
}
char *typestr(Node *node)
{
    ts[0] = 0;
    if (node->kind == ND_DECLARATOR)
        dcl(node);
    return ts;
}
char *fulltype_str(Type *t)
{
    static char buf[1024];
    sprintf(buf, "%s%s%s%s", type_token_str(t->sclass), 
            type_token_str(t->typequal), type_token_str(t->typespec), t->derived);
    return buf;
}

void dd(Node *node);
void d(Node *node)
{
    dd(node->children[0]);
    for(int i = 0; i < node->pointer_level; i++)
        strcat(ts, "*");
}
void dd(Node *node)
{
    if (node->child_count && node->children[0]->kind == ND_DECLARATOR)
    {
        d(node->children[0]);
    }
    if (node->kind == ND_DIRECT_DECL)
    {
        for(int i = 0; i < node->child_count; i++)
        {
            Node *n = node->children[i];
            if (n->is_array)
            {
                char b[64];
                if (n->child_count == 1 && n->children[0]->kind == ND_LITERAL)
                    sprintf(b, "[%s]", n->children[0]->val);
                else
                    sprintf(b, "[]");
                strcat(ts, b);
            }
            else if (n->is_function)
            {
                strcat(ts, "()");
            }
        }
    }
}
char *tstr_compact(Node *node)
{
    ts[0] = 0;
    if (node->kind == ND_DECLARATOR)
        d(node);
    return ts;
}
void print_tree(Node *node, int depth)
{
    if (!node)
        return;
    for(int i = 0; i < depth; i++) 
        fprintf(stderr, "  ");
    fprintf(stderr, "%s: %5s %s ch:%d sc:%s ", nodestr(node->kind), 
        node->is_array ? "array" : node->is_func_defn ? "fdef " : node->is_function ? "func " : "     ", 
        node->val, node->child_count, scope_str(node->scope));
    if (node->kind == ND_DECLARATION)
    {
        fprintf(stderr, "%s %s %s ", 
            token_str(node->sclass), token_str(node->typespec), token_str(node->typequal));
        for(int j = 0; j < node->child_count; j++)
            if (node->children[j]->kind == ND_DECLARATOR)
                fprintf(stderr, "%s %s <%s> | ", 
                    typestr(node->children[j]), 
                    token_str(node->typespec),
                    tstr_compact(node->children[j]));
    }
    if (node->kind == ND_DECLARATOR || node->kind == ND_DIRECT_DECL)
    {
        fprintf(stderr, "%s %d* %s ", node->val, node->pointer_level, node->is_function ? "func" : node->is_array ? "array" : "");
        if (node->is_array) fprintf(stderr, "%d", node->array_size);
    }
    fprintf(stderr, "\n");
    for(int i = 0; i < node->child_count; i++)
        print_tree(node->children[i], depth + 1);
}




char *get_typename(Node *node)
{
    // TODO get the struct, union, enum, or typedef identifier
    return "";
}
Type *new_type(Node *node, Node *child)
{
    // Construct a new type, the node must be a ND_DECLARATION
    Type *ty        = calloc(1, sizeof(Type));
    ty->typespec    = node->typespec;
    ty->typequal    = node->typequal;
    ty->sclass      = node->sclass;
    if (ty->typespec == TK_STRUCT || ty->typespec == TK_UNION || ty->typespec == TK_ENUM || ty->typespec == TK_TYPEDEF)
    {
        ty->name    = get_typename(node);
    }

    return ty;
}
bool is_named_type(Token_kind tk)
{
    return      tk == TK_STRUCT 
            ||  tk == TK_UNION 
            ||  tk == TK_ENUM 
            ||  tk == TK_TYPEDEF;
}
void calc_tsize(Type *t)
{
    // To work out what size a type is:
    //
    //  If there are no characters, the size is the base type size.
    //
    //  If first character is '*' this is a pointer, all other
    //  characters do not change the size, which is 2.
    //
    //  If first character is '[' this is an array. The number of
    //  elements will be the product of all the constants inside 
    //  successive brackets []. If a pointer '*' is encountered,
    //  brackets after are ignored, the elem size is 2, otherwise
    //  the elem size is the base type size
    int s = 0;
    switch(t->typespec)
    {
        case TK_VOID:       s = 0; break;
        case TK_CHAR:       s = 1; break;
        case TK_SHORT:      s = 2; break;
        case TK_INT:        s = 2; break;
        case TK_LONG:       s = 4; break;
        case TK_FLOAT:      s = 4; break;
        case TK_DOUBLE:     s = 4; break;
        case TK_SIGNED:     s = 2; break;
        case TK_UNSIGNED:   s = 2; break;
        default:            s = 2; break;
    }
    if (!t->derived[0])
    {
        // Base type
        t->size         = s;
        t->elem_size    = t->size;
    }
    else if (t->derived[0] == '*')
    {
        // This is a pointer
        t->is_pointer   = true;
        t->size         = 2;
        t->elem_size    = t->size;
    }
    else if (t->derived[0] == '(')
    {
        // This is a pointer
        t->is_function  = true;
        t->size         = 2;
        t->elem_size    = t->size;
    }
    else if (t->derived[0] == '[')
    {
        // This is an array, work out the dimensions
        t->is_array     = true;
        char *p         = t->derived;
        t->dimensions   = 0;
        while(*p && *p != '*')
        {
            if (*p == '[')
            {
                p++;
                char *q;
                int d = strtol(p, &q, 0);
                if (!t->dimensions)
                    t->elements = d;
                else
                    t->elements *= d;
                t->dimensions++;
                t->dim_sizes = (int**)realloc(t->dim_sizes, t->dimensions * sizeof(int**));
                t->dim_sizes[t->dimensions - 1] = calloc(1, sizeof(int));
                *t->dim_sizes[t->dimensions - 1] = d;

                t->elems_per_row = (int**)realloc(t->elems_per_row, t->dimensions * sizeof(int**));
                t->elems_per_row[t->dimensions - 1] = calloc(1, sizeof(int));
                if (*q == ']')
                    p = q;
            }
            p++;
        }
        if (p && *p == '*')
            t->elem_size = 4;
        else
            t->elem_size = s;
        t->size = t->elements * t->elem_size;
        int num_elems = 1;
        for(int i = t->dimensions - 1; i >= 0; i--)
        {
            *t->elems_per_row[i] = num_elems;
            num_elems *= *t->dim_sizes[i];
        }
    }
}
Type *insert_type(Node *node, char *ts)
{
    // Search type table, return location if found, otherwise insert
    for(Type *t = types; t; t = t->next)
    {
        if (    node->typespec == t->typespec
            &&  node->typequal == t->typequal
            &&  node->sclass == t->sclass
            &&  !strcmp(ts, t->derived))
        {
            // Check if named type is the same
            if (is_named_type(t->typespec))
            {
                // TODO
            }
            // if we're here, we have found a matching type, so return
            return t;
        }
    }
    // Didn't find type in table, create a new one
    Type *t     = calloc(1, sizeof(Type));
    t->typespec = node->typespec;
    t->typequal = node->typequal;
    t->sclass   = node->sclass;
    t->derived  = calloc(1, strlen(ts) + 1);
    strcpy(t->derived, ts);
    // Work out the dimensions and sizes
    calc_tsize(t);
    t->next     = types;
    types       = t;
    return t;
}
Symbol *new_symbol(Type *type, char *ident, int offset)
{
    Symbol *n = calloc(1, sizeof(Symbol));
    n->name     = ident;
    n->type     = type;
    n->offset   = offset;
    return n;
}
int align(int val, int size)
{
    if (size == 2) return (val & 1) ? (val & ~1) + 2 : val;
    if (size == 4) return (val & 3) ? (val & ~3) + 4 : val;
    return val;
}
Symbol *insert_symbol(Node *node, Type *type, char *ident, bool is_param)
{
    fprintf(stderr, "%s %s %s\n", __func__, fulltype_str(type), ident);
    // If we are inserting a symbol that is a parameter, we need to use a 
    // different offset, since this has been pushed onto the stack. We need to
    // insert the symbols backwards, while pushing them onto the stack in 
    // forwards order. Parameters can only occur at scope depth 1.


    Scope sc = node->scope;
    // Find scope in table, the scope structure already exists from the parse process
    Symbol_table *st = symbol_table;
    int go = 0; 
    // TODO refactor this, unify repeated logic
    if (sc.depth)
    {
        for(int d = 1; d <= sc.depth; d++)
        {
            if (d > 1)
                // Don't include size from globals
                go += st->size;
            int index = sc.indices[d - 1] - 1;
            st = st->children[index];
        }
    }
    else
    {
        // Treat global scope differently, we are not allocating downwards in stack space
        // but upwards in data space
        int offset  = st->size;
        st->size += type->size;
        Symbol *n   = new_symbol(type, ident, offset);
        Symbol *s = 0, *ls = 0;
        for(s = st->symbols; s; s = s->next)
            ls      = s;
        if (!ls)
            st->symbols = n;
        else
            ls->next = n;

        return n;
    }
    st->global_offset = go;
    // We are now at the correct scope, insert the symbol at the end of the list,
    // with offset within the current scope. This is the size of the current type,
    // added to the previous offset for local scopes. (TODO alignment)

    // Global offset is offset within the function, whatever scope level we are at.
    // This is the enclosing scopes
    Symbol *s = 0, *ls = 0;
    int offset          = 0;
    int param_offset    = 4; // There needs to be space for the link and base regs
    int last_size = 0;
    for(s = st->symbols; s; s = s->next)
    {
        if (s->is_param)
        {
            param_offset = s->offset;
            last_size = s->type->size;
        }
        else
            offset  = s->offset;
        ls      = s;
    }
    // ls now points to last symbol in list
    // Move offset to make space for this type
    offset          += type->size;
    param_offset    += last_size;
    if (!is_param)
        // Size is used for allocating space on stack for this
        // scope, we don't include parameters
        st->size        += type->size;
    Symbol *n       = new_symbol(type, ident, is_param ? param_offset : offset);
    n->is_param     = is_param;
    if (!ls)
        st->symbols = n;
    else
        ls->next    = n;
    return n;
}
char *get_decl_ident(Node *node)
{
    if (node->child_count)
    {
        if (node->children[0]->kind == ND_IDENT)
            return node->children[0]->val;
        return get_decl_ident(node->children[0]);
    }
    return 0;
}
void add_types_and_symbols(Node *node, bool is_param)
{
    // This is a declaration node, get all the derived types and 
    // variables. Put variable in symbol table structured to represent scope.
    // Put type in type table.

    char *ts = 0;
    char *ident = 0;
    for(int i = 0; i < node->child_count; i++)
    {
        Node *n = node->children[i];
        if (n->kind == ND_DECLARATOR)
        {
            ts      = tstr_compact(n);
            ident   = get_decl_ident(n);
        }
        if (ts && ident)
        {
            // Its an actual declarator
            fprintf(stderr, "Found %s %s\n", ts, ident);
            Type *ty = insert_type(node, ts);
            // ty is pointer to type in type table
            n->symbol = insert_symbol(node, ty, ident, is_param);
        }
    }
}
Symbol_table *find_scope(Node *node)
{

    // Get the offset from bp of the local variable in node.
    // Go to scope in node, then search for variable name in that scope,
    // and successively enclosing scopes, until found.
    Scope sc = node->scope;
    fprintf(stderr, "%s scope is %s\n", __func__, scope_str(sc));
    Symbol_table *st = symbol_table;
    if (sc.depth)
    {
        for(int d = 1; d <= sc.depth; d++)
        {
            int index = sc.indices[d - 1] - 1;
            st = st->children[index];
        }
    }
    // st now pointing to symbol table at scope.
    return st;
}

Symbol *find_symbol(Node *node, char *name)
{
    // Get the offset from bp of the local variable in node.
    // Go to scope in node, then search for variable name in that scope,
    // and successively enclosing scopes, until found.
    Symbol_table *st = find_scope(node);
    Symbol *s;
    bool found = false;
    while(!found)
    { 
        fprintf(stderr, "Searching in scope:%s\n", scope_str(st->scope));
        for(s = st->symbols; s; s = s->next)
        {
            fprintf(stderr, "Ident:%s\n", s->name);
            if (!strcmp(name, s->name))
            {
                found = true;
                break;
            }
        }
        if (found)
            break;
        // Not found in this scope, go to enclosing scope
        fprintf(stderr, "Not found, going to enclosing scope\n");
        if (st->parent)
            st = st->parent;
    }
    if (!found)
    {
        error("Symbol %s not found!\n", name);
    }
    return s;
}



void print_symbol_table(Symbol_table *s, int depth)
{
    if (depth==0)fprintf(stderr, "------ Symbol table ------\n");
    for(int i = 0; i < depth; i++) 
        fprintf(stderr, "  ");
    fprintf(stderr, "Scope:%-20s 0x%04x 0x%04x\n", scope_str(s->scope), s->size, s->global_offset);
    Symbol *sm;
    for(sm = s->symbols; sm; sm = sm->next)
    {
        for(int i = 0; i < depth; i++) 
            fprintf(stderr, "  ");
        fprintf(stderr, "%-10s %s 0x%02x %s\n", sm->name, 
            sm->is_param ? "param" : "local", sm->offset, fulltype_str(sm->type));
    }
    for(int i = 0; i < s->child_count; i++)
    {
        print_symbol_table(s->children[i], depth + 1);
    }
}
void print_type_table()
{
    Type *t;
    fprintf(stderr, "------ Type table ------\n");
    for(t = types; t; t = t->next)
    {
        fprintf(stderr, "%-20s ", fulltype_str(t));
        fprintf(stderr, "size:%d ", t->size);
        if (t->is_array)
        {
            fprintf(stderr, "%d", t->elem_size);
            for(int i = 0; i < t->dimensions; i++)
            {
                fprintf(stderr, "[%d]", *t->dim_sizes[i]);
            }
        }
        fprintf(stderr, "\n");
    }
}


void print_locals()
{
    for(Local *l = locals; l; l = l->next)
        fprintf(stderr, "Name:%-10s offset:%d\n", l->name, l->offset);
}