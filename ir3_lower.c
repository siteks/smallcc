/* ir3_lower.c — Lower IR3Inst list to SSAInst for risc_backend_emit().
 *
 * After linscan_regalloc() all virtual registers in IR3Inst have been
 * replaced with physical registers 0-7.  This pass performs a near-1:1
 * translation to SSAInst so that the unchanged risc_backend_emit() can
 * emit CPU4 assembly.
 *
 * IR3Op → SSAOp mapping:
 *   IR3_CONST  (sym==NULL)  → SSA_MOVI
 *   IR3_CONST  (sym!=NULL)  → SSA_MOVSYM
 *   IR3_LOAD                → SSA_LOAD
 *   IR3_STORE               → SSA_STORE
 *   IR3_LEA                 → SSA_LEA
 *   IR3_ALU                 → SSA_ALU
 *   IR3_ALU1                → SSA_ALU1
 *   IR3_MOV                 → SSA_MOV
 *   IR3_CALL                → SSA_CALL
 *   IR3_CALLR               → SSA_CALLR
 *   IR3_RET                 → SSA_RET
 *   IR3_J                   → SSA_J
 *   IR3_JZ                  → SSA_JZ
 *   IR3_JNZ                 → SSA_JNZ
 *   IR3_ENTER               → SSA_ENTER
 *   IR3_ADJ                 → SSA_ADJ
 *   IR3_SYMLABEL            → SSA_SYMLABEL
 *   IR3_LABEL               → SSA_LABEL
 *   IR3_WORD                → SSA_WORD
 *   IR3_BYTE                → SSA_BYTE
 *   IR3_ALIGN               → SSA_ALIGN
 *   IR3_PUTCHAR             → SSA_PUTCHAR
 *   IR3_COMMENT             → SSA_COMMENT
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "ir3.h"

static SSAInst *new_ssa(SSAOp op)
{
    SSAInst *s = calloc(1, sizeof(SSAInst));
    if (!s) { fprintf(stderr, "ir3_lower: out of memory\n"); exit(1); }
    s->op  = op;
    s->rd  = -1;
    s->rs1 = -1;
    s->rs2 = -1;
    return s;
}

SSAInst *ir3_lower(IR3Inst *head)
{
    SSAInst  *ssa_head = NULL;
    SSAInst **ssa_tail = &ssa_head;

    for (IR3Inst *p = head; p; p = p->next) {
        SSAInst *s = NULL;

        switch (p->op) {
        case IR3_CONST:
            s = new_ssa(p->sym ? SSA_MOVSYM : SSA_MOVI);
            s->rd   = p->rd;
            s->imm  = p->imm;
            s->sym  = p->sym;
            break;

        case IR3_LOAD:
            s = new_ssa(SSA_LOAD);
            s->rd   = p->rd;
            s->rs1  = p->rs1;
            s->imm  = p->imm;
            s->size = p->size;
            s->sym  = p->sym;   /* variable name for -ann annotation */
            break;

        case IR3_STORE:
            s = new_ssa(SSA_STORE);
            s->rd   = p->rd;
            s->rs1  = p->rs1;
            s->imm  = p->imm;
            s->size = p->size;
            break;

        case IR3_LEA:
            s = new_ssa(SSA_LEA);
            s->rd   = p->rd;
            s->imm  = p->imm;
            break;

        case IR3_ALU:
            s = new_ssa(SSA_ALU);
            s->alu_op = p->alu_op;
            s->rd     = p->rd;
            s->rs1    = p->rs1;
            s->rs2    = p->rs2;
            break;

        case IR3_ALU1:
            s = new_ssa(SSA_ALU1);
            s->alu_op = p->alu_op;
            s->rd     = p->rd;
            s->rs1    = p->rs1;
            break;

        case IR3_MOV:
            if (p->rd == IR3_VREG_NONE) continue;  /* dead phi residual: skip */
            s = new_ssa(SSA_MOV);
            s->rd  = p->rd;
            s->rs1 = p->rs1;
            break;

        case IR3_CALL:
            s = new_ssa(SSA_CALL);
            s->sym = p->sym;
            break;

        case IR3_CALLR:
            s = new_ssa(SSA_CALLR);
            break;

        case IR3_RET:
            s = new_ssa(SSA_RET);
            break;

        case IR3_J:
            s = new_ssa(SSA_J);
            s->imm = p->imm;
            break;

        case IR3_JZ:
            s = new_ssa(SSA_JZ);
            s->imm = p->imm;
            break;

        case IR3_JNZ:
            s = new_ssa(SSA_JNZ);
            s->imm = p->imm;
            break;

        case IR3_ENTER:
            s = new_ssa(SSA_ENTER);
            s->imm = p->imm;
            break;

        case IR3_ADJ:
            if (p->imm == 0) continue;   /* skip zero adjustments */
            s = new_ssa(SSA_ADJ);
            s->imm = p->imm;
            break;

        case IR3_SYMLABEL:
            s = new_ssa(SSA_SYMLABEL);
            s->sym = p->sym;
            break;

        case IR3_LABEL:
            s = new_ssa(SSA_LABEL);
            s->imm = p->imm;
            break;

        case IR3_WORD:
            s = new_ssa(SSA_WORD);
            s->imm = p->imm;
            s->sym = p->sym;
            break;

        case IR3_BYTE:
            s = new_ssa(SSA_BYTE);
            s->imm = p->imm;
            break;

        case IR3_ALIGN:
            s = new_ssa(SSA_ALIGN);
            break;

        case IR3_PUTCHAR:
            s = new_ssa(SSA_PUTCHAR);
            break;

        case IR3_COMMENT:
            s = new_ssa(SSA_COMMENT);
            s->sym = p->sym;
            break;

        case IR3_PHI:
            /* Phi nodes must be eliminated by phi deconstruction before lowering */
            if (p->rd != IR3_VREG_NONE) {
                fprintf(stderr, "ir3_lower: unexpected live IR3_PHI node (rd=%d, phi deconstruction incomplete)\n", p->rd);
                abort();
            }
            /* Dead phi (rd == IR3_VREG_NONE): skip silently */
            continue;

        default:
            fprintf(stderr, "ir3_lower: unknown IR3Op %d\n", (int)p->op);
            continue;
        }

        if (s) {
            s->line   = p->line;
            *ssa_tail = s;
            ssa_tail  = &s->next;
        }
    }

    return ssa_head;
}

/* ----------------------------------------------------------------
 * free_ssa — release all nodes in a SSAInst list
 * ---------------------------------------------------------------- */
void free_ssa(SSAInst *head)
{
    while (head) {
        SSAInst *next = head->next;
        free(head);
        head = next;
    }
}
