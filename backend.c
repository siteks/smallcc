
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "smallcc.h"


FILE *asm_out = NULL;

void set_asm_out(FILE *f)
{
    asm_out = f ? f : stdout;
}

// ---------------------------------------------------------------------------
// Annotation mode (-ann)
// ---------------------------------------------------------------------------
int flag_annotate = 0;
int flag_no_newinsns = 0;

static char       *ann_src  = NULL;   // strdup of the preprocessed source
const  char      **ann_lines  = NULL; // ann_lines[i] points to start of line i+1
int                 ann_nlines = 0;

void set_ann_source(const char *src)
{
    ann_src    = NULL;
    ann_lines  = NULL;
    ann_nlines = 0;
    if (!src) return;

    ann_src = arena_strdup(src);

    // Count physical lines to bound the allocation
    int count = 1;
    for (const char *p = ann_src; *p; p++)
        if (*p == '\n') count++;

    // allocate generously; logical line numbers can be at most count
    ann_lines = arena_alloc((count + 2) * sizeof(char *));
    ann_nlines = count + 1;

    // Walk the preprocessed source tracking logical line numbers via
    // linemarker directives (same algorithm as the tokeniser).
    // ann_lines[N-1] is set to the text of logical source line N.
    int cur_line = 1;
    char *p = ann_src;
    while (*p) {
        char *line_start = p;
        // Linemarker: # <number> "filename" at start of line
        if (*p == '#') {
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (isdigit((unsigned char)*q)) {
                char *end;
                int lm_line = (int)strtol(q, &end, 10);
                cur_line = lm_line;
                // skip to end of marker line
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                // the next physical line is logical line cur_line
                continue;
            }
        }
        // Normal source line — record at logical cur_line
        if (cur_line >= 1 && cur_line <= ann_nlines)
            ann_lines[cur_line - 1] = line_start;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        cur_line++;
    }
}

// Emit the text of source line N (1-based) as an assembly comment.
static void emit_src_comment(int line)
{
    if (line < 1 || line > ann_nlines) return;
    const char *s = ann_lines[line - 1];
    const char *e = s;
    while (*e && *e != '\n') e++;
    // trim leading whitespace
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    if (s < e)
        fprintf(asm_out, "; %.*s\n", (int)(e - s), s);
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
    [IR_LTS]     = "lts",
    [IR_LES]     = "les",
    [IR_GTS]     = "gts",
    [IR_GES]     = "ges",
    [IR_DIVS]    = "divs",
    [IR_MODS]    = "mods",
    [IR_SHRS]    = "shrs",
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
    int prev_line = 0;
    int bb_idx    = 0;
    for (IRInst *p = ir; p; p = p->next)
    {
        // Emit source-line comment when the line changes (skip structural nodes).
        if (flag_annotate && p->line && p->line != prev_line &&
            p->op != IR_BB_START && p->op != IR_NOP && p->op != IR_COMMENT)
        {
            emit_src_comment(p->line);
            prev_line = p->line;
        }

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
        case IR_BB_START:
            if (flag_annotate)
                fprintf(asm_out, "; --- bb%d ---\n", bb_idx++);
            break;
        case IR_NOP:
            break;  // no assembly emitted
        case IR_IMM:
            if (p->sym)
            {
                fprintf(asm_out, "    immw    %s\n", p->sym);
            }
            else
            {
                unsigned u = (unsigned)p->operand;
                unsigned hi = (u >> 16) & 0xffff;
                fprintf(asm_out, "    immw    0x%04x\n", u & 0xffff);
                if (hi)
                    fprintf(asm_out, "    immwh   0x%04x\n", hi);
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
        case IR_ADJ: {
            int v = p->operand;
            if (v < -128 || v > 127)
                fprintf(asm_out, "    adjw    %d\n", v);
            else if (v != 0)
                fprintf(asm_out, "    adj     %d\n", v);
            break;
        }
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
