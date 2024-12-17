

#include "mycc.h"


static int labels;
extern Local *locals;

// Insert new local var at start of list
void new_local(Node *node)
{
    Local *l    = calloc(1, sizeof(Local));
    l->next     = locals;
    l->name     = node->val;
    l->offset   = node->offset;
    locals      = l;
}
// Return offset of local, or -1 if not found
int find_local_addr(char *name)
{
    for(Local *p = locals; p; p = p->next)
    {
        if (!strcmp(p->name, name))
            return p->offset;
    }
    return -1;
}
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
void gen_pop(int r)
{
    printf("    pop\n");
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
void gen_lvaraddr(Node *node)
{
    if (node->kind != ND_IDENT)
        error("Expecting local var got %s", nodestr(node->kind));
    
    int offset = find_local_addr(node->val);
    if (offset < 0)
        error("Could not find local var %s address!", node->val);

    printf(";%s %s offset %d\n", __func__, node->val, offset);

    // frame is pointing to sp, so offset needs incrementing
    int t = offset + 1;
    printf("    lea     %d\n", -t);
}
void gen_preamble(int localidx)
{
    printf(";Reserve space for %d locals\n", localidx);
    printf("    enter   %d\n", localidx);
}
void gen_postamble()
{
    printf(";%s\n", __func__);
}

//--------------------------------------------------------------------------------
// Everything below here uses pseudoinstructions
//--------------------------------------------------------------------------------




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
        gen_lvaraddr(node);
        gen_lw();
    }
    else if (node->kind == ND_UNARYOP)
    {
        if (!strcmp(node->val, "+"))    {gen_expr(node->children[0]); return;}      
        if (!strcmp(node->val, "-"))    
        {
            gen_imm(0);
            gen_push();
            gen_expr(node->children[0]);
            gen_sub();
        }
    }
    else if (node->kind == ND_ASSIGN)
    {
        gen_lvaraddr(node->children[0]);
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
    // Function delarations. Space has already been reserved
    // Add the declaration to the list of variables
    if (node->child_count == 2)
    {
        // This is a declaration with assignment
        gen_lvaraddr(node->children[0]);
        gen_push();
        gen_expr(node->children[1]);
        gen_sw();
    }
}
void gen_compstmt(Node *node)
{
    printf(";%s\n", __func__);
    for(int i = 0; i < node->child_count; i++)
    {
        gen_stmt(node->children[i]);
    }
}
void gen_stmt(Node *node)
{
    switch(node->kind)
    {
    case ND_EXPRSTMT:   gen_exprstmt(node); return;
    case ND_COMPSTMT:   gen_compstmt(node); return;
    case ND_IFSTMT:     gen_ifstmt(node); return;
    case ND_WHILESTMT:  gen_whilestmt(node); return;
    case ND_RETURNSTMT: gen_returnstmt(node); return;
    default:;
    }
}
void gen_function(Node *node)
{
    printf(";%s\n", __func__);
    printf("%s:\n", node->val);
    int localidx = 0;
    locals = NULL;
    int labels = 0;
    for(int i = 0; i < node->child_count; i++)
        if (node->children[i]->kind == ND_DECLSTMT)
        {
            node->children[i]->children[0]->offset = localidx++;
            new_local(node->children[i]->children[0]);
        }
    gen_preamble(localidx);
    for(int i = 0; i < node->child_count; i++)
        if (node->children[i]->kind == ND_DECLSTMT)
            gen_decl(node->children[i]);



    for(int i = 0; i < node->child_count; i++)
        if (node->children[i]->kind != ND_DECLSTMT)
        {
            Node *n = node->children[i];
            gen_stmt(n);
        }
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

