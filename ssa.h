#ifndef SSA_H
#define SSA_H

#include "smallcc.h"

/* Virtual register namespace:
 *   VREG_START+0 = v8  → accumulator          (physical r0 after regalloc)
 *   VREG_START+1 = v9  → depth-1 scratch      (physical r1)
 *   VREG_START+2 = v10 → depth-2 scratch      (physical r2)
 *   ...
 *   -2 = bp-relative addressing (not a register)
 *   -1 = no destination
 *   0..7 = physical registers (only after regalloc())
 */
#define VREG_START 8   /* virtual regs >= VREG_START; physical regs 0-7 */

/* 3-address SSA-like IR for the CPU4 RISC backend.
 * Produced by lift_to_ssa() from the stack-based IR.
 * lift_to_ssa() emits virtual registers (>= VREG_START).
 * regalloc() maps them to physical registers (0-7).
 *   r0           = accumulator / return value
 *   r1-r3        = expression scratch (caller-saved; depth 1-3)
 *   r4-r5        = extra scratch (overflow; shouldn't be needed with SU labelling)
 *   r6-r7        = callee-saved (not yet used by the allocator)
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
    SSA_BRANCH,   /* beq/bne/blt/bgt/blts/bgts rs1,rs2,_lN (fused compare-branch, B2) */
    SSA_ALU_IMM,  /* rd = rs1 + imm  (add-immediate, D4; inc/dec/addi/addli) */
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
    int         rd;     /* VREG_START+ = virtual (pre-regalloc), 0-7 = physical (post), -1=none, -2=bp-rel */
    int         rs1;    /* same encoding as rd */
    int         rs2;    /* same encoding as rd */
    int         imm;    /* immediate, byte offset from bp, or label id */
    int         size;   /* 1/2/4 for SSA_LOAD/SSA_STORE */
    const char *sym;    /* symbolic name (labels, call targets) */
    int         line;   /* source line for annotation */
    struct SSAInst *next;
} SSAInst;

/* Convert stack IR to 3-address SSA with virtual register assignment. */
SSAInst *lift_to_ssa(IRInst *ir_head);

/* Simple copy-propagation pass over SSA list (in-place, on virtual regs). */
void     ssa_peephole(SSAInst *head);

/* Map virtual registers to physical registers (in-place). */
void     regalloc(SSAInst *head);

/* Emit CPU4 assembly from SSA list. */
void     risc_backend_emit(SSAInst *head);

/* Free a SSAInst list (all nodes heap-allocated). */
void     free_ssa(SSAInst *head);

#endif /* SSA_H */
