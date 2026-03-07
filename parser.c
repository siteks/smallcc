
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
// compound-statement ::= "{" (declaration | statement)* "}"
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
//                       | "for" "(" (declaration | expression?) ";" expression? ";" expression? ")" statement
// jump-statement ::= "goto" identifier ";"
//                  | "continue" ";"
//                  | "break" ";"
//                  | "return" expression? ";"
// Parser context instance
ParserContext parser_ctx;
extern int scope_indices[];

static Node *primary_expr();
static Node *unary_expr();
static Node *type_name();
static Node *mult_expr();
static Node *add_expr();
static Node *assign_expr();
static Node *expr();
static Node *stmt();
static Node *func_def();
static Node *declarator();
static Node *init_declarator();
static Node *declaration(int depth);
// Additional forwards needed by unary_expr (sizeof, cast)
static bool is_type_name_or_type(Token *tok);
static void struct_decl(Node *node, int depth);
static void enum_decl(Node *node);
static void parse_decl_specifiers(Node *node);





// Note: parser_ctx.current_function and parser_ctx.anon_index are now in ParserContext

void reset_parser(void)
{
    parser_ctx.current_function = NULL;
    // parser_ctx.anon_index intentionally NOT reset — monotonically increasing across TUs.
}
static char *new_anon_label()
{
    static char a[16];
    sprintf(a, "_l%06d", parser_ctx.anon_index++);
    return a;
}







Node *new_node(Node_kind kind, char *val, bool is_expr)
{
    Node *node = calloc(1, sizeof(Node));
    node->kind = kind;
    node->is_expr = is_expr;
    node->type = t_void;
    node->st   = type_ctx.curr_scope_st;
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
    if (token->kind == TK_IDENT && !strcmp(token->val, "va_start"))
    {
        expect(TK_IDENT);
        expect(TK_LPAREN);
        node = new_node(ND_VA_START, "va_start", true);
        node->type = t_void;
        add_child(node, assign_expr());   // ap
        expect(TK_COMMA);
        add_child(node, assign_expr());   // last named param
        expect(TK_RPAREN);
        return node;
    }
    else if (token->kind == TK_IDENT && !strcmp(token->val, "va_arg"))
    {
        expect(TK_IDENT);
        expect(TK_LPAREN);
        node = new_node(ND_VA_ARG, "va_arg", true);
        add_child(node, assign_expr());   // ap
        expect(TK_COMMA);
        Node *tn = type_name();           // type to fetch
        node->type = tn->type;
        expect(TK_RPAREN);
        return node;
    }
    else if (token->kind == TK_IDENT && !strcmp(token->val, "va_end"))
    {
        expect(TK_IDENT);
        expect(TK_LPAREN);
        node = new_node(ND_VA_END, "va_end", true);
        node->type = t_void;
        add_child(node, assign_expr());   // ap
        expect(TK_RPAREN);
        return node;
    }
    else if (token->kind == TK_IDENT)
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
        int char_val = (int)token->val[0];
        char c[64];
        sprintf(c, "%d", char_val);
        expect(TK_CHARACTER);
        fprintf(stderr, "%s %s\n", c, token->val);
        node = new_node(ND_LITERAL, c, true);
        node->ival = char_val;
        node->type = t_char;
    }
    else if (token->kind == TK_STRING)
    {
        node = new_node(ND_LITERAL, "<string>", true);
        node->strval     = token->val;
        node->strval_len = (int)token->ival;
        node->type       = get_pointer_type(t_char);
        expect(TK_STRING);
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
            ||  tk == TK_ARROW
            ||  tk == TK_INC
            ||  tk == TK_DEC;
}
static Node *unary_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
    Node *node = 0;
    if (token->kind == TK_SIZEOF)
    {
        token = token->next;
        node = new_node(ND_LITERAL, "0", true);
        node->type = t_int;
        if (token->kind == TK_LPAREN && is_type_name_or_type(token->next))
        {
            expect(TK_LPAREN);
            Node *tn = type_name();
            expect(TK_RPAREN);
            char buf[64];
            sprintf(buf, "%d", tn->type->size);
            strncpy(node->val, buf, sizeof(node->val) - 1);
            node->ival = tn->type->size;
        }
        else
        {
            Node *inner;
            if (token->kind == TK_LPAREN)
            {
                expect(TK_LPAREN);
                inner = unary_expr();
                expect(TK_RPAREN);
            }
            else
                inner = unary_expr();
            Type *t = (inner->kind == ND_IDENT && inner->symbol)
                       ? inner->symbol->type : inner->type;
            int sz = t ? t->size : 0;
            char buf[64];
            sprintf(buf, "%d", sz);
            strncpy(node->val, buf, sizeof(node->val) - 1);
            node->ival = sz;
        }
        return node;
    }
    else if (token->kind == TK_INC || token->kind == TK_DEC
            ||  token->kind == TK_AMPERSAND || token->kind == TK_STAR
            ||  token->kind == TK_PLUS || token->kind == TK_MINUS
            ||  token->kind == TK_BANG || token->kind == TK_TWIDDLE)
    {
        Token_kind k = token->kind;
        node = new_node(ND_UNARYOP, expect(k), true);
        node->op_kind = k;
        add_child(node, unary_expr());
    }
    else if (token->kind == TK_IDENT
            || token->kind == TK_CONSTINT
            || token->kind == TK_CONSTFLT
            || token->kind == TK_CHARACTER
            || token->kind == TK_STRING
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
                    // Determine type at current subscript depth
                    Type *sub_type = s ? s->type : node->type;
                    for (int _d = 0; _d < array_depth; _d++) {
                        if (sub_type->base == TB_ARRAY)
                            sub_type = sub_type->u.arr.elem;
                        else if (sub_type->base == TB_POINTER)
                            sub_type = sub_type->u.ptr.pointee;
                    }
                    if (sub_type->base == TB_POINTER)
                    {
                        // Pointer subscript: ptr[idx] → *(ptr + idx)
                        // propagate_types will insert the stride scaling via insert_scale
                        expect(TK_LBRACKET);
                        Node *e1  = node;
                        Node *idx = expr();
                        expect(TK_RBRACKET);
                        Type *elem = sub_type->u.ptr.pointee;
                        Node *add_node = new_node(ND_BINOP, "+", true);
                        add_node->op_kind = TK_PLUS;
                        add_child(add_node, e1);
                        add_child(add_node, idx);
                        node = new_node(ND_UNARYOP, "*", true);
                        node->op_kind = TK_STAR;
                        node->type = elem;
                        add_child(node, add_node);
                        break;
                    }
                    if (!s)
                        error("No ident before left bracket\n");
                    expect(token->kind);
                    Node *e1 = node;
                    Node *add;
                    // Walk type chain to current array depth
                    Type *arr_at_depth = s->type;
                    for (int _d = 0; _d < array_depth; _d++) {
                        if (arr_at_depth->base != TB_ARRAY)
                            error("Too many dimensions for array type %s\n", fulltype_str(s->type));
                        arr_at_depth = arr_at_depth->u.arr.elem;
                    }
                    if (arr_at_depth->base != TB_ARRAY)
                        error("Too many dimensions for array type %s\n", fulltype_str(s->type));
                    bool last_dim = (arr_at_depth->u.arr.elem->base != TB_ARRAY);
                    if (last_dim) {
                        node = new_node(ND_UNARYOP, "*", true);
                        node->op_kind = TK_STAR;
                        node->type = arr_at_depth->u.arr.elem;
                    } else {
                        node = new_node(ND_UNARYOP, "+", true);
                        node->op_kind = TK_PLUS;
                        node->is_array_deref = true;
                    }
                    add = add_child(node, new_node(ND_BINOP, "+", true));
                    add->op_kind = TK_PLUS;
                    add_child(add, e1);
                    Node *mul = new_node(ND_BINOP, "*", true);
                    mul->op_kind = TK_STAR;
                    // stride = size of element at this array depth
                    int mult = arr_at_depth->u.arr.elem->size;
                    array_depth++;
                    char buf[64];
                    sprintf(buf, "%d", mult);
                    Node *stride_lit = new_node(ND_LITERAL, buf, true);
                    stride_lit->ival = mult;
                    add_child(mul, stride_lit);
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
                {
                    add_child(node, assign_expr());
                    while (token->kind == TK_COMMA)
                    {
                        token = token->next;
                        add_child(node, assign_expr());
                    }
                }
                expect(TK_RPAREN);
                break;
            case(TK_DOT):
            {
                expect(token->kind);
                Node *n = new_node(ND_MEMBER, ".", true);
                n->op_kind = TK_DOT;
                add_child(n, node);
                add_child(n, new_node(ND_IDENT, expect(TK_IDENT), true));
                node = n;
                pex_node = n;
                break;
            }
            case(TK_ARROW):
            {
                expect(TK_ARROW);
                Node *n = new_node(ND_MEMBER, "->", true);
                n->op_kind = TK_ARROW;
                add_child(n, node);
                add_child(n, new_node(ND_IDENT, expect(TK_IDENT), true));
                node = n;
                pex_node = n;
                break;
            }
            case(TK_INC):
            case(TK_DEC):
            {
                Token_kind post_k = (token->kind == TK_INC) ? TK_POST_INC : TK_POST_DEC;
                char *op = (token->kind == TK_INC) ? "post++" : "post--";
                token = token->next;
                Node *n = new_node(ND_UNARYOP, op, true);
                n->op_kind = post_k;
                add_child(n, node);
                node = n;
                break;
            }
            default:break;
        }
    }
    return node;
}
static bool is_type_name_or_type(Token *tok)
{
    return is_type_name(tok->kind)
        || (tok->kind == TK_IDENT && is_typedef_name(tok->val));
}
static Node *type_name()
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
    if (token->kind == TK_IDENT && is_typedef_name(token->val))
    {
        node->type = find_typedef_type(token->val);
        expect(TK_IDENT);
        if (token->kind == TK_STAR)
        {
            expect(TK_STAR);
            node->type = get_pointer_type(node->type);
        }
        return node;
    }
    parse_decl_specifiers(node);
    if (node->typespec & DS_ENUM)
    {
        enum_decl(node);
    }
    while(token->kind != TK_RPAREN)
    {
        add_child(node, declarator());
    }
    node->type = type2_from_decl_node(node);
    fprintf(stderr, "%s type:%s\n", __func__, fulltype_str(node->type));
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
    while (token->kind == TK_STAR || token->kind == TK_SLASH || token->kind == TK_PERCENT)
    {
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, cast_expr());
        node = enode;
    }
    return node;
}
void insert_scale(Node *n, int child, int size)
{
    Node *sc = new_node(ND_BINOP, "*", true);
    sc->op_kind = TK_STAR;
    char buf[64];
    sprintf(buf, "%d", size);
    Node *lit = new_node(ND_LITERAL, buf, true);
    lit->ival = size;
    add_child(sc, lit);
    add_child(sc, n->children[child]);
    n->children[child] = sc;
}
static Node *add_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
    Node *node = mult_expr();
    while (token->kind == TK_PLUS || token->kind == TK_MINUS)
    {
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
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
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
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
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
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
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
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
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
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
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
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
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
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
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
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
        Token_kind k = token->kind;
        Node *enode = new_node(ND_BINOP, expect(k), true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, logand_expr());
        node = enode;
    }
    return node;
}
static Node *assign_expr();
static Node *cond_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
    Node *node = logor_expr();
    if (token->kind != TK_QUESTION)
        return node;
    token = token->next;    // consume '?'
    Node *tnode = new_node(ND_TERNARY, "?:", true);
    add_child(tnode, node);
    add_child(tnode, expr());
    expect(TK_COLON);
    add_child(tnode, cond_expr());
    return tnode;
}
static Node *assign_expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
    Node *node = cond_expr();
    if (token->kind == TK_ASSIGN)
    {
        Node *anode = new_node(ND_ASSIGN, expect(TK_ASSIGN), true);
        add_child(anode, node);
        add_child(anode, assign_expr());
        return anode;
    }
    // Compound assignment: a op= b
    static const struct { Token_kind compound; Token_kind base; } ca_map[] = {
        {TK_PLUS_ASSIGN, TK_PLUS}, {TK_MINUS_ASSIGN, TK_MINUS},
        {TK_STAR_ASSIGN, TK_STAR}, {TK_SLASH_ASSIGN, TK_SLASH},
        {TK_AMP_ASSIGN, TK_AMPERSAND}, {TK_BITOR_ASSIGN, TK_BITOR},
        {TK_BITXOR_ASSIGN, TK_BITXOR}, {TK_SHIFTL_ASSIGN, TK_SHIFTL},
        {TK_SHIFTR_ASSIGN, TK_SHIFTR}, {TK_PERCENT_ASSIGN, TK_PERCENT},
    };
    Token_kind op_tk = TK_EMPTY;
    for (int i = 0; i < (int)(sizeof(ca_map)/sizeof(ca_map[0])); i++)
        if (token->kind == ca_map[i].compound) { op_tk = ca_map[i].base; break; }
    if (op_tk != TK_EMPTY)
    {
        char *tok_val = token->val;
        token = token->next;        // consume compound token
        Node *anode = new_node(ND_COMPOUND_ASSIGN, tok_val, true);
        anode->op_kind = op_tk;
        add_child(anode, node);
        add_child(anode, assign_expr());
        return anode;
    }
    return node;
}
Node *expr()
{
    fprintf(stderr, "%s %s\n", __func__, token->val);
    Node *node = assign_expr();
    while (token->kind == TK_COMMA)
    {
        token = token->next;
        Node *enode = new_node(ND_BINOP, ",", true);
        enode->op_kind = TK_COMMA;
        add_child(enode, node);
        add_child(enode, assign_expr());
        node = enode;
    }
    return node;
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
        || (tk == TK_UNION)
        || (tk == TK_ENUM);
}
bool is_typequal(Token_kind tk)
{
    return (tk == TK_CONST)
        || (tk == TK_VOLATILE);
}

static void parse_decl_specifiers(Node *node)
{
    while (is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind))
    {
        if (is_sc_spec(token->kind))  node->sclass   = token->kind;
        if (is_typespec(token->kind)) node->typespec |= to_typespec(token->kind);
        expect(token->kind);
    }
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
    parse_decl_specifiers(node);
    if (node->typespec & (DS_STRUCT | DS_UNION))
    {
        struct_decl(node, 0);
    }
    else if (node->typespec & DS_ENUM)
    {
        enum_decl(node);
    }
    if (token->kind == TK_STAR || token->kind == TK_IDENT || token->kind == TK_LPAREN)
    {
        Node *n = add_child(node, declarator());
    }
    // else: abstract declarator (no name) — valid in C89

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
        {
            expect(TK_COMMA);
            if (token->kind == TK_ELLIPSIS)
            {
                expect(TK_ELLIPSIS);
                node->is_variadic = true;
                break;
            }
        }
    }
    leave_scope();
    // Restore type_ctx.last_symbol_table to this scope so that nested param_type_list
    // calls (inside function-pointer declarators) do not overwrite it.
    type_ctx.last_symbol_table = node->symtable;
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
        n->typespec |= (node->typespec & DS_UNION) ? DS_UNION : DS_STRUCT;
        add_child(n, new_node(ND_IDENT, expect(TK_IDENT), false));
    }
    if (token->kind == TK_LBRACE)
    {
        if (!n)
        {
            // Anonymous struct or union definition
            n = add_child(node, new_node(ND_STRUCT, 0, false));
            add_child(n, new_node(ND_IDENT, new_anon_label(), false));
        }
        expect(TK_LBRACE);
        n->symtable = enter_new_scope(false);
        n->symtable->scope.scope_type = ST_STRUCT;
        do
        {
            add_child(n, declaration(depth + 1));
        }
        while (token->kind != TK_RBRACE);
        leave_scope();
        expect(TK_RBRACE);
    }
}

static void enum_decl(Node *node)
{
    // Optional tag name
    char *tagname = NULL;
    if (token->kind == TK_IDENT)
    {
        tagname = strdup(token->val);
        expect(TK_IDENT);
    }
    if (!tagname)
        tagname = strdup(new_anon_label());

    if (token->kind == TK_LBRACE)
    {
        // Full definition: insert tag, parse body
        Symbol *tag_sym = insert_tag(node, tagname);
        Type  *ety     = get_enum_type(tag_sym);
        tag_sym->type   = ety;
        node->type      = ety;
        expect(TK_LBRACE);
        int next_val = 0;
        while (token->kind != TK_RBRACE)
        {
            char *ename = strdup(expect(TK_IDENT));
            if (token->kind == TK_ASSIGN)
            {
                expect(TK_ASSIGN);
                int sign = 1;
                if (token->kind == TK_MINUS) { sign = -1; expect(TK_MINUS); }
                next_val = sign * (int)token->ival;
                expect(TK_CONSTINT);
            }
            insert_enum_const(node, ety, ename, next_val++);
            if (token->kind == TK_COMMA)
                expect(TK_COMMA);
        }
        expect(TK_RBRACE);
    }
    else
    {
        // Tag-only reference
        Symbol *tag_sym = find_symbol(node, tagname, NS_TAG);
        node->type = tag_sym ? tag_sym->type : t_int;
    }
}

static Node *declaration(int depth)
{
    fprintf(stderr, "%s\n", __func__);
    // <declaration> ::=  {<declaration-specifier>}+ {<init-declarator>}* ;
    Node *node = new_node(ND_DECLARATION, 0, false);
    // At least one decl_spec
    // TODO storage class defaults A.8.1
    while(is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind)
          || (token->kind == TK_IDENT && is_typedef_name(token->val) && node->typespec == 0))
    {
        if (is_sc_spec(token->kind))    node->sclass = token->kind;
        if (is_typespec(token->kind))   node->typespec |= to_typespec(token->kind);
        if (token->kind == TK_IDENT && is_typedef_name(token->val))
        {
            node->typespec |= DS_TYPEDEF;
            node->type      = find_typedef_type(token->val);
        }
        expect(token->kind);
    }
    if (node->typespec & (DS_STRUCT | DS_UNION))
    {
        struct_decl(node, depth);
    }
    else if (node->typespec & DS_ENUM)
    {
        enum_decl(node);
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
                parser_ctx.current_function = node;
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
        // <compound-statement> ::= { {<declaration-or-statement>}* }
        // C99 extension: declarations may appear anywhere in a block.
        expect(TK_LBRACE);
        while (token->kind != TK_RBRACE)
        {
            if (is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind)
                || (token->kind == TK_IDENT && is_typedef_name(token->val)))
                add_child(node, declaration(0));
            else
                add_child(node, stmt());
        }
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
    else if (token->kind == TK_FOR)
    {
        node->kind = ND_FORSTMT;
        expect(TK_FOR);
        expect(TK_LPAREN);
        // init: optional declaration (C99) or expression
        if (is_sc_spec(token->kind) || is_typespec(token->kind) || is_typequal(token->kind)
            || (token->kind == TK_IDENT && is_typedef_name(token->val)))
        {
            // C99 for-init declaration: for (int i = 0; ...).
            // Enter an implicit scope so the variable is confined to the loop.
            node->symtable = enter_new_scope(false);
            add_child(node, declaration(0));   // declaration() consumes the ';'
        }
        else if (token->kind == TK_SEMICOLON)
        {
            add_child(node, new_node(ND_EMPTY, 0, false));
            expect(TK_SEMICOLON);
        }
        else
        {
            add_child(node, expr());
            expect(TK_SEMICOLON);
        }
        // condition (optional; absent = infinite loop)
        if (token->kind == TK_SEMICOLON)
            add_child(node, new_node(ND_EMPTY, 0, false));
        else
            add_child(node, expr());
        expect(TK_SEMICOLON);
        // increment (optional)
        if (token->kind == TK_RPAREN)
            add_child(node, new_node(ND_EMPTY, 0, false));
        else
            add_child(node, expr());
        expect(TK_RPAREN);
        add_child(node, stmt());          // body
        if (node->symtable)
            leave_scope();
    }
    else if (token->kind == TK_DO)
    {
        node->kind = ND_DOWHILESTMT;
        expect(TK_DO);
        add_child(node, stmt());          // body
        expect(TK_WHILE);
        expect(TK_LPAREN);
        add_child(node, expr());          // condition
        expect(TK_RPAREN);
        expect(TK_SEMICOLON);
    }
    else if (token->kind == TK_SWITCH)
    {
        node->kind = ND_SWITCHSTMT;
        expect(TK_SWITCH);
        expect(TK_LPAREN);
        add_child(node, expr());          // selector
        expect(TK_RPAREN);
        add_child(node, comp_stmt(false));// body (always a compound)
    }
    else if (token->kind == TK_CASE)
    {
        node->kind = ND_CASESTMT;
        expect(TK_CASE);
        if (token->kind == TK_IDENT)
        {
            Symbol *s = find_symbol(node, token->val, NS_IDENT);
            if (!s || !istype_enum(s->type))
                error("Expected integer constant in case\n");
            node->ival = (long long)s->offset;
            expect(TK_IDENT);
        }
        else
        {
            node->ival = (long long)expect_number();
        }
        expect(TK_COLON);
    }
    else if (token->kind == TK_DEFAULT)
    {
        node->kind = ND_DEFAULTSTMT;
        expect(TK_DEFAULT);
        expect(TK_COLON);
    }
    else if (token->kind == TK_BREAK)
    {
        node->kind = ND_BREAKSTMT;
        expect(TK_BREAK);
        expect(TK_SEMICOLON);
    }
    else if (token->kind == TK_CONTINUE)
    {
        node->kind = ND_CONTINUESTMT;
        expect(TK_CONTINUE);
        expect(TK_SEMICOLON);
    }
    else if (token->kind == TK_GOTO)
    {
        node->kind = ND_GOTOSTMT;
        expect(TK_GOTO);
        strncpy(node->val, expect_ident(), 63);
        expect(TK_SEMICOLON);
    }
    else if (token->kind == TK_IDENT)
    {
        char *name = expect(TK_IDENT);
        if (token->kind == TK_COLON)
        {
            node->kind = ND_LABELSTMT;
            strncpy(node->val, name, 63);
            expect(TK_COLON);
            add_child(node, stmt());
        }
        else
        {
            unget_token();
            node->kind = ND_EXPRSTMT;
            add_child(node, expr());
            expect(TK_SEMICOLON);
        }
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
const char *nodestr(Node_kind k)
{
    switch(k)
    {
        case ND_PROGRAM :   return "PROGRAM     ";
        case ND_STMT:       return "STMT        ";
        case ND_EXPRSTMT:   return "EXPRSTMT    ";
        case ND_COMPSTMT:   return "COMPSTMT    ";
        case ND_IFSTMT:     return "IFSTMT      ";
        case ND_WHILESTMT:  return "WHILESTMT   ";
        case ND_RETURNSTMT: return "RETURNSTMT  ";
        case ND_BINOP:      return "BINOP       ";
        case ND_UNARYOP:    return "UNARYOP     ";
        case ND_CAST:       return "CAST        ";
        case ND_ASSIGN:          return "ASSIGN      ";
        case ND_COMPOUND_ASSIGN: return "COMPOUND_AS ";
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
        case ND_MEMBER:      return "MEMBER      ";
        case ND_FORSTMT:     return "FORSTMT     ";
        case ND_DOWHILESTMT: return "DOWHILESTMT ";
        case ND_SWITCHSTMT:  return "SWITCHSTMT  ";
        case ND_CASESTMT:    return "CASESTMT    ";
        case ND_DEFAULTSTMT: return "DEFAULTSTMT ";
        case ND_BREAKSTMT:   return "BREAKSTMT   ";
        case ND_CONTINUESTMT:return "CONTINUESTMT";
        case ND_EMPTY:       return "EMPTY       ";
        case ND_LABELSTMT:   return "LABELSTMT   ";
        case ND_GOTOSTMT:    return "GOTOSTMT    ";
        case ND_TERNARY:     return "TERNARY     ";
        case ND_VA_START:    return "VA_START    ";
        case ND_VA_ARG:      return "VA_ARG      ";
        case ND_VA_END:      return "VA_END      ";
        case ND_UNDEFINED:   return "##FIXME##   ";
        default:             return "unknown     ";
    }
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
        node->kind == ND_ARRAY_DECL ? "array" :
        node->is_func_defn ? "fdef " :
        node->is_function ? "func " : "     ",
        node->val,
        node->child_count,
        node->st ? scope_str(node->st->scope) : "(nil)",
        node->type ? fulltype_str(node->type) : "",
        (unsigned long long)node->type);
    if (node->kind == ND_DECLARATION)
    {
        p += sprintf(p, "sclass:%s ",
            token_str(node->sclass));
        for(int j = 0; j < node->child_count; j++)
            if (node->children[j]->kind == ND_DECLARATOR && node->children[j]->symbol)
                p += sprintf(p, "%s | ",
                    fulltype_str(node->children[j]->symbol->type));
    }
    if (node->kind == ND_DECLARATOR || node->kind == ND_DIRECT_DECL)
    {
        p += sprintf(p, "%s %d* %s ", 
            node->val, 
            node->pointer_level, 
            node->is_function ? "func" :
            node->kind == ND_ARRAY_DECL ? "array" : "");
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


const char *get_decl_ident(Node *node)
{
    if (node->child_count)
    {
        if (node->children[0]->kind == ND_IDENT)
            return node->children[0]->val;
        return get_decl_ident(node->children[0]);
    }
    return 0;
}



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
    // Perform the 'usual arithmetic conversions' (C89 §3.2.1.5).
    // Promotions use the *original* lhs/rhs types for checks; insert_cast
    // replaces n->children[], so we return n->children[0]->type at the end
    // to get the post-cast result type (not lhs->type which stays pre-cast).
    Node *lhs = n->children[0];
    Node *rhs = n->children[1];
    fprintf(stderr, "%s %016llx %016llx\n", __func__, (unsigned long long)lhs->type, (unsigned long long)rhs->type);

    // Float/double: one side pulls the other up
    if (istype_float(lhs->type) && !istype_float(rhs->type))            insert_cast(n, 1, t_float);
    else if (!istype_float(lhs->type) && istype_float(rhs->type))       insert_cast(n, 0, t_float);

    // Integer promotions (C89 §3.2.1.1):
    //   char/uchar/short → int;  ushort → uint (int can't represent all ushort
    //   values on a 16-bit target where sizeof(int)==sizeof(short)==2);
    //   enum → int.
    // NOTE: on a 32-bit target, sizeof(int)==4 so int CAN represent all ushort
    // values; ushort would then promote to int rather than uint.  Also,
    // short→int would generate sxw (a real widening), not a no-op.
    if (istype_char(lhs->type))                                         insert_cast(n, 0, t_int);
    if (istype_char(rhs->type))                                         insert_cast(n, 1, t_int);
    if (istype_uchar(lhs->type))                                        insert_cast(n, 0, t_int);
    if (istype_uchar(rhs->type))                                        insert_cast(n, 1, t_int);
    if (istype_short(lhs->type))                                        insert_cast(n, 0, t_int);
    if (istype_short(rhs->type))                                        insert_cast(n, 1, t_int);
    if (istype_ushort(lhs->type))                                       insert_cast(n, 0, t_uint);
    if (istype_ushort(rhs->type))                                       insert_cast(n, 1, t_uint);
    if (istype_enum(lhs->type))                                         insert_cast(n, 0, t_int);
    if (istype_enum(rhs->type))                                         insert_cast(n, 1, t_int);

    // Usual arithmetic conversions — rank hierarchy (C89 §3.2.1.5):
    if (istype_ulong(lhs->type) && !istype_ulong(rhs->type))            insert_cast(n, 1, t_ulong);
    else if (!istype_ulong(lhs->type) && istype_ulong(rhs->type))       insert_cast(n, 0, t_ulong);
    else if (istype_long(lhs->type) && istype_uint(rhs->type))          insert_cast(n, 1, t_long);
    else if (istype_uint(lhs->type) && istype_long(rhs->type))          insert_cast(n, 0, t_long);
    else if (istype_long(lhs->type) && !istype_long(rhs->type))         insert_cast(n, 1, t_long);
    else if (!istype_long(lhs->type) && istype_long(rhs->type))         insert_cast(n, 0, t_long);
    else if (istype_uint(lhs->type) && !istype_uint(rhs->type))         insert_cast(n, 1, t_uint);
    else if (!istype_uint(lhs->type) && istype_uint(rhs->type))         insert_cast(n, 0, t_uint);

    // Return the post-cast type of the left child (both children now have the
    // same type after conversions, so either would give the same answer).
    return n->children[0]->type;
}
Type *check_unary_operand(Node *n)
{
    // Integer promotions for unary operands (C89 §3.2.1.1).
    // & and * do not evaluate/promote their operand in the usual sense:
    //   & takes an address (no value conversion), * dereferences a pointer.
    // NOTE: function argument promotion is not applied here; harmless on this
    // 16-bit target (all narrow integers ≤ 2 bytes share the calling convention
    // slot size), but would need revisiting for a 32-bit target.
    Node *lhs = n->children[0];
    fprintf(stderr, "%s %016llx\n", __func__, (unsigned long long)lhs->type);

    // For dereference, the result type is the element type; no promotion of the pointer.
    if (n->op_kind == TK_STAR)
        return elem_type(lhs->type);
    // Address-of: take address of the original object; no promotion.
    if (n->op_kind == TK_AMPERSAND)
        return get_pointer_type(lhs->type);
    // Increment/decrement: result type is the operand type, no promotion.
    if (n->op_kind == TK_INC || n->op_kind == TK_DEC ||
        n->op_kind == TK_POST_INC || n->op_kind == TK_POST_DEC)
        return lhs->type;

    // For all other unary ops (+, -, ~, !): promote narrow integer types.
    if (istype_char(lhs->type))    insert_cast(n, 0, t_int);
    if (istype_uchar(lhs->type))   insert_cast(n, 0, t_int);
    if (istype_short(lhs->type))   insert_cast(n, 0, t_int);
    if (istype_ushort(lhs->type))  insert_cast(n, 0, t_uint);
    if (istype_enum(lhs->type))    insert_cast(n, 0, t_int);

    // Logical not always yields int regardless of operand type.
    if (n->op_kind == TK_BANG)
        return t_int;

    // Result type is the (possibly promoted) operand type.
    return n->children[0]->type;
}
bool is_unscaled_ptr(Node *n)
{
    return n->type && (n->type->base == TB_POINTER || n->type->base == TB_ARRAY) && !n->is_array_deref;
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
    if (n->kind == ND_IDENT && n->is_expr)
    {
        if (p->kind != ND_MEMBER)
            n->type = find_symbol(n, n->val, NS_IDENT)->type;
    }
    if (n->kind == ND_MEMBER)
    {
        // This is a child of a member operator
        // lhs of member op is the structure, rhs is field
        // child_count may be > 2 when is_function=true (args in children[2..])
        if (n->child_count < 2)
            error("Malformed struct reference\n");
        Node *lhs   = n->children[0];
        Node *rhs   = n->children[1];
        if (lhs->kind == ND_IDENT)
        {
            // If the lhs is an ident (rather than a member operator), it must be
            // in the ident namespace
            lhs->type = find_symbol(lhs, lhs->val, NS_IDENT)->type;
        }
        // For ->, dereference the pointer to get the struct type
        Type *struct_type = lhs->type;
        if (n->op_kind == TK_ARROW)
        {
            if (!istype_ptr(struct_type))
                error("'->' requires pointer type\n");
            struct_type = struct_type->u.ptr.pointee;
        }
        fprintf(stderr, "%s looking in lhs type:%016llx for field %s\n", __func__, (unsigned long long)struct_type, rhs->val);
        Type *base = 0;
        n->offset = find_offset(struct_type, rhs->val, &base);
        if (n->offset < 0)
            error("Can't find member %s in struct\n", rhs->val);
        fprintf(stderr, "%s found member %s with offset %d basetype %016llx\n", __func__,
            rhs->val, n->offset, (unsigned long long)base);
        // Member now has type pointing to inner type
        n->type = base;
        // If this is a call through a function-pointer member, resolve to return type
        if (n->is_function && istype_ptr(n->type)
            && istype_function(n->type->u.ptr.pointee))
            n->type = n->type->u.ptr.pointee->u.fn.ret;
    }
    if (n->is_expr)
    {
        if (n->kind == ND_TERNARY)
        {
            // Result type is the common type of then/else branches
            n->type = n->children[1]->type;
        }
        if (n->kind == ND_BINOP)
        {
            if (n->op_kind == TK_COMMA)
            {
                // Comma: result type is the right operand's type
                n->type = n->children[1]->type;
            }
            else
            {
            n->type = check_operands(n);
            // Comparison and logical operators always yield int, regardless of operand types
            if (n->op_kind == TK_LT  || n->op_kind == TK_LE ||
                n->op_kind == TK_GT  || n->op_kind == TK_GE ||
                n->op_kind == TK_EQ  || n->op_kind == TK_NE ||
                n->op_kind == TK_LOGAND || n->op_kind == TK_LOGOR)
                n->type = t_int;

            // See if either node is a pointer and the other an int of some sort. If so, we need to
            // scale the int by the size of the pointed to type
            if (is_unscaled_ptr(n->children[0]) && istype_intlike(n->children[1]->type))
                insert_scale(n, 1, elem_type(n->children[0]->type)->size);
            else if (is_unscaled_ptr(n->children[1]) && istype_intlike(n->children[0]->type))
                insert_scale(n, 0, elem_type(n->children[1]->type)->size);
            }   // end else (non-comma binop)
        }
        if (n->kind == ND_UNARYOP)
        {
            Type *t = check_unary_operand(n);
            if (n->type == t_void)  // preserve non-void types already set by parser (e.g. array subscripts)
                n->type = t;
        }
    }
    if (n->kind == ND_COMPOUND_ASSIGN)
    {
        // Result type is the LHS type (same as plain assignment).
        // If RHS type differs from LHS type, cast RHS so codegen uses consistent ops.
        Type *lhs_type = n->children[0]->type;
        n->type = lhs_type;
        if (n->children[1]->type != lhs_type)
            insert_cast(n, 1, lhs_type);
    }
    if (n->kind == ND_RETURNSTMT && n->child_count)
    {
        n->type = parser_ctx.current_function->type->u.fn.ret;
        fprintf(stderr, "%s found return stmt with expr, type of func:%s:\n", __func__, fulltype_str(n->type));
        if (n->type != n->children[0]->type)
            insert_cast(n, 0, n->type);
    }
    fprintf(stderr, "After : %s\n", node_str(n));
}

