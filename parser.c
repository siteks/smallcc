
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
extern Type2        *types;
extern Symbol_table *symbol_table;




// t_void, t_int etc. declared in mycc.h as extern Type2*


extern Symbol_table *curr_st_scope;
extern int scope_depth;
extern int scope_indices[];

static Node *primary_expr();
static Node *mult_expr();
static Node *add_expr();
static Node *assign_expr();
static Node *expr();
static Node *stmt();
static Node *func_def();
static Node *declarator();
static Node *init_declarator();
static Node *declaration(int depth);





static Node *current_function;
static int anon_index = 0;
static char *new_anon_label()
{
    static char a[16];
    sprintf(a, "_l%06d", anon_index++);
    return a;
}







Node *new_node(Node_kind kind, char *val, bool is_expr)
{
    Node *node = calloc(1, sizeof(Node));
    node->kind = kind;
    node->is_expr = is_expr;
    node->type = t_void;
    set_scope(&node->scope);
    if (val)
        strncpy(node->val, val, 63);
    return node;
}

static Node *add_child(Node *parent, Node *child)
{
    parent->child_count++;
    parent->children = (Node **)realloc(parent->children, parent->child_count * sizeof(Node *));
    parent->children[parent->child_count - 1] = child;
    return child;
}

static Node *primary_expr()
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
            else error("Float constant out of range");
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
static Node *unary_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
            node->symbol = find_symbol(node, node->val, NS_IDENT);
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
                    Node *e1 = node;
                    Node *add;
                    // Walk type chain to current array depth
                    Type2 *arr_at_depth = s->type;
                    for (int _d = 0; _d < array_depth; _d++) {
                        if (arr_at_depth->base != TB2_ARRAY)
                            error("Too many dimensions for array type %s\n", fulltype_str(s->type));
                        arr_at_depth = arr_at_depth->u.arr.elem;
                    }
                    if (arr_at_depth->base != TB2_ARRAY)
                        error("Too many dimensions for array type %s\n", fulltype_str(s->type));
                    bool last_dim = (arr_at_depth->u.arr.elem->base != TB2_ARRAY);
                    if (last_dim) {
                        node = new_node(ND_UNARYOP, "*", true);
                        node->type = arr_at_depth->u.arr.elem;
                    } else {
                        node = new_node(ND_UNARYOP, "+", true);
                        node->is_array_deref = true;
                    }
                    add = add_child(node, new_node(ND_BINOP, "+", true));
                    add_child(add, e1);
                    Node *mul = new_node(ND_BINOP, "*", true);
                    // stride = size of element at this array depth
                    int mult = arr_at_depth->u.arr.elem->size;
                    array_depth++;
                    char buf[64];
                    sprintf(buf, "%d", mult);
                    add_child(mul, new_node(ND_LITERAL, buf, true));
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
            {
                // Member access. The primary expression is a struct (possibly
                // dereferenced pointer) and then there are one or more member 
                // references
                expect(token->kind);

                // Node *n = new_node(ND_MEMBER, 0, true);
                // add_child(n, struct_node);
                // add_child(n, new_node(ND_IDENT, expect(TK_IDENT), true));
                // struct_node = n;
                // node = n;
                Node *n = new_node(ND_MEMBER, 0, true);
                add_child(n, node);
                add_child(n, new_node(ND_IDENT, expect(TK_IDENT), true));
                node = n;
                break;
            }
            case(TK_INC):
            case(TK_DEC):
            default:break;
        }
    }
    return node;
}
bool is_type_name_or_type(Token *token)
{
    return is_type_name(token->kind);
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
        if (is_typespec(token->kind))   node->typespec |= to_typespec(token->kind);
        if (is_typequal(token->kind))   node->typequal = token->kind;
        expect(token->kind);
    }
    while(token->kind != TK_RPAREN)
    {
        add_child(node, declarator());
    }
    // add_types_and_symbols(node, false);
    node->type = type2_from_ts(node, tstr_compact(node));
    fprintf(stderr, "%s ts:%s:\n", __func__, tstr_compact(node));
    return node;
}
static Node *cast_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
        // We checked ident and it was not a type, so rewind one token
        // and proceed as unary_expr 
        unget_token();
        return unary_expr();
    }
}
static Node *mult_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
void insert_scale(Node *n, int child, int size)
{
    Node *sc = new_node(ND_BINOP, "*", true);
    char buf[64]; 
    sprintf(buf, "%d", size);
    add_child(sc, new_node(ND_LITERAL, buf, true));
    add_child(sc, n->children[child]);
    n->children[child] = sc;
}
static Node *add_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
static Node *shift_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
    Node *node = add_expr();
    while (token->kind == TK_SHIFTL || token->kind == TK_SHIFTR)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
        add_child(enode, node);
        add_child(enode, add_expr());
        node = enode;
    }
    return node;
}
static Node *rel_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
    Node *node = shift_expr();
    while (token->kind == TK_LT || token->kind == TK_LE || token->kind == TK_GT || token->kind == TK_GE)
    {
        Node *enode = new_node(ND_BINOP, expect(token->kind), true);
        add_child(enode, node);
        add_child(enode, shift_expr());
        node = enode;
    }
    return node;
}
static Node *equal_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
static Node *bitand_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
static Node *bitxor_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
static Node *bitor_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
static Node *logand_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
static Node *logor_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
static Node *assign_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
    fprintf(stderr, "%s %s\n", __func__, token->val);
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
        || (tk == TK_UNSIGNED)
        || (tk == TK_STRUCT)
        || (tk == TK_UNION);
}
bool is_typequal(Token_kind tk)
{
    return (tk == TK_CONST)
        || (tk == TK_VOLATILE);
}

static Node *param_declaration()
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
        if (is_typespec(token->kind))   node->typespec |= to_typespec(token->kind);
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
    add_types_and_symbols(node, true, 0);
    return node;
}
static Node *param_type_list()
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
static Node *constant_expr()
{
    fprintf(stderr, "%s\n", __func__);
    return equal_expr();
}
static Node *direct_decl()
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
static Node *declarator()
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
static Node *initializer();
static Node *initializer_list()
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
static Node *initializer()
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
static Node *init_declarator()
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
static Node *comp_stmt(bool use_last_scope);
static void mark_struct(Node *n)
{
    n->is_struct = true;
    for(int i = 0; i < n->child_count; i++)
        mark_struct(n->children[i]);
}

static void struct_decl(Node *node, int depth)
{
    // struct-or-union-specifier ::= struct-or-union identifier "{" struct-declaration+ "}"
    //                             | struct-or-union "{" struct-declaration+ "}"
    //                             | struct-or-union identifier        
    Node *n = 0;
    if (token->kind == TK_IDENT)
    {
        // struct or union definition with a tag, or an incomplete type, or a declaration.
        // Declarations using incomplete types are only valid if a pointer
        n = add_child(node, new_node(ND_STRUCT, 0, false));
        n->struct_depth = depth;
        n->typespec |= TB_STRUCT;
        Node *i = add_child(n, new_node(ND_IDENT, expect(TK_IDENT), false));
        i->is_struct = true;
    }
    if (token->kind == TK_LBRACE)
    {
        if (!n)
        {
            // Anonymous struct or union definition
            n = add_child(node, new_node(ND_STRUCT, 0, false));
            n->struct_depth = depth;
            Node *i = add_child(n, new_node(ND_IDENT, new_anon_label(), false));
            i->is_struct = true;
        }
        expect(TK_LBRACE);
        n->symtable = enter_new_scope(false);
        n->symtable->scope.scope_type = ST_STRUCT;
        do
        {
            Node *c = add_child(n, declaration(depth + 1));
            mark_struct(c);
        }
        while (token->kind != TK_RBRACE);
        leave_scope();
        expect(TK_RBRACE);
    }
}

static Node *declaration(int depth)
{
    fprintf(stderr, "%s\n", __func__);
    // <declaration> ::=  {<declaration-specifier>}+ {<init-declarator>}* ;
    Node *node = new_node(ND_DECLARATION, 0, false);
    // At least one decl_spec
    // TODO storage class defaults A.8.1
    while(is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind))
    {
        if (is_sc_spec(token->kind))    node->sclass = token->kind;
        if (is_typespec(token->kind))   node->typespec |= to_typespec(token->kind);
        if (is_typequal(token->kind))   node->typequal = token->kind;
        expect(token->kind);
    }
    if (node->typespec & TB_STRUCT)
    {
        struct_decl(node, depth);
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
                add_types_and_symbols(node, false, 0);
                current_function = node;
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
    // We don't add struct members but we do add tags
    // print_tree(node, 0);

    add_types_and_symbols(node, false, depth);
    return node;
}
static Node *comp_stmt(bool use_last_scope)
{
    fprintf(stderr, "%s\n", __func__);
    Node *node      = new_node(ND_COMPSTMT, 0, false);
    node->symtable  = enter_new_scope(use_last_scope);
    fprintf(stderr, "%s\n", curr_scope_str());
    if (token->kind == TK_LBRACE)
    {
        // <compound-statement> ::= { {<declaration>}* {<statement>}* }
        expect(TK_LBRACE);
        while (is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind))
        {
            // TODO Add defined types
            add_child(node, declaration(0));
        }
        while (token->kind != TK_RBRACE)
            add_child(node, stmt());
        expect(TK_RBRACE);
    }
    leave_scope();
    return node;
}
static Node *stmt()
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
Node *program()
{
    fprintf(stderr, "%s\n", __func__);
    Node *node = new_node(ND_PROGRAM, 0, false);
    while(!at_eof())
    {
        add_child(node, declaration(0));
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
        case ND_STRUCT:     return "STRUCT      ";
        case ND_DECLARATION:return "DECLARATION ";
        case ND_DECLARATOR: return "DECLARATOR  ";
        case ND_DIRECT_DECL:return "DIRECT_DECL ";
        case ND_PTYPE_LIST: return "PTYPE_LIST  ";
        case ND_TYPE_NAME:  return "TYPE_NAME   ";
        case ND_ARRAY_DECL: return "ARRAY_DECL  ";
        case ND_FUNC_DECL:  return "FUNC_DECL   ";
        case ND_MEMBER:     return "MEMBER      ";
        case ND_UNDEFINED:  return "##FIXME##   ";
        default:            return "unknown     ";
    }
}

// This is lifted from K&R
char ts[1024];
char vn[1024];
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
char buf[1024];
char *node_str(Node *node)
{
    buf[0] = 0;
    char *p = buf;
    if (!node)
        return buf;
    p += sprintf(p, "%s: %5s %s ch:%d sc:%s fts:%s: t:%016llx ", 
        nodestr(node->kind), 
        node->is_array ? "array" : 
        node->is_func_defn ? "fdef " : 
        node->is_function ? "func " : 
        node->is_struct ? "struct " : "     ", 
        node->val, 
        node->child_count, 
        scope_str(node->scope), 
        node->type ? fulltype_str(node->type) : "", 
        (unsigned long long)node->type);
    if (node->kind == ND_DECLARATION)
    {
        p += sprintf(p, "sclass:%s typequal:%s typespec:%s ", 
            type_token_str(node->sclass), 
            type_token_str(node->typequal), 
            typespec_str(node->typespec));
        for(int j = 0; j < node->child_count; j++)
            if (node->children[j]->kind == ND_DECLARATOR && node->children[j]->symbol)
                p += sprintf(p, "%s <%s> | ", 
                    fulltype_str(node->children[j]->symbol->type), 
                    tstr_compact(node->children[j]));
    }
    if (node->kind == ND_DECLARATOR || node->kind == ND_DIRECT_DECL)
    {
        p += sprintf(p, "%s %d* %s ", 
            node->val, 
            node->pointer_level, 
            node->is_function ? "func" : 
            node->is_array ? "array" : "");
        if (node->is_array) 
            p += sprintf(p, "%d", node->array_size);
    }
    return buf;
}

void print_tree(Node *node, int depth)
{
    if (depth==0)fprintf(stderr, "------ Parse tree ------\n");
    if (!node)
        return;
    for(int i = 0; i < depth; i++) 
        fprintf(stderr, "  ");
    fprintf(stderr, "%s", node_str(node));
    fprintf(stderr, "\n");
    for(int i = 0; i < node->child_count; i++)
        print_tree(node->children[i], depth + 1);
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



void insert_cast(Node *n, int child, Type2 *t)
{
    fprintf(stderr, "%s %s\n", __func__, fulltype_str(t));
    Node *c = new_node(ND_CAST, 0, true);

    // We don't care what is in the declaration, its just a placeholder
    add_child(c, new_node(ND_DECLARATION, 0, false));
    add_child(c, n->children[child]);
    c->type = t;
    n->children[child] = c;
}
Type2 *check_operands(Node *n)
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
Type2 *check_unary_operand(Node *n)
{
    // Perform the 'usual arithmetic conversions'
    // Check against the rules, and insert a cast to perform the conversion
    // Ignore the float promotion, we are keeping all float types the same 32 bits
    //
    Node *lhs = n->children[0];
    fprintf(stderr, "%s %016llx\n", __func__, (unsigned long long)lhs->type);
    if (istype_char(lhs->type))                                         insert_cast(n, 0, t_int);

    // For dereference, the result type is the pointed-to/element type, not the pointer/array type
    if (!strcmp(n->val, "*"))
        return elem_type(lhs->type);
    return lhs->type;
}
bool is_unscaled_ptr(Node *n)
{
    return n->type && (n->type->base == TB2_POINTER || n->type->base == TB2_ARRAY) && !n->is_array_deref;
}
void propagate_types(Node *p, Node *n)
{
    // Traverse expressions in tree, propagating types from literals and 
    // variables up to unary and binary operators
    fprintf(stderr, "%s\n", __func__);
    for(int i = 0; i < n->child_count; i++)
    {
        propagate_types(n, n->children[i]);
    }
    if (!p)
    {
        // We are at the top of the tree, nothing further to do
        fprintf(stderr, "%s finished\n", __func__);
        return;
    }
    fprintf(stderr, "Before: %s\n", node_str(n));
    if (n->kind == ND_IDENT && !n->is_struct)
    {
        if (p->kind != ND_MEMBER)
            n->type = find_symbol(n, n->val, NS_IDENT)->type;
    }
    if (n->kind == ND_MEMBER)
    {
        // This is a child of a member operator
        // lhs of member op is the structure, rhs is field
        if (n->child_count != 2)
            error("Malformed struct reference\n");
        Node *lhs   = n->children[0];
        Node *rhs   = n->children[1];
        if (lhs->kind == ND_IDENT)
        {
            // If the lhs is an ident (rather than a member operator), it must be
            // in the ident namespace
            lhs->type = find_symbol(lhs, lhs->val, NS_IDENT)->type;
        }
        fprintf(stderr, "%s looking in lhs type:%016llx for field %s\n", __func__, (unsigned long long)lhs->type, rhs->val);
        Type2 *base = 0;
        n->offset = find_offset(lhs->type, rhs->val, &base);
        if (n->offset < 0)
            error("Can't find member %s in struct\n", rhs->val);
        fprintf(stderr, "%s found member %s with offset %d basetype %016llx\n", __func__,
            rhs->val, n->offset, (unsigned long long)base);
        // Member now has type pointing to inner type
        n->type = base;
    }
    if (n->is_expr)
    {
        if (n->kind == ND_BINOP)
        {
            n->type = check_operands(n);
            
            // See if either node is a pointer and the other an int of some sort. If so, we need to 
            // scale the int by the size of the pointed to type
            if (is_unscaled_ptr(n->children[0]) && istype_intlike(n->children[1]->type))
                insert_scale(n, 1, elem_type(n->children[0]->type)->size);
            else if (is_unscaled_ptr(n->children[1]) && istype_intlike(n->children[0]->type))
                insert_scale(n, 0, elem_type(n->children[1]->type)->size);
        }
        if (n->kind == ND_UNARYOP)
        {
            Type2 *t = check_unary_operand(n);
            if (n->type == t_void)  // preserve non-void types already set by parser (e.g. array subscripts)
                n->type = t;
        }
    }
    if (n->kind == ND_RETURNSTMT && n->child_count)
    {
        n->type = current_function->type->u.fn.ret;
        fprintf(stderr, "%s found return stmt with expr, type of func:%s:\n", __func__, fulltype_str(n->type));
        if (n->type != n->children[0]->type)
            insert_cast(n, 0, n->type);
    }
    fprintf(stderr, "After : %s\n", node_str(n));
}

