

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
void gen_li(int reg, int val)
{
    printf(";%s\n", __func__);
    printf("    li      r%d 0x%02x\n", reg, val & 0xff);
    if (val & 0xff00) 
    {
        printf("    lih     r%d 0x%02x\n", reg, val >> 8);
    }
}
void gen_pushi(int val)
{
    printf(";%s %d\n", __func__, val);
    gen_li(0, val);
    printf("    adjm2\n");
    printf("    stw     r0 r6(0)\n");
}
void gen_pushr(int r)
{
    printf(";%s r%d\n", __func__, r);
    printf("    adjm2\n");
    printf("    stw     r%d r6(0)\n", r);
}
void gen_pop(int r)
{
    printf(";%s r%d\n", __func__, r);
    printf("    ldw     r%d r6(0)\n", r);
    printf("    adj2\n");
}

void gen_add()
{
    printf(";%s\n", __func__);
    gen_pop(1);
    gen_pop(0);
    printf("    add     r0 r0 r1\n");
    gen_pushr(0);
}
void gen_sub()
{
    printf(";%s\n", __func__);
    gen_pop(1);
    gen_pop(0);
    printf("    sub     r0 r0 r1\n");
    gen_pushr(0);
}
void gen_mul()
{
    printf(";%s\n", __func__);
    gen_pop(1);
    gen_pop(0);
    printf("    li      r2 0\n");
    printf("    li      r3 1\n");
    printf("l3: bz      r0 l1\n");
    printf("    bz      r1 l1\n");
    printf("    and     r4 r1 r3\n");
    printf("    bz      r4 l2\n");
    printf("    add     r2 r2 r0\n");
    printf("l2: slsl    r0\n");
    printf("    slsr    r1\n");
    printf("    bz      r7 l3\n");
    printf("l1:");
    gen_pushr(2);
}

void gen_div()
{
    printf(";%s\n", __func__);
}
void gen_eq()
{
    printf(";%s\n", __func__);
    gen_pop(0);
    gen_pop(1);
    printf("    xor     r0 r0 r1\n");
    printf("    inv     r0\n");
    gen_pushr(0);
}
void gen_ne()
{
    printf(";%s\n", __func__);
    gen_pop(0);
    gen_pop(1);
    printf("    xor     r0 r0 r1\n");
    gen_pushr(0);
}
void gen_lt()
{
    printf(";%s\n", __func__);
    gen_pop(1);
    gen_pop(0);
    printf("    slt      r0 r0 r1\n");
    gen_pushr(0);
}
void gen_le()
{
    printf(";%s\n", __func__);
    gen_pop(0);
    gen_pop(1);
    printf("    slt     r0 r0 r1\n");
    printf("    inv     r0\n");
    gen_pushr(0);
}
void gen_gt()
{
    printf(";%s\n", __func__);
    gen_pop(0);
    gen_pop(1);
    printf("    slt      r0 r0 r1\n");
    gen_pushr(0);
}
void gen_ge()
{
    printf(";%s\n", __func__);
    gen_pop(1);
    gen_pop(0);
    printf("    slt     r0 r0 r1\n");
    printf("    inv     r0\n");
    gen_pushr(0);
}
void gen_assign()
{
    printf(";%s\n", __func__);
    // if the variable has not yet been assigned, there needs
    // to be space allocated om the stack to hold it

    gen_pop(0);
    gen_pop(1);
    printf("    stw     r0 r1(0)\n");
}
void gen_readlocal()
{
    printf(";%s\n", __func__);
    gen_pop(0);
    printf("    ldw     r0 r0(0)\n");
    gen_pushr(0);
}
void gen_writelocal()
{
    printf(";%s\n", __func__);
    gen_pop(0);
    gen_pop(1);
    printf("    stw     r0 r1(0)\n");
    gen_pushr(0);
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
    int t = (offset + 1) * 2;
    gen_li(0, t);
    printf("    sub     r0 r5 r0\n");
    gen_pushr(0);
}
void gen_expr(Node *node)
{
    printf(";%s %s %s\n", __func__, nodestr(node->kind), node->val);
    if (node->kind == ND_BINOP)
    {
        gen_expr(node->lhs);
        gen_expr(node->rhs);
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
        gen_pushi((int)strtol(node->val, 0, 0));
    }
    else if (node->kind == ND_IDENT)
    {
        gen_lvaraddr(node);
        gen_readlocal();
    }
    else if (node->kind == ND_ASSIGN)
    {
        gen_lvaraddr(node->lhs);
        gen_expr(node->rhs);
        gen_assign();
    }
}
void gen_return()
{
    printf(";%s\n", __func__);

    printf(";get return vale in r0, dont do full pop since r6 overwritten\n");
    printf("    ldw     r0 r6(0)\n");
    printf(";sp adjust before return\n");
    printf("    add     r6 r5 r7\n");
    gen_pop(5);
    printf("    jl      r5 0\n");
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
    gen_pop(0);
    int label = labels++;
    printf("    bz r0 l%d\n", label);
    gen_stmt(node->children[1]);
    printf("l%d:\n", label);
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
    int lskip   = labels++;
    printf("l%d:\n", lloop);
    gen_expr(node->children[0]);
    gen_pop(0);
    printf("    bnz r0 l%d\n", lskip);
    printf("    li r4  l(l%d)\n", lbreak);
    printf("    lih r4 h(l%d)\n", lbreak);
    printf("    jl r4 0\n");
    printf("l%d:\n", lskip);
    gen_stmt(node->children[1]);
    printf("    li r4  l(l%d)\n", lloop);
    printf("    lih r4 h(l%d)\n", lloop);
    printf("    jl r4 0\n");
    printf("l%d:\n", lbreak);
}
void gen_exprstmt(Node *node)
{
    printf(";%s\n", __func__);
    gen_expr(node->children[0]);
}
void gen_preamble(int localidx)
{
    // printf(";%s\n", __func__);
    printf(";Save return address\n");
    gen_pushr(5);
    printf(";Set frame\n");
    printf("    add     r5 r6 r7\n");
    printf(";Reserve space for %d locals\n", localidx);
    int t = localidx * 2;
    gen_li(0, t);
    printf("    sub     r6 r6 r0\n");
}
void gen_postamble()
{
    printf(";%s\n", __func__);
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
        gen_expr(node->children[1]);
        gen_assign();
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

