
#include "mycc.h"

// Grammer
// translation-unit ::= external-declaration*
// external-declaration ::= function-definition
//                       | declaration
// function-definition ::= declaration-specifier* declarator declaration* compound-statement
// declaration-specifier ::= storage-class-specifier
//                         | type-specifier
//                         | type-qualifier
// storage-class-specifier ::= auto
//                           | register
//                           | static
//                           | extern
//                           | typedef
// type-specifier ::= "void"
//                  | "char"
//                  | "short"
//                  | "int"
//                  | "long"
//                  | "float"
//                  | "double"
//                  | "signed"
//                  | "unsigned"
//                  | struct-or-union-specifier
//                  | enum-specifier
//                  | typedef-name
// struct-or-union-specifier ::= struct-or-union identifier "{" struct-declaration+ "}"
//                             | struct-or-union "{" struct-declaration+ "}"
//                             | struct-or-union identifier
// struct-or-union ::= "struct"
//                   | "union"
// struct-declaration ::= specifier-qualifier* struct-declarator-list
// specifier-qualifier ::= type-specifier
//                       | type-qualifier
// struct-declarator-list ::= struct-declarator ("," struct-declarator)*
// struct-declarator ::= declarator
//                     | declarator ":" constant-expression
//                     | ":" constant-expression
// declarator ::= pointer? direct-declarator
// pointer ::= "*" type-qualifier* pointer?
// type-qualifier ::= "const"
//                  | "volatile"
// direct-declarator ::= identifier direct-decl-tail*
//                     | "(" declarator ")" direct-decl-tail*
// direct-decl-tail ::= "[" constant-expression? "]"
//                    | "(" parameter-type-list ")"
//                    | "(" identifier* ")"
// declaration ::=  declaration-specifier+ init-declarator-list? ";"
// init-declarator-list ::= init-declarator
//                        | init-declarator "," init-declarator-list
// init-declarator ::= declarator
//                   | declarator "=" initializer
// initializer ::= assignment-expression
//               | "{" initializer-list "}"
//               | "{" initializer-list "," "}"
// initializer-list ::= initializer ("," initializer-list)*
// constant-expression ::= conditional-expression
// conditional-expression ::= logical-or-expression
//                          | logical-or-expression "?" expression ":" conditional-expression
// logical-or-expression ::= logical-and-expression ("||" logical-and-expression)*
// logical-and-expression ::= inclusive-or-expression ("&&" inclusive-or-expression)*
// inclusive-or-expression ::= exclusive-or-expression ("|" exclusive-or-expression)*
// exclusive-or-expression ::= and-expression ("^" and-expression)*
// and-expression ::= equality-expression ("&" equality-expression)*
// equality-expression ::= relational-expression (("=="|"!=") relational-expression)*
// relational-expression ::= shift-expression (("<"|">"|"<="|">=") shift-expression)*
// shift-expression ::= additive-expression (("<<"|">>") additive-expression)*
// additive-expression ::= multiplicative-expression (("+"|"-") multiplicative-expression)*
// multiplicative-expression ::= cast-expression (("*"|"/"|"%") cast-expression)*
// cast-expression ::= unary-expression
//                   | "(" type-name ")" cast-expression
// unary-expression ::= postfix-expression
//                    | "++" unary-expression
//                    | "--" unary-expression
//                    | unary-operator cast-expression
//                    | "sizeof" unary-expression
//                    | "sizeof" type-name
// postfix-expression ::= primary-expression postfix-tail*
// postfix-tail ::= "[" expression "]"
//                | "(" expression? ")"
//                | "." identifier
//                | "->" identifier
//                | "++"
//                | "--"
// primary-expression ::= identifier
//                      | constant
//                      | string
//                      | "(" expression ")"
// constant ::= integer-constant
//            | character-constant
//            | floating-constant
//            | enumeration-constant
// expression ::= assignment-expression ("," assignment-expression)*
// assignment-expression ::= conditional-expression
//                         | unary-expression assignment-operator assignment-expression
// assignment-operator ::= "="
//                       | "*="
//                       | "/="
//                       | "%="
//                       | "+="
//                       | "-="
//                       | "<<="
//                       | ">>="
//                       | "&="
//                       | "^="
//                       | "|="
// unary-operator ::= "&"
//                  | "*"
//                  | "+"
//                  | "-"
//                  | "~"
//                  | "!"
// parameter-type-list ::= parameter-list
//                       | parameter-list "," "..."
// parameter-list ::= parameter-declaration ("," parameter-declaration)*
// parameter-declaration ::= declaration-specifier+ declarator
//                         | declaration-specifier+ abstract-declarator
//                         | declaration-specifier+
// type-name ::= <specifier-qualifier>+ <abstract-declarator>?
// abstract-declarator ::= pointer
//                       | pointer direct-abstract-declarator
//                       | direct-abstract-declarator
// direct-abstract-declarator ::= "(" abstract-declarator ")"
//                              ("[" constant-expression? "]" | "(" parameter-type-list? ")")*
// enum-specifier ::= "enum" identifier "{" enumerator-list "}"
//                  | "enum" "{" enumerator-list "}"
//                  | "enum" identifier
// enumerator-list ::= enumerator
//                   | enumerator-list "," enumerator
// enumerator ::= identifier
//              | identifier "=" constant-expression
// typedef-name ::= identifier
// compound-statement ::= "{" declaration* statement* "}"
// statement ::= labeled-statement
//             | expression-statement
//             | compound-statement
//             | selection-statement
//             | iteration-statement
//             | jump-statement
// labeled-statement ::= identifier ":" statement
//                     | "case" constant-expression ":" statement
//                     | "default" ":" statement
// expression-statement ::= expression? ";"
// selection-statement ::= "if" "(" expression ")" statement
//                       | "if" "(" expression ")" statement "else" statement
//                       | switch "(" expression ")" statement
// iteration-statement ::= "while" "(" expression ")" statement
//                       | "do" statement "while" "(" expression ")" ";"
//                       | "for" "(" expression? ";" expression? ";" expression? ")" statement
// jump-statement ::= "goto" identifier ";"
//                  | "continue" ";"
//                  | "break" ";"
//                  | "return" expression? ";"
extern Token        *token;
extern Local        *locals;
extern Type         *types;
extern Symbol_table *symbol_table;

Symbol_table        *last_symbol_table;

int depth;
int indices[100];

Type *t_char;
Type *t_short;
Type *t_enum;
Type *t_int;
Type *t_uint;
Type *t_long;
Type *t_ulong;
Type *t_float;
Type *t_double;

void make_basic_types()
{
    Node *n = new_node(ND_CAST, 0, 0);
    n->typespec = TK_CHAR;      t_char      = insert_type(n, "");
    n->typespec = TK_SHORT;     t_short     = insert_type(n, "");
    n->typespec = TK_ENUM;      t_enum      = insert_type(n, "");
    n->typespec = TK_INT;       t_int       = insert_type(n, "");
    n->typespec = TK_UINT;      t_uint      = insert_type(n, "");
    n->typespec = TK_LONG;      t_long      = insert_type(n, "");
    n->typespec = TK_ULONG;     t_ulong     = insert_type(n, "");
    n->typespec = TK_FLOAT;     t_float     = insert_type(n, "");
    n->typespec = TK_DOUBLE;    t_double    = insert_type(n, "");
}


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
Node *new_node(Node_kind kind, char *val, bool is_expr)
{
    Node *node = calloc(1, sizeof(Node));
    node->kind = kind;
    node->is_expr = is_expr;
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

typedef enum
{
    CS_NONE,
    CS_U,
    CS_L,
    CS_UL,
    CS_F,
    CS_OX = 0x100,
} Const_suffix;
Node *primary_expr()
{
    fprintf(stderr, "%s %s \n", __func__, token->val);
    Node *node;
    if (token->kind == TK_IDENT)
    {
        node = new_node(ND_IDENT, expect(TK_IDENT), true);
    }
    else if (token->kind == TK_CONSTINT || token->kind == TK_CONSTFLT)
    {
        Token *tk = token;
        node = new_node(ND_LITERAL, expect(token->kind), true);
        int slen = strlen(tk->val);
        Const_suffix cs = CS_NONE;
        // Get the suffix
        if (tk->val[slen - 1] == 'l' || tk->val[slen - 1] == 'L')
            if (!isdigit(tk->val[slen - 2]))
                cs = CS_UL;
            else
                cs = CS_L;
        else if (tk->val[slen - 1] == 'u' || tk->val[slen - 1] == 'U')
            cs = CS_U;
        else if (tk->val[slen - 1] == 'f' || tk->val[slen - 1] == 'F')
            cs = CS_F;
        if (tk->val[0] == '0') // leading zero is octal or hex
            cs |= CS_OX;

        // node->typespec = TK_INT;

        if (tk->kind == TK_CONSTINT)
        {
            //  dec     int,            l int,  ul int
            //  hex     int,    u int,  l int,  ul int
            //  u               u int,          ul int
            //  l                       l int,  ul int
            //  ul                              ul int
            long long i = node->ival = tk->ival;
            if (cs == CS_NONE)
                if (i >= -32768 && i <= 32767)                      node->type = t_int;
                else if (i >= -2147483648ll && i <= 2147483647ll)   node->type = t_long;
                else error("Integer constant out of range");
            else if (cs == CS_OX)
                if (i >= -32768 && i <= 32767)                      node->type = t_int;
                else if (i >= 0 && i <= 65535)                      node->type = t_uint;
                else if (i >= -2147483648ll && i <= 2147483647ll)   node->type = t_long;
                else if (i >= 0 && i <= 4294967295ll)               node->type = t_ulong;
                else error("Integer constant out of range");
            else if ((cs & ~CS_OX) == CS_U)
                if (i >= 0 && i <= 65535)                           node->type = t_uint;
                else if (i >= 0 && i <= 4294967295ll)               node->type = t_ulong;
                else error("Integer constant out of range");
            else if ((cs & ~CS_OX) == CS_L)
                if (i >= -2147483648ll && i <= 2147483647ll)        node->type = t_long;
                else if (i >= 0 && i <= 4294967295ll)               node->type = t_ulong;
                else error("Integer constant out of range");
            else if ((cs & ~CS_OX) == CS_UL){
                if (i >= 0 && i <= 4294967295ll)                    node->type = t_ulong;
                else error("Integer constant out of range");}
        }
        if (tk->kind == TK_CONSTFLT)
        {
            //  Floats, double, and long float are all 32 bits
            double f = node->fval = tk->fval;
            if (f >= -3.402823466e38 && f <= 3.402823466e38)        node->type = t_float;
            else error("Integer constant out of range");
        }
        // node->type = insert_type(node, "");

    }
    else if (token->kind == TK_CHARACTER)
    {
        char c[64];
        sprintf(c, "%d", (int)token->val[0]); 
        expect(TK_CHARACTER);
        fprintf(stderr, "%s %s\n", c, token->val);
        node = new_node(ND_LITERAL, c, true);
        node->type = t_char;
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
        node = new_node(ND_UNARYOP, expect(token->kind), true);
        add_child(node, unary_expr());
    }
    else if (token->kind == TK_IDENT 
            || token->kind == TK_CONSTINT 
            || token->kind == TK_CONSTFLT 
            || token->kind == TK_CHARACTER
            || token->kind == TK_LPAREN)
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
                        node    = new_node(ND_UNARYOP, "*", true);
                    else
                        node    = new_node(ND_UNARYOP, "+", true);
                    add         = add_child(node, new_node(ND_BINOP, "+", true));
                    add_child(add, e1);
                    Node *mul   = new_node(ND_BINOP, "*", true);
                    // Get array dimension
                    if (array_depth >= s->type->dimensions)
                        error("Too many dimensions for array type %s\n", fulltype_str(s->type));
                    int mult = s->type->elem_size;
                    mult *= *s->type->elems_per_row[array_depth ];
                    array_depth++;
                    char buf[64]; 
                    sprintf(buf, "%d", mult);
                    Node *dsize     = add_child(mul, new_node(ND_LITERAL, buf, true));
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
                add_child(node, new_node(ND_IDENT, expect(TK_IDENT), true));
                break;
            case(TK_INC):
            case(TK_DEC):
            default:break;
        }
    }
    return node;
}
bool is_type_name_or_type(Token *token)
{
    return is_type_name(token->kind) || (token->kind == TK_IDENT && find_type(token->val));
}
Node *type_name()
{
    fprintf(stderr, "%s\n", __func__);
    // type-name ::= <specifier-qualifier>+ <abstract-declarator>?
    // abstract-declarator ::= pointer
    //                       | pointer direct-abstract-declarator
    //                       | direct-abstract-declarator
    // direct-abstract-declarator ::= "(" abstract-declarator ")"
    //                              ("[" constant-expression? "]" | "(" parameter-type-list? ")")*
    //
    // At least one of (void, char..., typename, const, volatile), with optional abst-decl
    // This is a strict subset of declarator
    // We are going to ignore const, volatile
    // if (is_type_name_or_type(token))
    // {
    //     return new_node(ND_TYPE_NAME, expect(token->kind));
    // }
    Node *node = new_node(ND_DECLARATION, 0, true);
    while(is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind))
    {
        if (is_sc_spec(token->kind))    node->sclass = token->kind;
        if (is_typespec(token->kind))   node->typespec = token->kind;
        if (is_typequal(token->kind))   node->typequal = token->kind;
        expect(token->kind);
    }
    while(token->kind != TK_RPAREN)
    {
        add_child(node, declarator());
    }
    // add_types_and_symbols(node, false);
    node->type = insert_type(node, tstr_compact(node));
    fprintf(stderr, "%s ts:%s:\n", __func__, tstr_compact(node));
    return node;
}
Node *cast_expr()
{
    fprintf(stderr, "%s\n", __func__);
    if (token->kind != TK_LPAREN)
    {
        return unary_expr();
    }
    // May be either a cast or a primary expr with parens eg "(" expr ")". 
    // If the ident following the current token is a type name or 
    // const|volatile, this is a cast. A cast may be followed by a cast
    expect(TK_LPAREN);
    if (is_type_name_or_type(token))
    {
        // This is a cast, create the node and add the type elements as
        // children
        Node *node = new_node(ND_CAST, 0, true);
        // while (token->kind != TK_RPAREN)
        // {
        //     add_child(node, type_name());
        // }
        add_child(node, type_name());
        // Set the type of the cast to that declared in the child
        node->type = node->children[node->child_count - 1]->type;
        expect(TK_RPAREN);
        add_child(node, cast_expr());
        return node;
    }
    else
    {
        // This is a primary expr
        Node *node = expr();
        expect(TK_RPAREN);
        return node;
    }
}
Node *mult_expr()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = cast_expr();
    while (token->kind == TK_STAR || token->kind == TK_SLASH)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
        add_child(enode, node);
        add_child(enode, cast_expr());
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
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
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
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
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
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
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
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
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
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
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
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
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
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
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
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
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
        Node *anode = new_node(ND_ASSIGN, expect(TK_ASSIGN), true);
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
        || (tk == TK_UINT)
        || (tk == TK_LONG)
        || (tk == TK_ULONG)
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
Node *param_declaration()
{

    fprintf(stderr, "%s\n", __func__);
    // <parameter-list> ::= <parameter-declaration> <parameter-list-tail>*
    // <parameter-list-tail> ::= , <parameter-declaration>
    // <parameter-declaration> ::= {<declaration-specifier>}+ <declarator>
    //                           | {<declaration-specifier>}+ <abstract-declarator>
    //                           | {<declaration-specifier>}+
    Node *node = new_node(ND_DECLARATION, 0, false);
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
    Node *node = new_node(ND_PTYPE_LIST, 0, false);
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
    Node *node = new_node(ND_DIRECT_DECL, 0, false);
    if (token->kind == TK_IDENT)
    {
        add_child(node, new_node(ND_IDENT, expect(TK_IDENT), false));
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
            add_child(node, new_node(ND_ARRAY_DECL, expect(TK_LBRACKET), false));
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
            add_child(node, new_node(ND_FUNC_DECL, expect(TK_LPAREN), false));
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
    Node *node = new_node(ND_DECLARATOR, 0, false);
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
    Node *node = new_node(ND_INITLIST, 0, false);
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
    Node *node = new_node(ND_DECLARATION, 0, false);
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
    Node *node      = new_node(ND_COMPSTMT, 0, false);
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
    Node *node = new_node(ND_STMT, 0, false);
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
    make_basic_types();
    Node *node = new_node(ND_PROGRAM, 0, false);
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
        case ND_CAST:       return "CAST        ";
        case ND_ASSIGN:     return "ASSIGN      ";
        case ND_IDENT:      return "IDENT       ";
        case ND_INITLIST:   return "INITLIST    ";
        case ND_LITERAL:    return "LITERAL     ";
        case ND_DECLARATION:return "DECLARATION ";
        case ND_DECLARATOR: return "DECLARATOR  ";
        case ND_DIRECT_DECL:return "DIRECT_DECL ";
        case ND_PTYPE_LIST: return "PTYPE_LIST  ";
        case ND_TYPE_NAME:  return "TYPE_NAME   ";
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
    if (node->child_count && node->children[0]->kind == ND_DECLARATOR)
        d(node->children[0]);
    return ts;
}
void print_tree(Node *node, int depth)
{
    if (depth==0)fprintf(stderr, "------ Parse tree ------\n");
    if (!node)
        return;
    for(int i = 0; i < depth; i++) 
        fprintf(stderr, "  ");
    fprintf(stderr, "%s: %5s %s ch:%d sc:%s fts:%s: t:%016llx ", nodestr(node->kind), 
        node->is_array ? "array" : node->is_func_defn ? "fdef " : node->is_function ? "func " : "     ", 
        node->val, node->child_count, scope_str(node->scope), node->type ? fulltype_str(node->type) : "", (unsigned long long)node->type);
    if (node->kind == ND_DECLARATION)
    {
        fprintf(stderr, "sclass:%s typequal:%s typespec:%s ", 
            type_token_str(node->sclass), type_token_str(node->typequal), type_token_str(node->typespec));
        for(int j = 0; j < node->child_count; j++)
            if (node->children[j]->kind == ND_DECLARATOR && node->children[j]->symbol)
                fprintf(stderr, "%s <%s> | ", 
                    fulltype_str(node->children[j]->symbol->type), 
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
    //  If there are no characters in the derivation, the size is the base type size.
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
    fprintf(stderr, "%s ts:%s:\n", __func__, ts);
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
            fprintf(stderr, "%sFound type\n", __func__);
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
    fprintf(stderr, "%s ts:%s: fts:%s:\n", __func__, ts, fulltype_str(t));
    t->next     = types;
    types       = t;
    return t;
}
Type *find_type(char *name)
{
    // TODO find a typedef by name
    return 0;
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

    fprintf(stderr, "%s\n", __func__);
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
        if (ts)
        {
            // Its an actual declarator
            fprintf(stderr, "Found %s %s\n", ts, ident);
            Type *ty = insert_type(node, ts);
            // ty is pointer to type in type table
            node->type = ty;
            if (ident)
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


bool istype_float(Type *t)  { return t == t_float; }
bool istype_char(Type *t)   { return t == t_char; }
bool istype_short(Type *t)  { return t == t_short; }
bool istype_enum(Type *t)   { return t == t_enum; }
bool istype_int(Type *t)    { return t == t_int; }
bool istype_uint(Type *t)   { return t == t_uint; }
bool istype_long(Type *t)   { return t == t_long; }
bool istype_ulong(Type *t)  { return t == t_ulong; }


void insert_cast(Node *n, int child, Type *t)
{
    fprintf(stderr, "%s %s\n", __func__, fulltype_str(t));
    Node *c = new_node(ND_CAST, 0, true);

    // We don't care what is in the declaration, its just a placeholder
    add_child(c, new_node(ND_DECLARATION, 0, false));
    add_child(c, n->children[child]);
    c->type = t;
    n->children[child] = c;
}
Type *check_operands(Node *n)
{
    // Perform the 'usual arithmetic conversions'
    // Check against the rules, and insert a cast to perform the conversion
    // Ignore the float promotion, we are keeping all float types the same 32 bits
    //
    Node *lhs = n->children[0];
    Node *rhs = n->children[1];
    fprintf(stderr, "%s %016llx %016llx\n", __func__, (unsigned long long)lhs->type, (unsigned long long)rhs->type);
    if (istype_float(lhs->type) && !istype_float(rhs->type))            insert_cast(n, 1, t_float);
    else if (!istype_float(lhs->type) && istype_float(rhs->type))       insert_cast(n, 0, t_float);
    if (istype_char(lhs->type))                                         insert_cast(n, 0, t_int);
    if (istype_char(rhs->type))                                         insert_cast(n, 1, t_int);
    if (istype_short(lhs->type))                                        insert_cast(n, 0, t_int);
    if (istype_short(rhs->type))                                        insert_cast(n, 1, t_int);
    if (istype_enum(lhs->type))                                         insert_cast(n, 0, t_int);
    if (istype_enum(rhs->type))                                         insert_cast(n, 1, t_int);
    if (istype_ulong(lhs->type) && !istype_ulong(rhs->type))            insert_cast(n, 1, t_ulong);
    else if (!istype_ulong(lhs->type) && istype_ulong(rhs->type))       insert_cast(n, 1, t_ulong);
    else if (istype_long(lhs->type) && istype_uint(rhs->type))          insert_cast(n, 1, t_long);
    else if (istype_uint(lhs->type) && istype_long(rhs->type))          insert_cast(n, 0, t_long);
    else if (istype_long(lhs->type) && !istype_long(rhs->type))         insert_cast(n, 1, t_long);
    else if (!istype_long(lhs->type) && istype_long(rhs->type))         insert_cast(n, 0, t_long);

    return lhs->type;
}

void propagate_types(Node *n)
{
    // Traverse expressions in tree, propagating types from literals and 
    // variables up to unary and binary operators
    for(int i = 0; i < n->child_count; i++)
    {
        propagate_types(n->children[i]);
    }
    if (n->is_expr)
    {
        if (n->kind == ND_BINOP)
        {
            n->type = check_operands(n);
        }
    }
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
        fprintf(stderr, "%016llx %-20s ", (unsigned long long)t, fulltype_str(t));
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