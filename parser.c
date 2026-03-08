
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
    // Store identifier/label strings in union; operators use op_kind instead
    if (val) {
        if (kind == ND_IDENT)
            node->u.ident = strdup(val);
        else if (kind == ND_LABELSTMT || kind == ND_GOTOSTMT)
            node->u.label = strdup(val);
        // ND_BINOP, ND_UNARYOP, ND_MEMBER store operator in op_kind, not in u.op
        // ND_LITERAL: val is not used (ival/fval hold the value)
    }
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
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node;
    if (token_ctx.current->kind == TK_IDENT && !strcmp(token_ctx.current->val, "va_start"))
    {
        expect(TK_IDENT);
        expect(TK_LPAREN);
        node = new_node(ND_VA_START, NULL, true);
        node->type = t_void;
        add_child(node, assign_expr());   // ap
        expect(TK_COMMA);
        add_child(node, assign_expr());   // last named param
        expect(TK_RPAREN);
        node->nu.vastart.ap = node->children[0];
        node->nu.vastart.last = node->children[1];
        return node;
    }
    else if (token_ctx.current->kind == TK_IDENT && !strcmp(token_ctx.current->val, "va_arg"))
    {
        expect(TK_IDENT);
        expect(TK_LPAREN);
        node = new_node(ND_VA_ARG, NULL, true);
        add_child(node, assign_expr());   // ap
        expect(TK_COMMA);
        Node *tn = type_name();           // type to fetch
        node->type = tn->type;
        expect(TK_RPAREN);
        node->nu.vaarg.ap = node->children[0];
        return node;
    }
    else if (token_ctx.current->kind == TK_IDENT && !strcmp(token_ctx.current->val, "va_end"))
    {
        expect(TK_IDENT);
        expect(TK_LPAREN);
        node = new_node(ND_VA_END, NULL, true);
        node->type = t_void;
        add_child(node, assign_expr());   // ap
        expect(TK_RPAREN);
        node->nu.vaend.ap = node->children[0];
        return node;
    }
    else if (token_ctx.current->kind == TK_IDENT)
    {
        node = new_node(ND_IDENT, expect(TK_IDENT), true);
    }
    else if (token_ctx.current->kind == TK_CONSTINT || token_ctx.current->kind == TK_CONSTFLT)
    {
        Token *tk = token_ctx.current;
        node = new_node(ND_LITERAL, expect(token_ctx.current->kind), true);
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
    else if (token_ctx.current->kind == TK_CHARACTER)
    {
        int char_val = (int)token_ctx.current->val[0];
        char c[64];
        sprintf(c, "%d", char_val);
        expect(TK_CHARACTER);
        DBG_PRINT("%s %s\n", c, token_ctx.current->val);
        node = new_node(ND_LITERAL, c, true);
        node->ival = char_val;
        node->type = t_char;
    }
    else if (token_ctx.current->kind == TK_STRING)
    {
        node = new_node(ND_LITERAL, NULL, true);
        node->strval     = token_ctx.current->val;
        node->strval_len = (int)token_ctx.current->ival;
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
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = 0;
    if (token_ctx.current->kind == TK_SIZEOF)
    {
        token_ctx.current = token_ctx.current->next;
        node = new_node(ND_LITERAL, NULL, true);
        node->type = t_int;
        if (token_ctx.current->kind == TK_LPAREN && is_type_name_or_type(token_ctx.current->next))
        {
            expect(TK_LPAREN);
            Node *tn = type_name();
            expect(TK_RPAREN);
            node->ival = tn->type->size;
        }
        else
        {
            Node *inner;
            if (token_ctx.current->kind == TK_LPAREN)
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
            node->ival = sz;
        }
        return node;
    }
    else if (token_ctx.current->kind == TK_INC || token_ctx.current->kind == TK_DEC
            ||  token_ctx.current->kind == TK_AMPERSAND || token_ctx.current->kind == TK_STAR
            ||  token_ctx.current->kind == TK_PLUS || token_ctx.current->kind == TK_MINUS
            ||  token_ctx.current->kind == TK_BANG || token_ctx.current->kind == TK_TWIDDLE)
    {
        Token_kind k = token_ctx.current->kind;
        token_ctx.current = token_ctx.current->next;  // consume the operator token_ctx.current
        node = new_node(ND_UNARYOP, NULL, true);
        node->op_kind = k;
        add_child(node, unary_expr());
        // NEW: also populate tagged union field
        node->nu.unaryop.operand = node->children[0];
    }
    else if (token_ctx.current->kind == TK_IDENT
            || token_ctx.current->kind == TK_CONSTINT
            || token_ctx.current->kind == TK_CONSTFLT
            || token_ctx.current->kind == TK_CHARACTER
            || token_ctx.current->kind == TK_STRING
            || token_ctx.current->kind == TK_LPAREN)
    {
        node = primary_expr();
        if (node->kind == ND_IDENT)
        {
            // set the symbol
            node->symbol = find_symbol(node, node->u.ident, NS_IDENT);
        }
    }
    // postfix-expr
    // Keep pointer to array or func ident, so we can reference later during fixup
    Symbol *s = node->symbol;
    int array_depth = 0;
    // Keep ref to primary expression node so we can mark as function call is necessary
    Node *pex_node = node;
    while(is_postfix(token_ctx.current->kind))
    {
        switch (token_ctx.current->kind)
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
                        Node *add_node = new_node(ND_BINOP, NULL, true);
                        add_node->op_kind = TK_PLUS;
                        add_child(add_node, e1);
                        add_child(add_node, idx);
                        // NEW: also populate tagged union fields for BINOP
                        add_node->nu.binop.lhs = e1;
                        add_node->nu.binop.rhs = idx;
                        node = new_node(ND_UNARYOP, NULL, true);
                        node->op_kind = TK_STAR;
                        node->type = elem;
                        add_child(node, add_node);
                        // NEW: also populate tagged union field for UNARYOP
                        node->nu.unaryop.operand = add_node;
                        break;
                    }
                    if (!s)
                        error("No ident before left bracket\n");
                    expect(token_ctx.current->kind);
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
                    Node *outer_unary = new_node(ND_UNARYOP, NULL, true);
                    if (last_dim) {
                        outer_unary->op_kind = TK_STAR;
                        outer_unary->type = arr_at_depth->u.arr.elem;
                    } else {
                        outer_unary->op_kind = TK_PLUS;
                        outer_unary->is_array_deref = true;
                    }
                    node = outer_unary;
                    add = add_child(node, new_node(ND_BINOP, NULL, true));
                    add->op_kind = TK_PLUS;
                    add_child(add, e1);
                    Node *mul = new_node(ND_BINOP, NULL, true);
                    mul->op_kind = TK_STAR;
                    // stride = size of element at this array depth
                    int mult = arr_at_depth->u.arr.elem->size;
                    array_depth++;
                    char buf[64];
                    sprintf(buf, "%d", mult);
                    Node *stride_lit = new_node(ND_LITERAL, NULL, true);
                    stride_lit->ival = mult;
                    add_child(mul, stride_lit);
                    add_child(mul, expr());
                    add_child(add, mul);
                    // NEW: also populate tagged union fields
                    add->nu.binop.lhs = e1;
                    add->nu.binop.rhs = mul;
                    mul->nu.binop.lhs = stride_lit;
                    mul->nu.binop.rhs = mul->children[1];
                    outer_unary->nu.unaryop.operand = add;
                    expect(TK_RBRACKET);
                    break;
                }
            case(TK_LPAREN):
                // Function call
                expect(TK_LPAREN);
                pex_node->is_function = true;
                if (token_ctx.current->kind != TK_RPAREN)
                {
                    add_child(node, assign_expr());
                    while (token_ctx.current->kind == TK_COMMA)
                    {
                        token_ctx.current = token_ctx.current->next;
                        add_child(node, assign_expr());
                    }
                }
                expect(TK_RPAREN);
                break;
            case(TK_DOT):
            {
                expect(token_ctx.current->kind);
                Node *n = new_node(ND_MEMBER, NULL, true);
                n->op_kind = TK_DOT;
                add_child(n, node);
                add_child(n, new_node(ND_IDENT, expect(TK_IDENT), true));
                // NEW: also populate tagged union fields
                n->nu.member.base = node;
                n->nu.member.field_name = n->children[1]->u.ident;
                node = n;
                pex_node = n;
                break;
            }
            case(TK_ARROW):
            {
                expect(TK_ARROW);
                Node *n = new_node(ND_MEMBER, NULL, true);
                n->op_kind = TK_ARROW;
                add_child(n, node);
                add_child(n, new_node(ND_IDENT, expect(TK_IDENT), true));
                // NEW: also populate tagged union fields
                n->nu.member.base = node;
                n->nu.member.field_name = n->children[1]->u.ident;
                node = n;
                pex_node = n;
                break;
            }
            case(TK_INC):
            case(TK_DEC):
            {
                Token_kind post_k = (token_ctx.current->kind == TK_INC) ? TK_POST_INC : TK_POST_DEC;
                char *op = (token_ctx.current->kind == TK_INC) ? "post++" : "post--";
                token_ctx.current = token_ctx.current->next;
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
    DBG_FUNC();
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
    // if (is_type_name_or_type(token_ctx.current))
    // {
    //     return new_node(ND_TYPE_NAME, expect(token_ctx.current->kind));
    // }
    Node *node = new_node(ND_DECLARATION, 0, true);
    if (token_ctx.current->kind == TK_IDENT && is_typedef_name(token_ctx.current->val))
    {
        node->type = find_typedef_type(token_ctx.current->val);
        expect(TK_IDENT);
        if (token_ctx.current->kind == TK_STAR)
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
    while(token_ctx.current->kind != TK_RPAREN)
    {
        add_child(node, declarator());
    }
    node->type = type2_from_decl_node(node);
    DBG_PRINT("%s type:%s\n", __func__, fulltype_str(node->type));
    return node;
}
static Node *cast_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    if (token_ctx.current->kind != TK_LPAREN)
    {
        return unary_expr();
    }
    // May be either a cast or a primary expr with parens eg "(" expr ")". 
    // If the ident following the current token_ctx.current is a type name or 
    // const|volatile, this is a cast. A cast may be followed by a cast
    expect(TK_LPAREN);
    if (is_type_name_or_type(token_ctx.current))
    {
        // This is a cast, create the node and add the type elements as
        // children
        Node *node = new_node(ND_CAST, 0, true);
        // while (token_ctx.current->kind != TK_RPAREN)
        // {
        //     add_child(node, type_name());
        // }
        add_child(node, type_name());
        // Set the type of the cast to that declared in the child
        node->type = node->children[node->child_count - 1]->type;
        expect(TK_RPAREN);
        add_child(node, cast_expr());
        // Set nu.cast.expr to point to the expression (children[1])
        node->nu.cast.expr = node->children[1];
        return node;
    }
    else
    {
        // We checked ident and it was not a type, so rewind one token_ctx.current
        // and proceed as unary_expr 
        unget_token();
        return unary_expr();
    }
}
static Node *mult_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = cast_expr();
    while (token_ctx.current->kind == TK_STAR || token_ctx.current->kind == TK_SLASH || token_ctx.current->kind == TK_PERCENT)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, cast_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
void insert_scale(Node *n, int child, int size)
{
    Node *sc = new_node(ND_BINOP, NULL, true);
    sc->op_kind = TK_STAR;
    Node *lit = new_node(ND_LITERAL, NULL, true);
    lit->ival = size;
    add_child(sc, lit);
    add_child(sc, n->children[child]);
    // Set nu.binop fields for the new scale node
    sc->nu.binop.lhs = lit;
    sc->nu.binop.rhs = sc->children[1];
    n->children[child] = sc;
    // Update parent's nu.binop field to point to the new scale node
    if (n->kind == ND_BINOP || n->kind == ND_ASSIGN) {
        if (child == 0) n->nu.binop.lhs = sc;
        else if (child == 1) n->nu.binop.rhs = sc;
    }
}
static Node *add_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = mult_expr();
    while (token_ctx.current->kind == TK_PLUS || token_ctx.current->kind == TK_MINUS)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, mult_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *shift_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = add_expr();
    while (token_ctx.current->kind == TK_SHIFTL || token_ctx.current->kind == TK_SHIFTR)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, add_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *rel_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = shift_expr();
    while (token_ctx.current->kind == TK_LT || token_ctx.current->kind == TK_LE || token_ctx.current->kind == TK_GT || token_ctx.current->kind == TK_GE)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, shift_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *equal_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = rel_expr();
    while (token_ctx.current->kind == TK_EQ || token_ctx.current->kind == TK_NE)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, rel_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *bitand_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = equal_expr();
    while (token_ctx.current->kind == TK_AMPERSAND)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, equal_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *bitxor_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = bitand_expr();
    while (token_ctx.current->kind == TK_BITXOR)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, bitand_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *bitor_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = bitxor_expr();
    while (token_ctx.current->kind == TK_BITOR)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, bitxor_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *logand_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = bitor_expr();
    while (token_ctx.current->kind == TK_LOGAND)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, bitor_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *logor_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = logand_expr();
    while (token_ctx.current->kind == TK_LOGOR)
    {
        Token_kind k = token_ctx.current->kind;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        add_child(enode, node);
        add_child(enode, logand_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
        node = enode;
    }
    return node;
}
static Node *assign_expr();
static Node *cond_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = logor_expr();
    if (token_ctx.current->kind != TK_QUESTION)
        return node;
    token_ctx.current = token_ctx.current->next;    // consume '?'
    Node *tnode = new_node(ND_TERNARY, NULL, true);
    add_child(tnode, node);
    add_child(tnode, expr());
    expect(TK_COLON);
    add_child(tnode, cond_expr());
    // NEW: also populate tagged union fields
    tnode->nu.ternary.cond = node;
    tnode->nu.ternary.then_ = tnode->children[1];
    tnode->nu.ternary.else_ = tnode->children[2];
    return tnode;
}
static Node *assign_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = cond_expr();
    if (token_ctx.current->kind == TK_ASSIGN)
    {
        Node *anode = new_node(ND_ASSIGN, expect(TK_ASSIGN), true);
        add_child(anode, node);
        add_child(anode, assign_expr());
        // NEW: populate tagged union fields
        anode->nu.binop.lhs = node;
        anode->nu.binop.rhs = anode->children[1];
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
        if (token_ctx.current->kind == ca_map[i].compound) { op_tk = ca_map[i].base; break; }
    if (op_tk != TK_EMPTY)
    {
        token_ctx.current = token_ctx.current->next;        // consume compound token_ctx.current
        Node *anode = new_node(ND_COMPOUND_ASSIGN, NULL, true);
        anode->op_kind = op_tk;
        add_child(anode, node);
        add_child(anode, assign_expr());
        // NEW: also populate tagged union fields
        anode->nu.compound_assign.lhs = node;
        anode->nu.compound_assign.rhs = anode->children[1];
        anode->nu.compound_assign.op = op_tk;
        return anode;
    }
    return node;
}
Node *expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = assign_expr();
    while (token_ctx.current->kind == TK_COMMA)
    {
        token_ctx.current = token_ctx.current->next;
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = TK_COMMA;
        add_child(enode, node);
        add_child(enode, assign_expr());
        // NEW: also populate tagged union fields
        enode->nu.binop.lhs = node;
        enode->nu.binop.rhs = enode->children[1];
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
    while (is_sc_spec(token_ctx.current->kind) || is_typespec(token_ctx.current->kind) || is_typequal(token_ctx.current->kind))
    {
        if (is_sc_spec(token_ctx.current->kind))  node->sclass   = token_ctx.current->kind;
        if (is_typespec(token_ctx.current->kind)) node->typespec |= to_typespec(token_ctx.current->kind);
        expect(token_ctx.current->kind);
    }
}

static Node *param_declaration()
{
    DBG_FUNC();
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
    if (token_ctx.current->kind == TK_STAR || token_ctx.current->kind == TK_IDENT || token_ctx.current->kind == TK_LPAREN)
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
    DBG_FUNC();
    Node *node = new_node(ND_PTYPE_LIST, 0, false);
    node->symtable = enter_new_scope(false);
    while(token_ctx.current->kind != TK_RPAREN)
    {
        add_child(node, param_declaration());
        if (token_ctx.current->kind == TK_COMMA)
        {
            expect(TK_COMMA);
            if (token_ctx.current->kind == TK_ELLIPSIS)
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
    DBG_FUNC();
    return equal_expr();
}
static Node *direct_decl()
{
    DBG_FUNC();
    // <direct-declarator> ::= <identifier> <direct-decl-tail>*
    //                       | ( <declarator> ) <direct-decl-tail>*
    // <direct-decl-tail> ::= [ {<constant-expression>}? ]
    //                      | ( <parameter-type-list> )
    //                      | ( {<identifier>}* )
    Node *node = new_node(ND_DIRECT_DECL, 0, false);
    if (token_ctx.current->kind == TK_IDENT)
    {
        add_child(node, new_node(ND_IDENT, expect(TK_IDENT), false));
    }
    else if (token_ctx.current->kind == TK_LPAREN)
    {
        expect(TK_LPAREN);
        add_child(node, declarator());
        expect(TK_RPAREN);
    }
    while(true)
    {
        if (token_ctx.current->kind == TK_LBRACKET)
        {
            add_child(node, new_node(ND_ARRAY_DECL, expect(TK_LBRACKET), false));
            Node *n = node->children[node->child_count - 1];
            if (token_ctx.current->kind != TK_RBRACKET)
            {
                add_child(n, constant_expr());
            }
            expect(TK_RBRACKET);
        }
        else if (token_ctx.current->kind == TK_LPAREN)
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
    DBG_FUNC();
    // <declarator> ::= {<pointer>}? <direct-declarator>
    // <pointer> ::= * {<type-qualifier>}* {<pointer>}?
    // <type-qualifier> ::= const
    //                    | volatile
    Node *node = new_node(ND_DECLARATOR, 0, false);
    node->pointer_level = 0;
    while(token_ctx.current->kind == TK_STAR)
    {
        expect(TK_STAR);
        node->pointer_level++;
        if (is_typequal(token_ctx.current->kind))
        {
            // TODO record this somehow
            expect(token_ctx.current->kind);
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
    while (token_ctx.current->kind == TK_COMMA)
    {
        expect(TK_COMMA);
        add_child(node, initializer());
    }
    // NEW: populate tagged union fields
    node->nu.initlist.count = node->child_count;
    node->nu.initlist.items = node->children;  // share the array (both old and new point to same)
    return node;
}
static Node *initializer()
{
    DBG_FUNC();
    // <initializer> ::= <assignment-expression>
    //                 | { <initializer-list> }
    //                 | { <initializer-list> , }  
    Node *node;
    if (token_ctx.current->kind == TK_LBRACE)
    {
        expect(token_ctx.current->kind);
        node = initializer_list();
        if (token_ctx.current->kind == TK_COMMA)
            expect(token_ctx.current->kind);
        expect(TK_RBRACE);
    }
    else
        node = assign_expr();
    return node;
}
static Node *init_declarator()
{
    DBG_FUNC();
    Node *node;
    node = declarator();
    if (token_ctx.current->kind == TK_ASSIGN)
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
    if (token_ctx.current->kind == TK_IDENT)
    {
        // struct or union definition with a tag, or an incomplete type, or a declaration.
        // Declarations using incomplete types are only valid if a pointer
        n = add_child(node, new_node(ND_STRUCT, 0, false));
        n->typespec |= (node->typespec & DS_UNION) ? DS_UNION : DS_STRUCT;
        add_child(n, new_node(ND_IDENT, expect(TK_IDENT), false));
    }
    if (token_ctx.current->kind == TK_LBRACE)
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
        while (token_ctx.current->kind != TK_RBRACE);
        leave_scope();
        expect(TK_RBRACE);
    }
}

static void enum_decl(Node *node)
{
    // Optional tag name
    char *tagname = NULL;
    if (token_ctx.current->kind == TK_IDENT)
    {
        tagname = strdup(token_ctx.current->val);
        expect(TK_IDENT);
    }
    if (!tagname)
        tagname = strdup(new_anon_label());

    if (token_ctx.current->kind == TK_LBRACE)
    {
        // Full definition: insert tag, parse body
        Symbol *tag_sym = insert_tag(node, tagname);
        Type  *ety     = get_enum_type(tag_sym);
        tag_sym->type   = ety;
        node->type      = ety;
        expect(TK_LBRACE);
        int next_val = 0;
        while (token_ctx.current->kind != TK_RBRACE)
        {
            char *ename = strdup(expect(TK_IDENT));
            if (token_ctx.current->kind == TK_ASSIGN)
            {
                expect(TK_ASSIGN);
                int sign = 1;
                if (token_ctx.current->kind == TK_MINUS) { sign = -1; expect(TK_MINUS); }
                next_val = sign * (int)token_ctx.current->ival;
                expect(TK_CONSTINT);
            }
            insert_enum_const(node, ety, ename, next_val++);
            if (token_ctx.current->kind == TK_COMMA)
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
    DBG_FUNC();
    // <declaration> ::=  {<declaration-specifier>}+ {<init-declarator>}* ;
    Node *node = new_node(ND_DECLARATION, 0, false);
    // At least one decl_spec
    // TODO storage class defaults A.8.1
    while(is_sc_spec(token_ctx.current->kind) || is_typespec(token_ctx.current->kind) || is_typequal(token_ctx.current->kind)
          || (token_ctx.current->kind == TK_IDENT && is_typedef_name(token_ctx.current->val) && node->typespec == 0))
    {
        if (is_sc_spec(token_ctx.current->kind))    node->sclass = token_ctx.current->kind;
        if (is_typespec(token_ctx.current->kind))   node->typespec |= to_typespec(token_ctx.current->kind);
        if (token_ctx.current->kind == TK_IDENT && is_typedef_name(token_ctx.current->val))
        {
            node->typespec |= DS_TYPEDEF;
            node->type      = find_typedef_type(token_ctx.current->val);
        }
        expect(token_ctx.current->kind);
    }
    if (node->typespec & (DS_STRUCT | DS_UNION))
    {
        struct_decl(node, depth);
    }
    else if (node->typespec & DS_ENUM)
    {
        enum_decl(node);
    }
    while(token_ctx.current->kind != TK_SEMICOLON)
    {
        add_child(node, init_declarator());
        if (token_ctx.current->kind != TK_SEMICOLON)
        {
            // We could get this far to find out this is a function definition.
            // If the next token_ctx.current is a '{' and we are not in a struct, union, enum, then
            // it is a func definition
            if (token_ctx.current->kind == TK_LBRACE)
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
    DBG_FUNC();
    Node *node      = new_node(ND_COMPSTMT, 0, false);
    node->symtable  = enter_new_scope(use_last_scope);
    DBG_PRINT("%s\n", curr_scope_str());
    if (token_ctx.current->kind == TK_LBRACE)
    {
        // <compound-statement> ::= { {<declaration-or-statement>}* }
        // C99 extension: declarations may appear anywhere in a block.
        expect(TK_LBRACE);
        while (token_ctx.current->kind != TK_RBRACE)
        {
            if (is_sc_spec(token_ctx.current->kind) || is_typespec(token_ctx.current->kind) || is_typequal(token_ctx.current->kind)
                || (token_ctx.current->kind == TK_IDENT && is_typedef_name(token_ctx.current->val)))
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
    DBG_FUNC();
    Node *node = new_node(ND_STMT, 0, false);
    if (token_ctx.current->kind == TK_LBRACE)
    {
        add_child(node, comp_stmt(false));
    }
    else if (token_ctx.current->kind == TK_IF)
    {
        node->kind = ND_IFSTMT;
        expect(TK_IF);
        expect(TK_LPAREN);
        Node *cond = expr();
        add_child(node, cond);
        expect(TK_RPAREN);
        Node *then_ = stmt();
        add_child(node, then_);
        // NEW: populate tagged union fields
        node->nu.ifstmt.cond = cond;
        node->nu.ifstmt.then_ = then_;
        node->nu.ifstmt.else_ = NULL;
        if (token_ctx.current->kind == TK_ELSE)
        {
            expect(TK_ELSE);
            Node *else_ = stmt();
            add_child(node, else_);
            node->nu.ifstmt.else_ = else_;
        }
    }
    else if (token_ctx.current->kind == TK_WHILE)
    {
        node->kind = ND_WHILESTMT;
        expect(TK_WHILE);
        expect(TK_LPAREN);
        Node *cond = expr();
        add_child(node, cond);
        expect(TK_RPAREN);
        Node *body = stmt();
        add_child(node, body);
        // NEW: populate tagged union fields
        node->nu.whilestmt.cond = cond;
        node->nu.whilestmt.body = body;
    }
    else if (token_ctx.current->kind == TK_FOR)
    {
        node->kind = ND_FORSTMT;
        expect(TK_FOR);
        expect(TK_LPAREN);
        // init: optional declaration (C99) or expression
        Node *init = NULL;
        if (is_sc_spec(token_ctx.current->kind) || is_typespec(token_ctx.current->kind) || is_typequal(token_ctx.current->kind)
            || (token_ctx.current->kind == TK_IDENT && is_typedef_name(token_ctx.current->val)))
        {
            // C99 for-init declaration: for (int i = 0; ...).
            // Enter an implicit scope so the variable is confined to the loop.
            node->symtable = enter_new_scope(false);
            init = declaration(0);   // declaration() consumes the ';'
            add_child(node, init);
        }
        else if (token_ctx.current->kind == TK_SEMICOLON)
        {
            init = new_node(ND_EMPTY, 0, false);
            add_child(node, init);
            expect(TK_SEMICOLON);
        }
        else
        {
            init = expr();
            add_child(node, init);
            expect(TK_SEMICOLON);
        }
        // condition (optional; absent = infinite loop)
        Node *cond = NULL;
        if (token_ctx.current->kind == TK_SEMICOLON)
        {
            cond = new_node(ND_EMPTY, 0, false);
            add_child(node, cond);
        }
        else
        {
            cond = expr();
            add_child(node, cond);
        }
        expect(TK_SEMICOLON);
        // increment (optional)
        Node *inc = NULL;
        if (token_ctx.current->kind == TK_RPAREN)
        {
            inc = new_node(ND_EMPTY, 0, false);
            add_child(node, inc);
        }
        else
        {
            inc = expr();
            add_child(node, inc);
        }
        expect(TK_RPAREN);
        Node *body = stmt();          // body
        add_child(node, body);
        // NEW: populate tagged union fields
        node->nu.forstmt.init = init;
        node->nu.forstmt.cond = cond;
        node->nu.forstmt.inc = inc;
        node->nu.forstmt.body = body;
        if (node->symtable)
            leave_scope();
    }
    else if (token_ctx.current->kind == TK_DO)
    {
        node->kind = ND_DOWHILESTMT;
        expect(TK_DO);
        Node *body = stmt();          // body
        add_child(node, body);
        expect(TK_WHILE);
        expect(TK_LPAREN);
        Node *cond = expr();          // condition
        add_child(node, cond);
        expect(TK_RPAREN);
        expect(TK_SEMICOLON);
        // NEW: populate tagged union fields
        node->nu.dowhile.body = body;
        node->nu.dowhile.cond = cond;
    }
    else if (token_ctx.current->kind == TK_SWITCH)
    {
        node->kind = ND_SWITCHSTMT;
        expect(TK_SWITCH);
        expect(TK_LPAREN);
        add_child(node, expr());          // selector
        expect(TK_RPAREN);
        add_child(node, comp_stmt(false));// body (always a compound)
    }
    else if (token_ctx.current->kind == TK_CASE)
    {
        node->kind = ND_CASESTMT;
        expect(TK_CASE);
        if (token_ctx.current->kind == TK_IDENT)
        {
            Symbol *s = find_symbol(node, token_ctx.current->val, NS_IDENT);
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
    else if (token_ctx.current->kind == TK_DEFAULT)
    {
        node->kind = ND_DEFAULTSTMT;
        expect(TK_DEFAULT);
        expect(TK_COLON);
    }
    else if (token_ctx.current->kind == TK_BREAK)
    {
        node->kind = ND_BREAKSTMT;
        expect(TK_BREAK);
        expect(TK_SEMICOLON);
    }
    else if (token_ctx.current->kind == TK_CONTINUE)
    {
        node->kind = ND_CONTINUESTMT;
        expect(TK_CONTINUE);
        expect(TK_SEMICOLON);
    }
    else if (token_ctx.current->kind == TK_GOTO)
    {
        node->kind = ND_GOTOSTMT;
        expect(TK_GOTO);
        node->u.label = strdup(expect_ident());
        expect(TK_SEMICOLON);
    }
    else if (token_ctx.current->kind == TK_IDENT)
    {
        char *name = expect(TK_IDENT);
        if (token_ctx.current->kind == TK_COLON)
        {
            node->kind = ND_LABELSTMT;
            node->u.label = strdup(name);
            expect(TK_COLON);
            Node *target = stmt();
            add_child(node, target);
            // NEW: populate tagged union fields
            node->nu.labelstmt.name = strdup(name);
            node->nu.labelstmt.stmt = target;
        }
        else
        {
            unget_token();
            node->kind = ND_EXPRSTMT;
            Node *e = expr();
            add_child(node, e);
            // NEW: populate tagged union field
            node->nu.exprstmt.decl = e;
            expect(TK_SEMICOLON);
        }
    }
    else if (token_ctx.current->kind == TK_RETURN)
    {
        node->kind = ND_RETURNSTMT;
        expect(TK_RETURN);
        Node *ret_expr = NULL;
        if (token_ctx.current->kind != TK_SEMICOLON)
        {
            ret_expr = expr();
            add_child(node, ret_expr);
        }
        expect(TK_SEMICOLON);
        // NEW: populate tagged union field
        node->nu.returnstmt.expr = ret_expr;
    }
    else if (token_ctx.current->kind != TK_SEMICOLON)
    {
        node->kind = ND_EXPRSTMT;
        Node *e = expr();
        add_child(node, e);
        // NEW: populate tagged union field
        node->nu.exprstmt.decl = e;
        expect(TK_SEMICOLON);
    }
    return node;
}
Node *program()
{
    DBG_FUNC();
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

// Helper to get display string from node's union based on kind
static const char *node_val_str(Node *node)
{
    if (!node) return "";
    switch (node->kind) {
        case ND_IDENT:
            return node->u.ident ? node->u.ident : "";
        case ND_LABELSTMT:
        case ND_GOTOSTMT:
            return node->u.label ? node->u.label : "";
        case ND_DECLARATOR:
        case ND_DIRECT_DECL:
            return node->u.ident ? node->u.ident : "";
        default:
            return "";
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
        node_val_str(node),
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
            node_val_str(node),
            node->pointer_level,
            node->is_function ? "func" :
            node->kind == ND_ARRAY_DECL ? "array" : "");
    }
    return buf;
}

void print_tree(Node *node, int depth)
{
    if (depth==0) DBG_PRINT("------ Parse tree ------\n");
    if (!node)
        return;
    for(int i = 0; i < depth; i++)
        DBG_PRINT("  ");
    DBG_PRINT("%s", node_str(node));
    DBG_PRINT("\n");
    for(int i = 0; i < node->child_count; i++)
        print_tree(node->children[i], depth + 1);
}


const char *get_decl_ident(Node *node)
{
    if (node->child_count)
    {
        if (node->children[0]->kind == ND_IDENT)
            return node->children[0]->u.ident;
        return get_decl_ident(node->children[0]);
    }
    return 0;
}



void insert_cast(Node *n, int child, Type *t)
{
    DBG_PRINT("%s %s\n", __func__, fulltype_str(t));
    Node *c = new_node(ND_CAST, 0, true);

    // We don't care what is in the declaration, its just a placeholder
    add_child(c, new_node(ND_DECLARATION, 0, false));
    add_child(c, n->children[child]);
    c->type = t;
    // Set the CAST node's own nu.cast.expr to point to the expression (children[1])
    c->nu.cast.expr = c->children[1];
    n->children[child] = c;
    // Update nu.* fields to match children[]
    if (n->kind == ND_BINOP || n->kind == ND_ASSIGN) {
        if (child == 0) n->nu.binop.lhs = c;
        else if (child == 1) n->nu.binop.rhs = c;
    } else if (n->kind == ND_RETURNSTMT && child == 0) {
        n->nu.returnstmt.expr = c;
    } else if (n->kind == ND_UNARYOP && child == 0) {
        n->nu.unaryop.operand = c;
    } else if (n->kind == ND_WHILESTMT && child == 0) {
        n->nu.whilestmt.cond = c;
    } else if (n->kind == ND_FORSTMT) {
        if (child == 0) n->nu.forstmt.init = c;
        else if (child == 1) n->nu.forstmt.cond = c;
        else if (child == 2) n->nu.forstmt.inc = c;
        else if (child == 3) n->nu.forstmt.body = c;
    } else if (n->kind == ND_DOWHILESTMT) {
        if (child == 0) n->nu.dowhile.body = c;
        else if (child == 1) n->nu.dowhile.cond = c;
    } else if (n->kind == ND_IFSTMT && child == 0) {
        n->nu.ifstmt.cond = c;
    } else if (n->kind == ND_EXPRSTMT && child == 0) {
        n->nu.exprstmt.decl = c;
    } else if (n->kind == ND_CAST && child == 1) {
        n->nu.cast.expr = c;
    } else if (n->kind == ND_TERNARY) {
        if (child == 0) n->nu.ternary.cond = c;
        else if (child == 1) n->nu.ternary.then_ = c;
        else if (child == 2) n->nu.ternary.else_ = c;
    } else if (n->kind == ND_COMPOUND_ASSIGN) {
        if (child == 0) n->nu.compound_assign.lhs = c;
        else if (child == 1) n->nu.compound_assign.rhs = c;
    } else if (n->kind == ND_VA_START) {
        if (child == 0) n->nu.vastart.ap = c;
        else if (child == 1) n->nu.vastart.last = c;
    } else if (n->kind == ND_VA_ARG && child == 0) {
        n->nu.vaarg.ap = c;
    } else if (n->kind == ND_VA_END && child == 0) {
        n->nu.vaend.ap = c;
    }
}
Type *check_operands(Node *n)
{
    // Perform the 'usual arithmetic conversions' (C89 §3.2.1.5).
    // Promotions use the *original* lhs/rhs types for checks; insert_cast
    // replaces n->children[], so we return n->children[0]->type at the end
    // to get the post-cast result type (not lhs->type which stays pre-cast).
    Node *lhs = n->children[0];
    Node *rhs = n->children[1];
    DBG_PRINT("%s %016llx %016llx\n", __func__, (unsigned long long)lhs->type, (unsigned long long)rhs->type);

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
    DBG_PRINT("%s %016llx\n", __func__, (unsigned long long)lhs->type);

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
    DBG_FUNC();
    for(int i = 0; i < n->child_count; i++)
    {
        propagate_types(n, n->children[i]);
    }
    if (!p)
    {
        // We are at the top of the tree, nothing further to do
        DBG_PRINT("%s finished\n", __func__);
        return;
    }
    DBG_PRINT("Before: %s\n", node_str(n));
    if (n->kind == ND_IDENT && n->is_expr)
    {
        if (p->kind != ND_MEMBER)
            n->type = find_symbol(n, n->u.ident, NS_IDENT)->type;
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
            lhs->type = find_symbol(lhs, lhs->u.ident, NS_IDENT)->type;
        }
        // For ->, dereference the pointer to get the struct type
        Type *struct_type = lhs->type;
        if (n->op_kind == TK_ARROW)
        {
            if (!istype_ptr(struct_type))
                error("'->' requires pointer type\n");
            struct_type = struct_type->u.ptr.pointee;
        }
        DBG_PRINT("%s looking in lhs type:%016llx for field %s\n", __func__, (unsigned long long)struct_type, rhs->u.ident);
        Type *base = 0;
        n->offset = find_offset(struct_type, rhs->u.ident, &base);
        if (n->offset < 0)
            error("Can't find member %s in struct\n", rhs->u.ident);
        DBG_PRINT("%s found member %s with offset %d basetype %016llx\n", __func__,
            rhs->u.ident, n->offset, (unsigned long long)base);
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
        DBG_PRINT("%s found return stmt with expr, type of func:%s:\n", __func__, fulltype_str(n->type));
        if (n->type != n->children[0]->type)
            insert_cast(n, 0, n->type);
    }
    DBG_PRINT("After : %s\n", node_str(n));
}

