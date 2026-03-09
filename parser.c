
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

static Node *primary_expr();
static Node *unary_expr();
static Node *type_name();
static Node *assign_expr();
static Node *expr();
static Node *stmt();
static Node *func_def();
static Node *declarator();
static Node *init_declarator();
static Node *declaration(int depth);
// Additional forwards needed by unary_expr (sizeof, cast)
static bool is_type_name_or_type(Token *tok);
static Node *struct_decl(DeclParseState *ds, int depth);
static void enum_decl(DeclParseState *ds);
static void parse_decl_specifiers(DeclParseState *ds);
static Node *make_decl_node(DeclParseState *ds, Node *spec, Node *decls);





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
            node->u.ident.name = strdup(val);
        else if (kind == ND_GOTOSTMT)
            node->u.label = strdup(val);
    }
    return node;
}



static void list_append(Node **head, Node *item)
{
    if (!*head) { *head = item; return; }
    Node *p = *head;
    while (p->next) p = p->next;
    p->next = item;
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
        node->u.vastart.ap = assign_expr();
        expect(TK_COMMA);
        node->u.vastart.last = assign_expr();
        expect(TK_RPAREN);
        return node;
    }
    else if (token_ctx.current->kind == TK_IDENT && !strcmp(token_ctx.current->val, "va_arg"))
    {
        expect(TK_IDENT);
        expect(TK_LPAREN);
        node = new_node(ND_VA_ARG, NULL, true);
        node->u.vaarg.ap = assign_expr();
        expect(TK_COMMA);
        Node *tn = type_name();           // type to fetch
        node->type = tn->type;
        expect(TK_RPAREN);
        return node;
    }
    else if (token_ctx.current->kind == TK_IDENT && !strcmp(token_ctx.current->val, "va_end"))
    {
        expect(TK_IDENT);
        expect(TK_LPAREN);
        node = new_node(ND_VA_END, NULL, true);
        node->type = t_void;
        node->u.vaend.ap = assign_expr();
        expect(TK_RPAREN);
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

        if (tk->kind == TK_CONSTINT)
        {
            //  dec     int,            l int,  ul int
            //  hex     int,    u int,  l int,  ul int
            //  u               u int,          ul int
            //  l                       l int,  ul int
            //  ul                              ul int
            long long i = node->u.literal.ival = tk->ival;
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
            double f = node->u.literal.fval = tk->fval;
            if (f >= -3.402823466e38 && f <= 3.402823466e38)        node->type = t_float;
            else error("Float constant out of range");
        }
    }
    else if (token_ctx.current->kind == TK_CHARACTER)
    {
        int char_val = (int)token_ctx.current->val[0];
        char c[64];
        sprintf(c, "%d", char_val);
        expect(TK_CHARACTER);
        DBG_PRINT("%s %s\n", c, token_ctx.current->val);
        node = new_node(ND_LITERAL, c, true);
        node->u.literal.ival = char_val;
        node->type = t_char;
    }
    else if (token_ctx.current->kind == TK_STRING)
    {
        node = new_node(ND_LITERAL, NULL, true);
        node->u.literal.strval     = token_ctx.current->val;
        node->u.literal.strval_len = (int)token_ctx.current->ival;
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
            node->u.literal.ival = tn->type->size;
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
            node->u.literal.ival = sz;
        }
        return node;
    }
    else if (token_ctx.current->kind == TK_INC || token_ctx.current->kind == TK_DEC
            ||  token_ctx.current->kind == TK_AMPERSAND || token_ctx.current->kind == TK_STAR
            ||  token_ctx.current->kind == TK_PLUS || token_ctx.current->kind == TK_MINUS
            ||  token_ctx.current->kind == TK_BANG || token_ctx.current->kind == TK_TILDE)
    {
        Token_kind k = token_ctx.current->kind;
        token_ctx.current = token_ctx.current->next;  // consume the operator token_ctx.current
        node = new_node(ND_UNARYOP, NULL, true);
        node->op_kind = k;
        node->u.unaryop.operand = unary_expr();
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
            node->symbol = find_symbol(node, node->u.ident.name, NS_IDENT);
        }
    }
    // postfix-expr
    // Keep pointer to array or func ident, so we can reference later during fixup
    Symbol *s = node->symbol;
    int array_depth = 0;
    // pex_node tracks the "most recent base node" in the postfix chain.
    // When '(' is encountered, pex_node->is_function is set — marking that node as the
    // call site. For simple calls: pex_node == node (the ident/literal). For member calls
    // (s.fp() or s->fp()): pex_node is the ND_MEMBER node after processing '.' or '->'.
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
                        add_node->u.binop.lhs = e1;
                        add_node->u.binop.rhs = idx;
                        node = new_node(ND_UNARYOP, NULL, true);
                        node->op_kind = TK_STAR;
                        node->type = elem;
                        node->u.unaryop.operand = add_node;
                        break;
                    }
                    if (!s)
                        error("No ident before left bracket\n");
                    expect(token_ctx.current->kind);
                    Node *e1 = node;
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
                        outer_unary->u.unaryop.is_array_deref = true;
                    }
                    node = outer_unary;
                    Node *add = new_node(ND_BINOP, NULL, true);
                    add->op_kind = TK_PLUS;
                    Node *mul = new_node(ND_BINOP, NULL, true);
                    mul->op_kind = TK_STAR;
                    // stride = size of element at this array depth
                    int mult = arr_at_depth->u.arr.elem->size;
                    array_depth++;
                    Node *stride_lit = new_node(ND_LITERAL, NULL, true);
                    stride_lit->u.literal.ival = mult;
                    mul->u.binop.lhs = stride_lit;
                    mul->u.binop.rhs = expr();
                    add->u.binop.lhs = e1;
                    add->u.binop.rhs = mul;
                    outer_unary->u.unaryop.operand = add;
                    expect(TK_RBRACKET);
                    break;
                }
            case(TK_LPAREN):
                // Function call: route args into the appropriate u.*.args linked list
                expect(TK_LPAREN);
                if (pex_node->kind == ND_IDENT)         pex_node->u.ident.is_function = true;
                else if (pex_node->kind == ND_UNARYOP)  pex_node->u.unaryop.is_function = true;
                else if (pex_node->kind == ND_MEMBER)   pex_node->u.member.is_function = true;
                if (token_ctx.current->kind != TK_RPAREN)
                {
                    if (node->kind == ND_IDENT)
                    {
                        list_append(&node->u.ident.args, assign_expr());
                        while (token_ctx.current->kind == TK_COMMA)
                        {
                            token_ctx.current = token_ctx.current->next;
                            list_append(&node->u.ident.args, assign_expr());
                        }
                    }
                    else if (node->kind == ND_UNARYOP)
                    {
                        list_append(&node->u.unaryop.args, assign_expr());
                        while (token_ctx.current->kind == TK_COMMA)
                        {
                            token_ctx.current = token_ctx.current->next;
                            list_append(&node->u.unaryop.args, assign_expr());
                        }
                    }
                    else if (node->kind == ND_MEMBER)
                    {
                        list_append(&node->u.member.args, assign_expr());
                        while (token_ctx.current->kind == TK_COMMA)
                        {
                            token_ctx.current = token_ctx.current->next;
                            list_append(&node->u.member.args, assign_expr());
                        }
                    }
                    else
                    {
                        error("Unexpected node kind %s at function call\n", nodestr(node->kind));
                    }
                }
                expect(TK_RPAREN);
                break;
            case(TK_DOT):
            {
                expect(token_ctx.current->kind);
                Node *n = new_node(ND_MEMBER, NULL, true);
                n->op_kind = TK_DOT;
                n->u.member.base = node;
                n->u.member.field_name = strdup(expect(TK_IDENT));
                node = n;
                pex_node = n;
                break;
            }
            case(TK_ARROW):
            {
                expect(TK_ARROW);
                Node *n = new_node(ND_MEMBER, NULL, true);
                n->op_kind = TK_ARROW;
                n->u.member.base = node;
                n->u.member.field_name = strdup(expect(TK_IDENT));
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
                n->u.unaryop.operand = node;
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
    DeclParseState ds = {0};
    if (token_ctx.current->kind == TK_IDENT && is_typedef_name(token_ctx.current->val))
    {
        ds.typedef_type = find_typedef_type(token_ctx.current->val);
        ds.typespec = DS_TYPEDEF;
        expect(TK_IDENT);
        if (token_ctx.current->kind == TK_STAR)
        {
            expect(TK_STAR);
            ds.typedef_type = get_pointer_type(ds.typedef_type);
        }
        Node *node = make_decl_node(&ds, NULL, NULL);
        node->type = ds.typedef_type;
        return node;
    }
    parse_decl_specifiers(&ds);
    if (ds.typespec & DS_ENUM)
    {
        enum_decl(&ds);
    }
    Node *decls = NULL;
    while(token_ctx.current->kind != TK_RPAREN)
    {
        list_append(&decls, declarator());
    }
    Node *node = make_decl_node(&ds, NULL, decls);
    node->type = type2_from_decl_node(node, ds);
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
        Node *tn = type_name();
        node->type = tn->type;
        expect(TK_RPAREN);
        Node *cexpr = cast_expr();
        node->u.cast.type_decl = tn;
        node->u.cast.expr = cexpr;
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
// Generic binary-expression parser.
// sub_parser: function to parse the next-lower-precedence operand
// ops: NULL-terminated array of Token_kinds that trigger this level
static Node *parse_binop(Node *(*sub_parser)(void), const Token_kind *ops)
{
    Node *node = sub_parser();
    for (;;)
    {
        Token_kind k = token_ctx.current->kind;
        bool matched = false;
        for (int i = 0; ops[i] != TK_INVALID; i++)
            if (k == ops[i]) { matched = true; break; }
        if (!matched) break;
        expect(k);
        Node *enode = new_node(ND_BINOP, NULL, true);
        enode->op_kind = k;
        enode->u.binop.lhs = node;
        enode->u.binop.rhs = sub_parser();
        node = enode;
    }
    return node;
}

static const Token_kind ops_mult[]   = { TK_STAR, TK_SLASH, TK_PERCENT, TK_INVALID };
static const Token_kind ops_add[]    = { TK_PLUS, TK_MINUS, TK_INVALID };
static const Token_kind ops_shift[]  = { TK_SHIFTL, TK_SHIFTR, TK_INVALID };
static const Token_kind ops_rel[]    = { TK_LT, TK_LE, TK_GT, TK_GE, TK_INVALID };
static const Token_kind ops_equal[]  = { TK_EQ, TK_NE, TK_INVALID };
static const Token_kind ops_bitand[] = { TK_AMPERSAND, TK_INVALID };
static const Token_kind ops_bitxor[] = { TK_BITXOR, TK_INVALID };
static const Token_kind ops_bitor[]  = { TK_BITOR, TK_INVALID };
static const Token_kind ops_logand[] = { TK_LOGAND, TK_INVALID };
static const Token_kind ops_logor[]  = { TK_LOGOR, TK_INVALID };

// Forward declarations needed for parse_binop sub_parser references
static Node *cast_expr();
static Node *add_expr();
static Node *shift_expr();
static Node *rel_expr();
static Node *equal_expr();
static Node *bitand_expr();
static Node *bitxor_expr();
static Node *bitor_expr();
static Node *logand_expr();

static Node *mult_expr()   { return parse_binop(cast_expr,    ops_mult);   }
static Node *add_expr()    { return parse_binop(mult_expr,    ops_add);    }
static Node *shift_expr()  { return parse_binop(add_expr,     ops_shift);  }
static Node *rel_expr()    { return parse_binop(shift_expr,   ops_rel);    }
static Node *equal_expr()  { return parse_binop(rel_expr,     ops_equal);  }
static Node *bitand_expr() { return parse_binop(equal_expr,   ops_bitand); }
static Node *bitxor_expr() { return parse_binop(bitand_expr,  ops_bitxor); }
static Node *bitor_expr()  { return parse_binop(bitxor_expr,  ops_bitor);  }
static Node *logand_expr() { return parse_binop(bitor_expr,   ops_logand); }
static Node *logor_expr()  { return parse_binop(logand_expr,  ops_logor);  }

static Node **get_u_child_slot(Node *n, int child);
void insert_scale(Node *n, int child, int size)
{
    Node *sc = new_node(ND_BINOP, NULL, true);
    sc->op_kind = TK_STAR;
    Node *lit = new_node(ND_LITERAL, NULL, true);
    lit->u.literal.ival = size;

    Node **slot = get_u_child_slot(n, child);
    Node *original = *slot;

    sc->u.binop.lhs = lit;
    sc->u.binop.rhs = original;
    *slot = sc;
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
    tnode->u.ternary.cond = node;
    tnode->u.ternary.then_ = expr();
    expect(TK_COLON);
    tnode->u.ternary.else_ = cond_expr();
    return tnode;
}
static Node *assign_expr()
{
    DBG_FUNC_TOKEN(token_ctx.current);
    Node *node = cond_expr();
    if (token_ctx.current->kind == TK_ASSIGN)
    {
        Node *anode = new_node(ND_ASSIGN, expect(TK_ASSIGN), true);
        anode->u.binop.lhs = node;
        anode->u.binop.rhs = assign_expr();
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
        anode->u.compound_assign.lhs = node;
        anode->u.compound_assign.rhs = assign_expr();
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
        enode->u.binop.lhs = node;
        enode->u.binop.rhs = assign_expr();
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

static StorageClass tk_to_sc(Token_kind tk)
{
    switch (tk) {
        case TK_AUTO:     return SC_AUTO;
        case TK_REGISTER: return SC_REGISTER;
        case TK_STATIC:   return SC_STATIC;
        case TK_EXTERN:   return SC_EXTERN;
        case TK_TYPEDEF:  return SC_TYPEDEF;
        default:          return SC_NONE;
    }
}

const char *sc_str(StorageClass sc)
{
    switch (sc) {
        case SC_NONE:     return "none";
        case SC_AUTO:     return "auto";
        case SC_REGISTER: return "register";
        case SC_STATIC:   return "static";
        case SC_EXTERN:   return "extern";
        case SC_TYPEDEF:  return "typedef";
        default:          return "?";
    }
}

static void parse_decl_specifiers(DeclParseState *ds)
{
    while (is_sc_spec(token_ctx.current->kind) || is_typespec(token_ctx.current->kind) || is_typequal(token_ctx.current->kind)
           || (token_ctx.current->kind == TK_IDENT && is_typedef_name(token_ctx.current->val) && ds->typespec == 0))
    {
        if (is_sc_spec(token_ctx.current->kind))  ds->sclass = tk_to_sc(token_ctx.current->kind);
        if (is_typespec(token_ctx.current->kind)) ds->typespec |= to_typespec(token_ctx.current->kind);
        if (token_ctx.current->kind == TK_IDENT && is_typedef_name(token_ctx.current->val))
        {
            ds->typespec    |= DS_TYPEDEF;
            ds->typedef_type = find_typedef_type(token_ctx.current->val);
        }
        expect(token_ctx.current->kind);
    }
}

static Node *make_decl_node(DeclParseState *ds, Node *spec, Node *decls)
{
    Node *node = new_node(ND_DECLARATION, NULL, false);
    node->u.declaration.spec    = spec;
    node->u.declaration.decls   = decls;
    node->u.declaration.typespec = ds->typespec;
    node->u.declaration.sclass   = ds->sclass;
    // Propagate typedef_type into node->type for use by type2_from_decl_node
    if (ds->typedef_type)
        node->type = ds->typedef_type;
    return node;
}

static Node *param_declaration()
{
    DBG_FUNC();
    // <parameter-list> ::= <parameter-declaration> <parameter-list-tail>*
    // <parameter-list-tail> ::= , <parameter-declaration>
    // <parameter-declaration> ::= {<declaration-specifier>}+ <declarator>
    //                           | {<declaration-specifier>}+ <abstract-declarator>
    //                           | {<declaration-specifier>}+
    DeclParseState ds = {0};
    // At least one decl_spec
    // TODO storage class defaults A.8.1
    parse_decl_specifiers(&ds);
    Node *spec = NULL;
    if (ds.typespec & (DS_STRUCT | DS_UNION))
    {
        spec = struct_decl(&ds, 0);
    }
    else if (ds.typespec & DS_ENUM)
    {
        enum_decl(&ds);
    }
    Node *decls = NULL;
    if (token_ctx.current->kind == TK_STAR || token_ctx.current->kind == TK_IDENT || token_ctx.current->kind == TK_LPAREN)
    {
        list_append(&decls, declarator());
    }
    // else: abstract declarator (no name) — valid in C89

    Node *node = make_decl_node(&ds, spec, decls);
    // At this point, we can add the symbols and types to the tables
    add_types_and_symbols(node, ds, true, 0);
    return node;
}
static Node *param_type_list()
{
    DBG_FUNC();
    Node *node = new_node(ND_PTYPE_LIST, 0, false);
    node->symtable = enter_new_scope(false);
    while(token_ctx.current->kind != TK_RPAREN)
    {
        list_append(&node->u.ptype_list.params, param_declaration());
        if (token_ctx.current->kind == TK_COMMA)
        {
            expect(TK_COMMA);
            if (token_ctx.current->kind == TK_ELLIPSIS)
            {
                expect(TK_ELLIPSIS);
                node->u.ptype_list.is_variadic = true;
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
    // C89 §3.4: constant-expression ::= conditional-expression
    DBG_FUNC();
    return cond_expr();
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
        node->u.direct_decl.name = new_node(ND_IDENT, expect(TK_IDENT), false);
    }
    else if (token_ctx.current->kind == TK_LPAREN)
    {
        expect(TK_LPAREN);
        node->u.direct_decl.name = declarator();
        expect(TK_RPAREN);
    }
    while(true)
    {
        if (token_ctx.current->kind == TK_LBRACKET)
        {
            Node *arr = new_node(ND_ARRAY_DECL, expect(TK_LBRACKET), false);
            list_append(&node->u.direct_decl.suffixes, arr);
            if (token_ctx.current->kind != TK_RBRACKET)
            {
                arr->u.array_decl.size = constant_expr();
            }
            expect(TK_RBRACKET);
        }
        else if (token_ctx.current->kind == TK_LPAREN)
        {
            Node *fn = new_node(ND_FUNC_DECL, expect(TK_LPAREN), false);
            list_append(&node->u.direct_decl.suffixes, fn);
            // ND_FUNC_DECL kind itself signals "function suffix"; no is_function field needed
            // TODO can also be identifier
            fn->u.func_decl.params = param_type_list();
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
    node->u.declarator.pointer_level = 0;
    while(token_ctx.current->kind == TK_STAR)
    {
        expect(TK_STAR);
        node->u.declarator.pointer_level++;
        if (is_typequal(token_ctx.current->kind))
        {
            // TODO record this somehow
            expect(token_ctx.current->kind);
        }
    }
    node->u.declarator.direct_decl = direct_decl();
    return node;
}
static Node *initializer();
static Node *initializer_list()
{
    Node *node = new_node(ND_INITLIST, 0, false);
    list_append(&node->u.initlist.items, initializer());
    while (token_ctx.current->kind == TK_COMMA)
    {
        expect(TK_COMMA);
        list_append(&node->u.initlist.items, initializer());
    }
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
        node->u.declarator.init = initializer();
    }
    return node;
}
static Node *comp_stmt(bool use_last_scope);

static Node *struct_decl(DeclParseState *ds, int depth)
{
    // struct-or-union-specifier ::= struct-or-union identifier "{" struct-declaration+ "}"
    //                             | struct-or-union "{" struct-declaration+ "}"
    //                             | struct-or-union identifier
    Node *n = NULL;
    if (token_ctx.current->kind == TK_IDENT)
    {
        // struct or union definition with a tag, or an incomplete type, or a declaration.
        // Declarations using incomplete types are only valid if a pointer
        n = new_node(ND_STRUCT, 0, false);
        n->u.struct_spec.is_union = (ds->typespec & DS_UNION) != 0;
        n->u.struct_spec.tag = new_node(ND_IDENT, expect(TK_IDENT), false);
    }
    if (token_ctx.current->kind == TK_LBRACE)
    {
        if (!n)
        {
            // Anonymous struct or union definition
            n = new_node(ND_STRUCT, 0, false);
            n->u.struct_spec.is_union = (ds->typespec & DS_UNION) != 0;
            n->u.struct_spec.tag = new_node(ND_IDENT, new_anon_label(), false);
        }
        expect(TK_LBRACE);
        n->symtable = enter_new_scope(false);
        n->symtable->scope.scope_type = ST_STRUCT;
        do
        {
            list_append(&n->u.struct_spec.members, declaration(depth + 1));
        }
        while (token_ctx.current->kind != TK_RBRACE);
        leave_scope();
        expect(TK_RBRACE);
    }
    return n;
}

static void enum_decl(DeclParseState *ds)
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
        Symbol *tag_sym = insert_tag(type_ctx.curr_scope_st, tagname);
        Type  *ety     = get_enum_type(tag_sym);
        tag_sym->type   = ety;
        ds->typedef_type = ety;
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
            insert_enum_const(type_ctx.curr_scope_st, ety, ename, next_val++);
            if (token_ctx.current->kind == TK_COMMA)
                expect(TK_COMMA);
        }
        expect(TK_RBRACE);
    }
    else
    {
        // Tag-only reference
        Symbol *tag_sym = find_symbol_st(type_ctx.curr_scope_st, tagname, NS_TAG);
        ds->typedef_type = tag_sym ? tag_sym->type : t_int;
    }
}

static Node *declaration(int depth)
{
    DBG_FUNC();
    // <declaration> ::=  {<declaration-specifier>}+ {<init-declarator>}* ;
    DeclParseState ds = {0};
    // At least one decl_spec (including optional typedef-name)
    parse_decl_specifiers(&ds);
    Node *spec = NULL;
    if (ds.typespec & (DS_STRUCT | DS_UNION))
    {
        spec = struct_decl(&ds, depth);
    }
    else if (ds.typespec & DS_ENUM)
    {
        enum_decl(&ds);
    }
    Node *decls = NULL;
    while(token_ctx.current->kind != TK_SEMICOLON)
    {
        list_append(&decls, init_declarator());
        if (token_ctx.current->kind != TK_SEMICOLON)
        {
            // We could get this far to find out this is a function definition.
            // If the next token_ctx.current is a '{' and we are not in a struct, union, enum, then
            // it is a func definition
            if (token_ctx.current->kind == TK_LBRACE)
            {
                Node *node = make_decl_node(&ds, spec, decls);
                add_types_and_symbols(node, ds, false, 0);
                parser_ctx.current_function = node;
                // This is the first compound statement of a
                // function, so we need to use the scope
                // created in the parameter list
                node->u.declaration.func_body = comp_stmt(true);
                node->u.declaration.is_func_defn = true;
                return node;
            }
            expect(TK_COMMA);
        }
    }

    expect(TK_SEMICOLON);
    // At this point, we can add the symbols and types to the tables
    // We don't add struct members but we do add tags
    Node *node = make_decl_node(&ds, spec, decls);
    add_types_and_symbols(node, ds, false, depth);
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
                list_append(&node->u.compstmt.stmts, declaration(0));
            else
                list_append(&node->u.compstmt.stmts, stmt());
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
        node->u.stmt_wrap.body = comp_stmt(false);
    }
    else if (token_ctx.current->kind == TK_IF)
    {
        node->kind = ND_IFSTMT;
        expect(TK_IF);
        expect(TK_LPAREN);
        node->u.ifstmt.cond = expr();
        expect(TK_RPAREN);
        node->u.ifstmt.then_ = stmt();
        node->u.ifstmt.else_ = NULL;
        if (token_ctx.current->kind == TK_ELSE)
        {
            expect(TK_ELSE);
            node->u.ifstmt.else_ = stmt();
        }
    }
    else if (token_ctx.current->kind == TK_WHILE)
    {
        node->kind = ND_WHILESTMT;
        expect(TK_WHILE);
        expect(TK_LPAREN);
        node->u.whilestmt.cond = expr();
        expect(TK_RPAREN);
        node->u.whilestmt.body = stmt();
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
        }
        else if (token_ctx.current->kind == TK_SEMICOLON)
        {
            init = new_node(ND_EMPTY, 0, false);
            expect(TK_SEMICOLON);
        }
        else
        {
            init = expr();
            expect(TK_SEMICOLON);
        }
        node->u.forstmt.init = init;
        // condition (optional; absent = infinite loop)
        if (token_ctx.current->kind == TK_SEMICOLON)
            node->u.forstmt.cond = new_node(ND_EMPTY, 0, false);
        else
            node->u.forstmt.cond = expr();
        expect(TK_SEMICOLON);
        // increment (optional)
        if (token_ctx.current->kind == TK_RPAREN)
            node->u.forstmt.inc = new_node(ND_EMPTY, 0, false);
        else
            node->u.forstmt.inc = expr();
        expect(TK_RPAREN);
        node->u.forstmt.body = stmt();
        if (node->symtable)
            leave_scope();
    }
    else if (token_ctx.current->kind == TK_DO)
    {
        node->kind = ND_DOWHILESTMT;
        expect(TK_DO);
        node->u.dowhile.body = stmt();
        expect(TK_WHILE);
        expect(TK_LPAREN);
        node->u.dowhile.cond = expr();
        expect(TK_RPAREN);
        expect(TK_SEMICOLON);
    }
    else if (token_ctx.current->kind == TK_SWITCH)
    {
        node->kind = ND_SWITCHSTMT;
        expect(TK_SWITCH);
        expect(TK_LPAREN);
        node->u.switchstmt.selector = expr();
        expect(TK_RPAREN);
        node->u.switchstmt.body = comp_stmt(false);
    }
    else if (token_ctx.current->kind == TK_CASE)
    {
        node->kind = ND_CASESTMT;
        expect(TK_CASE);
        if (token_ctx.current->kind == TK_IDENT)
        {
            Symbol *s = find_symbol_st(node->st, token_ctx.current->val, NS_IDENT);
            if (!s || !s->is_enum_const)
                error("Expected integer constant in case\n");
            node->u.casestmt.value = (long long)s->offset;
            expect(TK_IDENT);
        }
        else
        {
            node->u.casestmt.value = (long long)expect_number();
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
            expect(TK_COLON);
            node->u.labelstmt.name = strdup(name);
            node->u.labelstmt.stmt = stmt();
        }
        else
        {
            unget_token();
            node->kind = ND_EXPRSTMT;
            node->u.exprstmt.expr = expr();
            expect(TK_SEMICOLON);
        }
    }
    else if (token_ctx.current->kind == TK_RETURN)
    {
        node->kind = ND_RETURNSTMT;
        expect(TK_RETURN);
        if (token_ctx.current->kind != TK_SEMICOLON)
            node->u.returnstmt.expr = expr();
        expect(TK_SEMICOLON);
    }
    else if (token_ctx.current->kind != TK_SEMICOLON)
    {
        node->kind = ND_EXPRSTMT;
        node->u.exprstmt.expr = expr();
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
        list_append(&node->u.program.decls, declaration(0));
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
            return node->u.ident.name ? node->u.ident.name : "";
        case ND_LABELSTMT:
            return node->u.labelstmt.name ? node->u.labelstmt.name : "";
        case ND_GOTOSTMT:
            return node->u.label ? node->u.label : "";
        case ND_DECLARATOR:
            return "";  // name is in u.declarator.direct_decl
        case ND_DIRECT_DECL:
        {
            Node *nm = node->u.direct_decl.name;
            return (nm && nm->kind == ND_IDENT && nm->u.ident.name) ? nm->u.ident.name : "";
        }
        default:
            return "";
    }
}

static char buf[1024];
char *node_str(Node *node)
{
    buf[0] = 0;
    char *p = buf;
    if (!node)
        return buf;
    p += sprintf(p, "%s: %5s %s sc:%s fts:%s: t:%016llx ",
        nodestr(node->kind),
        node->kind == ND_ARRAY_DECL ? "array" :
        (node->kind == ND_DECLARATION && node->u.declaration.is_func_defn) ? "fdef " :
        (node->kind == ND_IDENT ? node->u.ident.is_function :
         node->kind == ND_UNARYOP ? node->u.unaryop.is_function :
         node->kind == ND_MEMBER ? node->u.member.is_function : false) ? "func " : "     ",
        node_val_str(node),
        node->st ? scope_str(node->st->scope) : "(nil)",
        node->type ? fulltype_str(node->type) : "",
        (unsigned long long)node->type);
    if (node->kind == ND_DECLARATION)
    {
        p += sprintf(p, "sclass:%s ",
            sc_str(node->u.declaration.sclass));
        for (Node *d = node->u.declaration.decls; d; d = d->next)
            if (d->kind == ND_DECLARATOR && d->symbol)
                p += sprintf(p, "%s | ",
                    fulltype_str(d->symbol->type));
    }
    if (node->kind == ND_DECLARATOR || node->kind == ND_DIRECT_DECL)
    {
        p += sprintf(p, "%s %d* %s ",
            node_val_str(node),
            node->u.declarator.pointer_level,
            node->kind == ND_ARRAY_DECL ? "array" : "");
    }
    return buf;
}

static void call_fn(Node *child, void (*fn)(Node *, void *), void *ctx)
{
    if (child) fn(child, ctx);
}

void for_each_child(Node *node, void (*fn)(Node *child, void *ctx), void *ctx)
{
    if (!node) return;
    switch (node->kind)
    {
    // Cat A: expression nodes — dispatch through u.*
    case ND_BINOP:
    case ND_ASSIGN:
        call_fn(node->u.binop.lhs, fn, ctx);
        call_fn(node->u.binop.rhs, fn, ctx);
        return;
    case ND_UNARYOP:
        call_fn(node->u.unaryop.operand, fn, ctx);
        for (Node *a = node->u.unaryop.args; a; a = a->next)
            fn(a, ctx);
        return;
    case ND_CAST:
        call_fn(node->u.cast.type_decl, fn, ctx);
        call_fn(node->u.cast.expr, fn, ctx);
        return;
    case ND_TERNARY:
        call_fn(node->u.ternary.cond, fn, ctx);
        call_fn(node->u.ternary.then_, fn, ctx);
        call_fn(node->u.ternary.else_, fn, ctx);
        return;
    case ND_COMPOUND_ASSIGN:
        call_fn(node->u.compound_assign.lhs, fn, ctx);
        call_fn(node->u.compound_assign.rhs, fn, ctx);
        return;
    case ND_MEMBER:
        call_fn(node->u.member.base, fn, ctx);
        for (Node *a = node->u.member.args; a; a = a->next)
            fn(a, ctx);
        return;
    case ND_VA_START:
        call_fn(node->u.vastart.ap, fn, ctx);
        call_fn(node->u.vastart.last, fn, ctx);
        return;
    case ND_VA_ARG:
        call_fn(node->u.vaarg.ap, fn, ctx);
        return;
    case ND_VA_END:
        call_fn(node->u.vaend.ap, fn, ctx);
        return;

    // Cat B: statement nodes — dispatch through u.*
    case ND_IFSTMT:
        call_fn(node->u.ifstmt.cond, fn, ctx);
        call_fn(node->u.ifstmt.then_, fn, ctx);
        call_fn(node->u.ifstmt.else_, fn, ctx);
        return;
    case ND_WHILESTMT:
        call_fn(node->u.whilestmt.cond, fn, ctx);
        call_fn(node->u.whilestmt.body, fn, ctx);
        return;
    case ND_FORSTMT:
        call_fn(node->u.forstmt.init, fn, ctx);
        call_fn(node->u.forstmt.cond, fn, ctx);
        call_fn(node->u.forstmt.inc, fn, ctx);
        call_fn(node->u.forstmt.body, fn, ctx);
        return;
    case ND_DOWHILESTMT:
        call_fn(node->u.dowhile.body, fn, ctx);
        call_fn(node->u.dowhile.cond, fn, ctx);
        return;
    case ND_SWITCHSTMT:
        call_fn(node->u.switchstmt.selector, fn, ctx);
        call_fn(node->u.switchstmt.body, fn, ctx);
        return;
    case ND_RETURNSTMT:
        call_fn(node->u.returnstmt.expr, fn, ctx);
        return;
    case ND_EXPRSTMT:
        call_fn(node->u.exprstmt.expr, fn, ctx);
        return;
    case ND_LABELSTMT:
        call_fn(node->u.labelstmt.stmt, fn, ctx);
        return;

    // Leaf nodes — no children to visit
    case ND_IDENT:
        for (Node *a = node->u.ident.args; a; a = a->next)
            fn(a, ctx);
        return;
    case ND_LITERAL:
    case ND_GOTOSTMT:
    case ND_BREAKSTMT:
    case ND_CONTINUESTMT:
    case ND_CASESTMT:
    case ND_DEFAULTSTMT:
    case ND_EMPTY:
        return;

    // ND_DECLARATOR: migrated to u.declarator
    case ND_DECLARATOR:
        call_fn(node->u.declarator.direct_decl, fn, ctx);
        call_fn(node->u.declarator.init, fn, ctx);
        return;
    // ND_ARRAY_DECL: migrated to u.array_decl
    case ND_ARRAY_DECL:
        call_fn(node->u.array_decl.size, fn, ctx);
        return;
    // ND_FUNC_DECL: migrated to u.func_decl
    case ND_FUNC_DECL:
        call_fn(node->u.func_decl.params, fn, ctx);
        return;

    // ND_TYPE_NAME: migrated to u.type_name (ND_TYPE_NAME is never created currently)
    case ND_TYPE_NAME:
        call_fn(node->u.type_name.decl, fn, ctx);
        return;
    // ND_STMT: migrated to u.stmt_wrap
    case ND_STMT:
        call_fn(node->u.stmt_wrap.body, fn, ctx);
        return;

    // ND_PROGRAM: migrated to u.program
    case ND_PROGRAM:
    {
        for (Node *c = node->u.program.decls; c; c = c->next)
            fn(c, ctx);
        return;
    }
    // ND_COMPSTMT: migrated to u.compstmt
    case ND_COMPSTMT:
    {
        for (Node *c = node->u.compstmt.stmts; c; c = c->next)
            fn(c, ctx);
        return;
    }
    // ND_PTYPE_LIST: migrated to u.ptype_list
    case ND_PTYPE_LIST:
    {
        for (Node *c = node->u.ptype_list.params; c; c = c->next)
            fn(c, ctx);
        return;
    }
    // ND_INITLIST: migrated to u.initlist
    case ND_INITLIST:
    {
        for (Node *c = node->u.initlist.items; c; c = c->next)
            fn(c, ctx);
        return;
    }
    // ND_DECLARATION: migrated to u.declaration
    case ND_DECLARATION:
        call_fn(node->u.declaration.spec, fn, ctx);
        for (Node *c = node->u.declaration.decls; c; c = c->next)
            fn(c, ctx);
        call_fn(node->u.declaration.func_body, fn, ctx);
        return;
    // ND_DIRECT_DECL: migrated to u.direct_decl
    case ND_DIRECT_DECL:
        call_fn(node->u.direct_decl.name, fn, ctx);
        for (Node *c = node->u.direct_decl.suffixes; c; c = c->next)
            fn(c, ctx);
        return;
    // ND_STRUCT: migrated to u.struct_spec
    case ND_STRUCT:
        call_fn(node->u.struct_spec.tag, fn, ctx);
        for (Node *c = node->u.struct_spec.members; c; c = c->next)
            fn(c, ctx);
        return;

    default:
        error("for_each_child: unhandled node kind %d (%s)\n", node->kind, nodestr(node->kind));
    }
}

static void print_tree_visitor(Node *child, void *ctx)
{
    int depth = *(int *)ctx;
    print_tree(child, depth);
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
    int next_depth = depth + 1;
    for_each_child(node, print_tree_visitor, &next_depth);
}


const char *get_decl_ident(Node *node)
{
    // ND_DECLARATOR: name is in u.declarator.direct_decl
    if (node->kind == ND_DECLARATOR)
    {
        if (node->u.declarator.direct_decl)
            return get_decl_ident(node->u.declarator.direct_decl);
        return 0;
    }
    // ND_DIRECT_DECL: name is in u.direct_decl.name
    if (node->kind == ND_DIRECT_DECL)
    {
        Node *name = node->u.direct_decl.name;
        if (!name) return 0;
        if (name->kind == ND_IDENT)
            return name->u.ident.name;
        return get_decl_ident(name);
    }
    return 0;
}



// Get pointer to the Nth u.* child slot for node kinds that use tagged union fields.
// Returns NULL if the kind/child combo doesn't map to a u.* field.
static Node **get_u_child_slot(Node *n, int child)
{
    switch (n->kind)
    {
    case ND_BINOP: case ND_ASSIGN:
        if (child == 0) return &n->u.binop.lhs;
        if (child == 1) return &n->u.binop.rhs;
        break;
    case ND_UNARYOP:
        if (child == 0) return &n->u.unaryop.operand;
        break;
    case ND_CAST:
        if (child == 0) return &n->u.cast.type_decl;
        if (child == 1) return &n->u.cast.expr;
        break;
    case ND_TERNARY:
        if (child == 0) return &n->u.ternary.cond;
        if (child == 1) return &n->u.ternary.then_;
        if (child == 2) return &n->u.ternary.else_;
        break;
    case ND_COMPOUND_ASSIGN:
        if (child == 0) return &n->u.compound_assign.lhs;
        if (child == 1) return &n->u.compound_assign.rhs;
        break;
    case ND_RETURNSTMT:
        if (child == 0) return &n->u.returnstmt.expr;
        break;
    case ND_IFSTMT:
        if (child == 0) return &n->u.ifstmt.cond;
        break;
    case ND_WHILESTMT:
        if (child == 0) return &n->u.whilestmt.cond;
        break;
    case ND_FORSTMT:
        if (child == 0) return &n->u.forstmt.init;
        if (child == 1) return &n->u.forstmt.cond;
        if (child == 2) return &n->u.forstmt.inc;
        if (child == 3) return &n->u.forstmt.body;
        break;
    case ND_DOWHILESTMT:
        if (child == 0) return &n->u.dowhile.body;
        if (child == 1) return &n->u.dowhile.cond;
        break;
    case ND_EXPRSTMT:
        if (child == 0) return &n->u.exprstmt.expr;
        break;
    case ND_VA_START:
        if (child == 0) return &n->u.vastart.ap;
        if (child == 1) return &n->u.vastart.last;
        break;
    case ND_VA_ARG:
        if (child == 0) return &n->u.vaarg.ap;
        break;
    case ND_VA_END:
        if (child == 0) return &n->u.vaend.ap;
        break;
    case ND_DECLARATOR:
        if (child == 0) return &n->u.declarator.direct_decl;
        if (child == 1) return &n->u.declarator.init;
        break;
    case ND_ARRAY_DECL:
        if (child == 0) return &n->u.array_decl.size;
        break;
    case ND_FUNC_DECL:
        if (child == 0) return &n->u.func_decl.params;
        break;
    case ND_TYPE_NAME:
        if (child == 0) return &n->u.type_name.decl;
        break;
    case ND_STMT:
        if (child == 0) return &n->u.stmt_wrap.body;
        break;
    default:
        error("get_u_child_slot: unhandled node kind %d (%s)", n->kind, nodestr(n->kind));
        return NULL;
    }
    return NULL;
}

void insert_cast(Node *n, int child, Type *t)
{
    DBG_PRINT("%s %s\n", __func__, fulltype_str(t));
    Node *c = new_node(ND_CAST, 0, true);
    Node *placeholder = new_node(ND_DECLARATION, 0, false);

    Node **slot = get_u_child_slot(n, child);
    Node *original = *slot;

    c->u.cast.type_decl = placeholder;
    c->u.cast.expr = original;
    c->type = t;
    *slot = c;
}
// Insert ND_CAST nodes for implicit arithmetic conversions (C89 §3.2.1.5 usual arithmetic conversions).
static void insert_binop_coercions(Node *n)
{
    // Promotions use the *original* lhs/rhs types for checks; insert_cast
    // replaces n->u.binop.lhs/rhs in place.
    Node *lhs = n->u.binop.lhs;
    Node *rhs = n->u.binop.rhs;
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

    // Usual arithmetic conversions — rank hierarchy (C89 §3.2.1.5)
    if (istype_ulong(lhs->type) && !istype_ulong(rhs->type))            insert_cast(n, 1, t_ulong);
    else if (!istype_ulong(lhs->type) && istype_ulong(rhs->type))       insert_cast(n, 0, t_ulong);
    else if (istype_long(lhs->type) && istype_uint(rhs->type))          insert_cast(n, 1, t_long);
    else if (istype_uint(lhs->type) && istype_long(rhs->type))          insert_cast(n, 0, t_long);
    else if (istype_long(lhs->type) && !istype_long(rhs->type))         insert_cast(n, 1, t_long);
    else if (!istype_long(lhs->type) && istype_long(rhs->type))         insert_cast(n, 0, t_long);
    else if (istype_uint(lhs->type) && !istype_uint(rhs->type))         insert_cast(n, 1, t_uint);
    else if (!istype_uint(lhs->type) && istype_uint(rhs->type))         insert_cast(n, 0, t_uint);
}

// Insert ND_CAST nodes for implicit integer promotions in a unary expression (C89 §3.2.1.1).
static void insert_unary_coercions(Node *n)
{
    Node *lhs = n->u.unaryop.operand;
    DBG_PRINT("%s %016llx\n", __func__, (unsigned long long)lhs->type);

    // Dereference, address-of, and increment/decrement: no promotion needed.
    if (n->op_kind == TK_STAR || n->op_kind == TK_AMPERSAND ||
        n->op_kind == TK_INC  || n->op_kind == TK_DEC ||
        n->op_kind == TK_POST_INC || n->op_kind == TK_POST_DEC)
        return;

    // For +, -, ~, !: promote narrow integer types.
    if (istype_char(lhs->type))    insert_cast(n, 0, t_int);
    if (istype_uchar(lhs->type))   insert_cast(n, 0, t_int);
    if (istype_short(lhs->type))   insert_cast(n, 0, t_int);
    if (istype_ushort(lhs->type))  insert_cast(n, 0, t_uint);
    if (istype_enum(lhs->type))    insert_cast(n, 0, t_int);
}

bool is_unscaled_ptr(Node *n)
{
    return n->type && (n->type->base == TB_POINTER || n->type->base == TB_ARRAY)
        && !(n->kind == ND_UNARYOP && n->u.unaryop.is_array_deref);
}

// --- Shared post-order traversal helper ---

typedef void (*NodeVisitor)(Node *);

static void post_order_walk(Node *n, NodeVisitor fn);
static void post_order_step(Node *child, void *ctx)
{
    post_order_walk(child, (NodeVisitor)ctx);
}
static void post_order_walk(Node *n, NodeVisitor fn)
{
    if (!n) return;
    for_each_child(n, post_order_step, fn);
    fn(n);
}

// --- Pass 1: resolve_symbols ---
// Sets n->type for every ND_IDENT with is_expr == true.

static void resolve_symbols_step(Node *n)
{
    if (n->kind == ND_IDENT && n->is_expr)
        n->type = find_symbol(n, n->u.ident.name, NS_IDENT)->type;
}

void resolve_symbols(Node *root)
{
    post_order_walk(root, resolve_symbols_step);
}

// --- Pass 2: derive_types ---
// Sets n->type from children's already-set types. No AST mutations.

// Pure helper: compute the result type for a binary expression after arithmetic
// promotions and conversions, without inserting any cast nodes.
static Type *binop_result_type(Node *n)
{
    Type *l = n->u.binop.lhs->type, *r = n->u.binop.rhs->type;
    // If either operand type is unknown (t_void), defer — can't determine result type.
    // Internal stride literals (created by the parser, not typed) have t_void.
    if (l == t_void || r == t_void) return t_void;
    // Pointer/array arithmetic: pointer + int → pointer; preserve the pointer type.
    // This matches check_operands returning lhs->type when lhs is a pointer/array.
    if (l->base == TB_POINTER || l->base == TB_ARRAY) return l;
    if (r->base == TB_POINTER || r->base == TB_ARRAY) return r;
    if (istype_float(l) || istype_float(r))
        return t_float;
    // Apply integer promotions conceptually
    if (istype_char(l) || istype_uchar(l) || istype_short(l) || istype_enum(l))  l = t_int;
    if (istype_char(r) || istype_uchar(r) || istype_short(r) || istype_enum(r))  r = t_int;
    if (istype_ushort(l)) l = t_uint;
    if (istype_ushort(r)) r = t_uint;
    // Usual arithmetic conversions
    if (istype_ulong(l) || istype_ulong(r))                       return t_ulong;
    if ((istype_long(l) && istype_uint(r)) ||
        (istype_uint(l) && istype_long(r)))                       return t_long;
    if (istype_long(l) || istype_long(r))                         return t_long;
    if (istype_uint(l) || istype_uint(r))                         return t_uint;
    return t_int;
}

// Pure helper: compute the result type for a unary expression after promotions,
// without inserting any cast nodes.
static Type *unary_result_type(Node *n)
{
    Type *t = n->u.unaryop.operand->type;
    if (n->op_kind == TK_STAR)       return elem_type(t);
    if (n->op_kind == TK_AMPERSAND)  return get_pointer_type(t);
    if (n->op_kind == TK_INC  || n->op_kind == TK_DEC ||
        n->op_kind == TK_POST_INC || n->op_kind == TK_POST_DEC)
        return t;
    // Promotions for +, -, ~, !
    if (istype_char(t) || istype_uchar(t) || istype_short(t) || istype_enum(t)) t = t_int;
    else if (istype_ushort(t)) t = t_uint;
    if (n->op_kind == TK_BANG) return t_int;
    return t;
}

static void derive_types_step(Node *n)
{
    DBG_PRINT("derive_types: %s\n", node_str(n));
    if (n->kind == ND_MEMBER)
    {
        if (!n->u.member.base || !n->u.member.field_name)
            error("Malformed struct reference\n");
        Node *lhs = n->u.member.base;
        char *field_name = n->u.member.field_name;
        // For ->, dereference the pointer to get the struct type
        Type *struct_type = lhs->type;
        if (n->op_kind == TK_ARROW)
        {
            if (!istype_ptr(struct_type))
                error("'->' requires pointer type\n");
            struct_type = struct_type->u.ptr.pointee;
        }
        DBG_PRINT("%s looking in lhs type:%016llx for field %s\n", __func__, (unsigned long long)struct_type, field_name);
        Type *base = 0;
        n->u.member.offset = find_offset(struct_type, field_name, &base);
        if (n->u.member.offset < 0)
            error("Can't find member %s in struct\n", field_name);
        DBG_PRINT("%s found member %s with offset %d basetype %016llx\n", __func__,
            field_name, n->u.member.offset, (unsigned long long)base);
        n->type = base;
        // If this is a call through a function-pointer member, resolve to return type
        if (n->u.member.is_function && istype_ptr(n->type)
            && istype_function(n->type->u.ptr.pointee))
            n->type = n->type->u.ptr.pointee->u.fn.ret;
    }
    if (n->is_expr)
    {
        if (n->kind == ND_TERNARY)
            n->type = n->u.ternary.then_->type;
        if (n->kind == ND_BINOP)
        {
            if (n->op_kind == TK_COMMA)
                n->type = n->u.binop.rhs->type;
            else
            {
                n->type = binop_result_type(n);
                // Comparison and logical operators always yield int, regardless of operand types
                if (n->op_kind == TK_LT   || n->op_kind == TK_LE ||
                    n->op_kind == TK_GT   || n->op_kind == TK_GE ||
                    n->op_kind == TK_EQ   || n->op_kind == TK_NE ||
                    n->op_kind == TK_LOGAND || n->op_kind == TK_LOGOR)
                    n->type = t_int;
            }
        }
        if (n->kind == ND_UNARYOP)
        {
            Type *t = unary_result_type(n);
            if (n->type == t_void)  // preserve non-void types already set by parser (e.g. array subscripts)
                n->type = t;
        }
    }
    if (n->kind == ND_COMPOUND_ASSIGN)
        n->type = n->u.compound_assign.lhs->type;
    if (n->kind == ND_RETURNSTMT && n->u.returnstmt.expr)
    {
        n->type = parser_ctx.current_function->type->u.fn.ret;
        DBG_PRINT("%s found return stmt with expr, type of func:%s:\n", __func__, fulltype_str(n->type));
    }
    DBG_PRINT("derive_types after: %s\n", node_str(n));
}

void derive_types(Node *root)
{
    post_order_walk(root, derive_types_step);
}

// --- Pass 3: insert_coercions ---
// Mutates the AST with insert_cast / insert_scale. No type derivation.

static void insert_coercions_step(Node *n)
{
    if (n->is_expr && n->kind == ND_BINOP && n->op_kind != TK_COMMA)
    {
        insert_binop_coercions(n);
        // Scale pointer arithmetic: if one side is a pointer and the other an integer,
        // scale the integer by the size of the pointed-to element.
        Node *lhs = n->u.binop.lhs;
        Node *rhs = n->u.binop.rhs;
        if (is_unscaled_ptr(lhs) && istype_intlike(rhs->type))
            insert_scale(n, 1, elem_type(lhs->type)->size);
        else if (is_unscaled_ptr(rhs) && istype_intlike(lhs->type))
            insert_scale(n, 0, elem_type(rhs->type)->size);
    }
    if (n->is_expr && n->kind == ND_UNARYOP)
        insert_unary_coercions(n);
    if (n->kind == ND_COMPOUND_ASSIGN)
    {
        // If RHS type differs from LHS type, cast RHS so codegen uses consistent ops.
        Node *lhs = n->u.compound_assign.lhs;
        Node *rhs = n->u.compound_assign.rhs;
        Type *lhs_type = lhs->type;
        if (rhs->type != lhs_type)
            insert_cast(n, 1, lhs_type);
    }
    if (n->kind == ND_RETURNSTMT && n->u.returnstmt.expr)
    {
        Node *expr = n->u.returnstmt.expr;
        if (n->type != expr->type)
            insert_cast(n, 0, n->type);
    }
}

void insert_coercions(Node *root)
{
    post_order_walk(root, insert_coercions_step);
}

