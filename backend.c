
#include "smallcc.h"


static FILE *asm_out = NULL;

void set_asm_out(FILE *f)
{
    asm_out = f ? f : stdout;
}


static const char *ir_opname[] = {
    [IR_ADD]     = "add",
    [IR_SUB]     = "sub",
    [IR_MUL]     = "mul",
    [IR_DIV]     = "div",
    [IR_MOD]     = "mod",
    [IR_LB]      = "lb",
    [IR_LW]      = "lw",
    [IR_LL]      = "ll",
    [IR_SB]      = "sb",
    [IR_SW]      = "sw",
    [IR_SL]      = "sl",
    [IR_RET]     = "ret",
    [IR_PUSH]    = "push",
    [IR_PUSHW]   = "pushw",
    [IR_POP]     = "pop",
    [IR_POPW]    = "popw",
    [IR_SXB]     = "sxb",
    [IR_SXW]     = "sxw",
    [IR_FADD]    = "fadd",
    [IR_FSUB]    = "fsub",
    [IR_FMUL]    = "fmul",
    [IR_FDIV]    = "fdiv",
    [IR_ITOF]    = "itof",
    [IR_FTOI]    = "ftoi",
    [IR_EQ]      = "eq",
    [IR_NE]      = "ne",
    [IR_LT]      = "lt",
    [IR_LE]      = "le",
    [IR_GT]      = "gt",
    [IR_GE]      = "ge",
    [IR_FLT]     = "flt",
    [IR_FLE]     = "fle",
    [IR_FGT]     = "fgt",
    [IR_FGE]     = "fge",
    [IR_SHL]     = "shl",
    [IR_SHR]     = "shr",
    [IR_AND]     = "and",
    [IR_OR]      = "or",
    [IR_XOR]     = "xor",
    [IR_JLI]     = "jli",
    [IR_PUTCHAR] = "putchar",
    [IR_ALIGN]   = "align",
};


void backend_emit_asm(IRInst *ir)
{
    if (!asm_out) asm_out = stdout;
    for (IRInst *p = ir; p; p = p->next)
    {
        switch (p->op)
        {
        case IR_LABEL:
            fprintf(asm_out, "_l%d:\n", p->operand);
            break;
        case IR_SYMLABEL:
            fprintf(asm_out, "%s:\n", p->sym);
            break;
        case IR_COMMENT:
            fprintf(asm_out, ";%s\n", p->sym);
            break;
        case IR_IMM:
            if (p->sym)
            {
                fprintf(asm_out, "    immw    %s\n", p->sym);
            }
            else
            {
                unsigned u = (unsigned)p->operand;
                fprintf(asm_out, "    immw    0x%04x\n", u & 0xffff);
                if (u > 0xffff)
                    fprintf(asm_out, "    immwh   0x%04x\n", (u >> 16) & 0xffff);
            }
            break;
        case IR_LEA:
            fprintf(asm_out, "    lea     %d\n", p->operand);
            break;
        case IR_J:
            fprintf(asm_out, "    j       _l%d\n", p->operand);
            break;
        case IR_JZ:
            fprintf(asm_out, "    jz      _l%d\n", p->operand);
            break;
        case IR_JNZ:
            fprintf(asm_out, "    jnz     _l%d\n", p->operand);
            break;
        case IR_JL:
            fprintf(asm_out, "    jl      %s\n", p->sym);
            break;
        case IR_ENTER:
            fprintf(asm_out, "    enter   %d\n", p->operand);
            break;
        case IR_ADJ:
            fprintf(asm_out, "    adj     %d\n", p->operand);
            break;
        case IR_WORD:
            if (p->sym)
                fprintf(asm_out, "    word    %s\n", p->sym);
            else
                fprintf(asm_out, "    word    0x%04x\n", p->operand & 0xffff);
            break;
        case IR_BYTE:
            fprintf(asm_out, "    byte    0x%02x\n", p->operand & 0xff);
            break;
        default:
            if (p->op < (int)(sizeof(ir_opname) / sizeof(ir_opname[0])) && ir_opname[p->op])
                fprintf(asm_out, "    %s\n", ir_opname[p->op]);
            break;
        }
    }
}
