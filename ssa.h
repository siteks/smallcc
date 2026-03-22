#ifndef SSA_H
#define SSA_H

#include "smallcc.h"

/* SSAInst — the final IR consumed by risc_backend_emit() (CPU4 assembly).
 *
 * Produced by ir3_lower() from the post-regalloc IR3Inst list.
 * All registers are physical (0-7) at this point:
 *   r0           = accumulator / return value
 *   r1-r5        = scratch (caller-saved)
 *   r6-r7        = callee-saved
 *   -1           = no destination
 *   -2           = bp-relative addressing (LOAD/STORE only)
 */

typedef enum {
    SSA_MOVI,     /* rd = imm                          (immw [+ immwh]) */
    SSA_MOVSYM,   /* rd = &sym                         (immw sym) */
    SSA_LEA,      /* rd = bp + imm                     (lea rd, imm) */
    SSA_MOV,      /* rd = rs1                          (or rd, rs1, rs1) */
    SSA_LOAD,     /* rd = mem[base+disp], size=1/2/4
                   *   rs1 == -2: bp-relative (F2)
                   *   rs1 >= 0: register-relative (F3b), imm=0 */
    SSA_STORE,    /* mem[base+disp] = rs1, size=1/2/4
                   *   rd  == -2: bp-relative (F2), rs1=value_reg
                   *   rd  >= 0: register-relative (F3b) addr_reg, rs1=value_reg */
    SSA_ALU,      /* rd = rs1 alu_op rs2               (3-address ALU) */
    SSA_ALU1,     /* rd = op(rs1)                      (sxb/sxw/itof/ftoi) */
    SSA_ADJ,      /* sp += imm                         (adjw imm) */
    SSA_ENTER,    /* enter N                           */
    SSA_RET,      /* ret                               */
    SSA_CALL,     /* jl sym                            */
    SSA_CALLR,    /* jlr  (target in r0)               */
    SSA_J,        /* j _lN                             */
    SSA_JZ,       /* jz _lN  (r0 == 0)                 */
    SSA_JNZ,      /* jnz _lN (r0 != 0)                 */
    SSA_BRANCH,   /* beq/bne/blt/bgt/blts/bgts rs1,rs2,_lN (fused compare-branch) */
    SSA_ALU_IMM,  /* rd = rs1 + imm  (add-immediate; inc/dec/addi/addli) */
    SSA_LABEL,    /* _lN:                              */
    SSA_SYMLABEL, /* sym:                              */
    SSA_WORD,     /* data word                         */
    SSA_BYTE,     /* data byte                         */
    SSA_ALIGN,    /* align directive                   */
    SSA_PUTCHAR,  /* putchar (F0, r0 implicit)         */
    SSA_COMMENT,  /* asm comment                       */
} SSAOp;

typedef struct SSAInst {
    SSAOp       op;
    IROp        alu_op; /* SSA_ALU/SSA_ALU1: original IR opcode */
    int         rd;     /* 0-7 = physical, -1=none, -2=bp-rel */
    int         rs1;    /* same encoding as rd */
    int         rs2;    /* same encoding as rd */
    int         imm;    /* immediate, byte offset from bp, or label id */
    int         size;   /* 1/2/4 for SSA_LOAD/SSA_STORE */
    const char *sym;    /* symbolic name (labels, call targets) */
    int         line;   /* source line for annotation */
    struct SSAInst *next;
} SSAInst;

/* Pre-scan the SSA list to detect r6/r7 usage and deepest bp-relative offset
 * per function.  Must be called before risc_backend_emit(). */
void     rb_prescan(SSAInst *head);

/* Emit CPU4 assembly from SSA list. */
void     risc_backend_emit(SSAInst *head);

/* Free a SSAInst list (all nodes heap-allocated). */
void     free_ssa(SSAInst *head);

#endif /* SSA_H */
