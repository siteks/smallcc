

#include "mycc.h"


static int depth;


void gen_pushi(int val)
{
    printf("    li      r0 0x%02x\n", val & 0xff);
    if (val & 0xff00) 
    {
        printf("    lih     r0 0x%02x\n", val >> 8);
    }
    printf("    adjm2\n");
    printf("    stw     r0 r6(0)\n");
    depth++;
}
void gen_pushr(int r)
{
    printf("    adjm2\n");
    printf("    stw     r%d r6(0)\n", r);
    depth++;
}
void gen_pop(int r)
{
    printf("    ldw     r%d r6(0)\n", r);
    printf("    adj2\n");
    depth--;
}

void gen_add()
{
    gen_pop(1);
    gen_pop(0);
    printf("    add     r0 r0 r1\n");
    gen_pushr(0);
}
void gen_sub()
{
    gen_pop(1);
    gen_pop(0);
    printf("    sub     r0 r0 r1\n");
    gen_pushr(0);
}
void gen_mul()
{
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

}
void gen_eq()
{
    gen_pop(0);
    gen_pop(1);
    printf("    xor     r0 r0 r1\n");
    printf("    inv     r0\n");
    gen_pushr(0);
}
void gen_ne()
{
    gen_pop(0);
    gen_pop(1);
    printf("    xor     r0 r0 r1\n");
    gen_pushr(0);
}
void gen_lt()
{
    gen_pop(1);
    gen_pop(0);
    printf("    slt      r0 r0 r1\n");
    gen_pushr(0);
}
void gen_le()
{
    gen_pop(0);
    gen_pop(1);
    printf("    slt     r0 r0 r1\n");
    printf("    inv     r0\n");
    gen_pushr(0);
}
void gen_gt()
{
    gen_pop(0);
    gen_pop(1);
    printf("    slt      r0 r0 r1\n");
    gen_pushr(0);
}
void gen_ge()
{
    gen_pop(1);
    gen_pop(0);
    printf("    slt     r0 r0 r1\n");
    printf("    inv     r0\n");
    gen_pushr(0);
}
void gen_assign()
{
    // if the variable has not yet been assigned, there needs
    // to be space allocated om the stack to hold it

    gen_pop(1);
    gen_pop(0);
    printf("    stw     r0 r1(0)\n");
}
void gen_readlocal()
{
    gen_pop(0);
    printf("    ldw     r0 r0(0)\n");
    gen_pushr(0);
}
void gen_writelocal()
{
    gen_pop(0);
    gen_pop(1);
    printf("    stw     r0 r1(0)\n");
    gen_pushr(0);
}
void gen_lval(Node *node)
{
    if (node->kind != ND_LOCAL)
        error("Expecting lvalue");
    
    printf(";lval:%s at offset:%d\n", node->ident, node->offset);

    // frame is pointing to sp, so offset needs incrementing
    int t = (node->offset + 1) * 2;
    printf("    li      r0 0x%02x\n", t & 0xff);
    if (t & 0xff00)
    {
        printf("    lih     r0 0x%02x\n", t >> 8);
    }
    printf("    sub     r0 r5 r0\n");
    printf("    adjm2\n");
    printf("    stw     r0 r6(0)\n");
    depth++;
}
void gen_return()
{
    // There may be locals on the stack, adjust sp before return
    printf(";get return vale in r0, dont do full pop since r6 overwritten\n");
    printf("    ldw     r0 r6(0)\n");
    printf(";sp adjust before return\n");
    printf("    add     r6 r5 r7\n");
    printf("    ldw     r5 r6(0)\n");
    printf("    adj2\n");
    printf("    jl      r5 0\n");
}
void gen_preamble(int locals)
{
    depth = 0;
    printf(";Save return address\n");
    printf("    adjm2\n");
    printf("    stw     r5 r6(0)\n");
    printf(";Set frame\n");
    printf("    add     r5 r6 r7\n");
    printf(";Reserve space for %d locals\n", locals);
    int t = locals * 2;
    printf("    li      r0 0x%02x\n", t & 0xff);
    if (t & 0xff00)
        printf("    lih     r0 0x%02x\n", t >> 8);
    printf("     sub     r6 r6 r0\n");
}
void gen_postamble()
{
}
void gen_code(Node *node)
{
    
    switch (node->kind) 
    {
    case ND_NUM:
        printf(";num:%d\n", node->val);
        gen_pushi(node->val);
        return;
    case ND_LOCAL:
        gen_lval(node);
        gen_readlocal();
        return;
    case ND_ASSIGN:
        printf(";=\n");
        gen_lval(node->lhs);
        gen_code(node->rhs);
        gen_writelocal();
        return;
    case ND_RETURN:
        gen_code(node->lhs);
        gen_return();
        return;
    default:;
    }
    gen_code(node->lhs);
    gen_code(node->rhs);
    if (node->kind == ND_ADD)           gen_add();
    else if (node->kind == ND_SUB)      gen_sub();
    else if (node->kind == ND_MUL)      gen_mul();
    else if (node->kind == ND_DIV)      gen_div();
    else if (node->kind == ND_EQ)       gen_eq();
    else if (node->kind == ND_NE)       gen_ne();
    else if (node->kind == ND_LT)       gen_lt();
    else if (node->kind == ND_LE)       gen_le();
    else if (node->kind == ND_GT)       gen_gt();
    else if (node->kind == ND_GE)       gen_ge();
}

