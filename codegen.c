

#include "mycc.h"


extern Symbol_table *symbol_table;
char *scope_str(Scope sc);


int find_local_addr(Node *node, char *name)
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
    int offset = st->global_offset + s->offset;
    printf(";%s ident:%s at scope:%s got offset %d\n", __func__, name, scope_str(st->scope), offset);
    return offset;
}


static int labels;

//--------------------------------------------------------------------------------
// Pseudoinstructions
//
// Stack VM
//
// State:
//  pc
//  sp
//  bp
//  lr
//  r0  
//
//  imm     val     r0 = val
//  lb              r0 = *r0
//  lw              r0 = *r0
//  sb              **sp++ = r0
//  sw              **sp++ = r0
//
//  op              r0 = *sp++ op r0
//  push            *--sp = r0
//  pop             r0 = *sp++

//  jl      val     lr = pc; pc = val; 
//  enter   val     *--sp = lr; *--sp = bp; bp = sp; sp -= val;
//  return          sp = bp; bp = *sp++; pc = *sp++
//  lea     val     r0 = bp + val
//
//
// To store to local var
//  lea     offset
//  push
//  
//--------------------------------------------------------------------------------
int new_label()
{
    return labels++;
}
void gen_label(int label)
{
    printf("l%d:\n", label);
}

void gen_jz(int label)
{
    printf("    jz      l%d\n", label);
}
void gen_jnz(int label)
{
    printf("    jnz     l%d\n", label);
}
void gen_j(int label)
{
    printf("    j       l%d\n", label);
}
void gen_pushi(int val)
{
    printf("    immw    0x%04x\n", val);
    printf("    push\n");
}
void gen_imm(int val)
{
    printf("    immw    0x%04x\n", val);
}
void gen_push()
{
    printf("    push\n");
}
void gen_pop()
{
    printf("    pop\n");
}
void gen_adj(int offset)
{
    printf("    adj %d\n", offset);
}
void gen_add()
{
    printf("    add\n");
}
void gen_sub()
{
    printf("    sub\n");
}
void gen_mul()
{
    printf("    mul\n");
}

void gen_div()
{
    printf("    div\n");
}
void gen_eq()
{
    printf("    eq\n");
}
void gen_ne()
{
    printf("    ne\n");
}
void gen_lt()
{
    printf("    lt\n");
}
void gen_le()
{
    printf("    le\n");
}
void gen_gt()
{
    printf("    gt\n");
}
void gen_ge()
{
    printf("    ge\n");
}
void gen_lw()
{
    printf("    lw\n");
}
void gen_sw()
{
    printf("    sw\n");
}
void gen_assign()
{
}
void gen_varaddr(Node *node)
{
    if (node->kind != ND_IDENT)
        error("Expecting local var got %s", nodestr(node->kind));
    
    int offset = find_local_addr(node, node->val);

    printf(";%s %s offset %d\n", __func__, node->val, offset);

    // frame is pointing to sp, so offset needs incrementing
    int t = offset + 2;
    printf("    lea     %d\n", -t);
}
void gen_varaddr_from_ident(Node *node, char *name)
{
    int offset = find_local_addr(node, name);

    printf(";%s %s offset %d\n", __func__, name, offset);

    // frame is pointing to sp, so offset needs incrementing
    int t = offset + 2;
    printf("    lea     %d\n", -t);
}
void gen_preamble(int words)
{
    printf(";Reserve space for %d words\n", words);
    printf("    enter   %d\n", words);
}
void gen_postamble()
{
    printf(";%s\n", __func__);
}

//--------------------------------------------------------------------------------
// Everything below here uses pseudoinstructions
//--------------------------------------------------------------------------------

void gen_expr(Node *node);
void gen_addr(Node *node)
{
    printf(";%s %s %s\n", __func__, nodestr(node->kind), node->val);
    if (node->kind == ND_IDENT)
    {
        gen_varaddr(node);
    }
    else if (node->kind == ND_UNARYOP && !strcmp(node->val, "*"))
    {
        gen_expr(node->children[0]);
    }
    else
        error("Expecting lvalue\n");
}

void gen_expr(Node *node)
{
    printf(";%s %s %s\n", __func__, nodestr(node->kind), node->val);
    if (node->kind == ND_BINOP)
    {
        gen_expr(node->children[0]);
        gen_push();
        gen_expr(node->children[1]);
        if (!strcmp(node->val, "+"))    {gen_add(); return;}
        if (!strcmp(node->val, "-"))    {gen_sub(); return;}
        if (!strcmp(node->val, "*"))    {gen_mul(); return;}
        if (!strcmp(node->val, "/"))    {gen_div(); return;}
        if (!strcmp(node->val, "<"))    {gen_lt(); return;}
        if (!strcmp(node->val, "<="))   {gen_le(); return;}
        if (!strcmp(node->val, ">"))    {gen_gt(); return;}
        if (!strcmp(node->val, ">="))   {gen_ge(); return;}
        if (!strcmp(node->val, "=="))   {gen_eq(); return;}
        if (!strcmp(node->val, "!="))   {gen_ne(); return;}
        error("%s not handled in codegen", node->val);
    }
    else if (node->kind == ND_LITERAL)
    {
        gen_imm((int)strtol(node->val, 0, 0));
    }
    else if (node->kind == ND_IDENT)
    {
        gen_varaddr(node);
        gen_lw();
    }
    else if (node->kind == ND_UNARYOP)
    {
        if (!strcmp(node->val, "+"))    
        {
            gen_expr(node->children[0]);
        }      
        else if (!strcmp(node->val, "-"))    
        {
            gen_imm(0);
            gen_push();
            gen_expr(node->children[0]);
            gen_sub();
        }
        else if (!strcmp(node->val, "*"))
        {
            gen_expr(node->children[0]);
            gen_lw();
        }
    }
    else if (node->kind == ND_ASSIGN)
    {
        gen_addr(node->children[0]);
        gen_push();
        gen_expr(node->children[1]);
        gen_sw();
    }
}
void gen_return()
{
    printf("    ret\n");
}
void gen_returnstmt(Node *node)
{
    printf(";%s\n", __func__);
    if (node->child_count)
    {
        // Must have an expression
        gen_expr(node->children[0]);
    }
    gen_return();

}
void gen_ifstmt(Node *node)
{
    printf(";%s\n", __func__);
    // Structure is expr, stmt, [stmt]
    gen_expr(node->children[0]);
    int label = new_label();
    gen_jz(label);
    gen_stmt(node->children[1]);
    gen_label(label);
    if (node->child_count == 3)
    {
        printf(";else clause\n");
        gen_stmt(node->children[2]);
    }
    
}
void gen_whilestmt(Node *node)
{
    printf(";%s\n", __func__);
    // structure is expr, stmt
    int lloop   = labels++;
    int lbreak  = labels++;
    gen_label(lloop);
    gen_expr(node->children[0]);
    gen_jz(lbreak);
    gen_stmt(node->children[1]);
    gen_j(lloop);
    gen_label(lbreak);
}
void gen_exprstmt(Node *node)
{
    // printf(";%s\n", __func__);
    gen_expr(node->children[0]);
}
void gen_decl(Node *node)
{
    printf(";%s %s\n", __func__, node->val);
    // The symbol table has all the details needed for declarations.
    // Reserve space for local variables.

    // Iterate over declarators, generating code for initialisations
    for(int i = 0; i < node->child_count; i++)
    {
        Node *n = node->children[i];
        if (n->kind == ND_DECLARATOR && n->child_count == 2)
        {
            // Get ident from symbol table
            if (!n->symbol)
                error("Missing symbol!\n");
            gen_varaddr_from_ident(n, n->symbol->name);
            gen_push();
            gen_expr(n->children[1]);
            gen_sw();
        }
    }


    // TODO init_declarator
    // if (node->child_count == 2)
    // {
    //     // This is a declaration with assignment
    //     Gen_lvaraddr(node->children[0]);
    //     gen_push();
    //     gen_expr(node->children[1]);
    //     gen_sw();
    // }
}
void gen_compstmt(Node *node)
{
    printf(";%s\n", __func__);
    // Make space on stack for this scope's locals
    gen_adj(-node->symtable->size);
    for(int i = 0; i < node->child_count; i++)
    {
        Node *n = node->children[i];
        if (n->kind == ND_DECLARATION)
            gen_decl(n);
        else
            gen_stmt(n);
    }
    // Release the space back
    gen_adj(node->symtable->size);
}
void gen_stmt(Node *node)
{
    printf(";%s\n", __func__);
    switch(node->kind)
    {
    case ND_EXPRSTMT:   gen_exprstmt(node); return;
    case ND_COMPSTMT:   gen_compstmt(node); return;
    case ND_IFSTMT:     gen_ifstmt(node); return;
    case ND_WHILESTMT:  gen_whilestmt(node); return;
    case ND_RETURNSTMT: gen_returnstmt(node); return;
    case ND_STMT:       gen_stmt(node->children[0]); return;
    default:;
    }
}
void gen_param_list(Node *node)
{
    gen_preamble(node->child_count);
}
void gen_function(Node *node)
{
    printf(";%s\n", __func__);
    printf("%s:\n", node->val);
    gen_param_list(node->children[0]);
    gen_stmt(node->children[1]);
    gen_return();
}
void gen_code(Node *node)
{
    printf(";%s\n", __func__);
    
    switch (node->kind) 
    {
    case ND_PROGRAM:
        for(int i = 0; i < node->child_count; i++)
            gen_code(node->children[i]);
        return;
    case ND_FUNCTION:
        gen_function(node);
        return;
    default:;
    }
}

