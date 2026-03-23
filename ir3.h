#ifndef IR3_H
#define IR3_H

/* ir3.h — Generic 3-address SSA IR for the CPU4 RISC backend.
 *
 * This is a fresh-vreg SSA representation produced by braun_ssa() from the
 * stack-based IRInst list.  Unlike the old depth-indexed scheme, every
 * expression result gets a unique virtual register ID so the linear-scan
 * allocator can assign physical registers without fragility.
 *
 * Virtual register encoding (in IR3Inst rd / rs1 / rs2):
 *   IR3_VREG_NONE  (-1)  — no destination / unused operand
 *   IR3_VREG_BP    (-2)  — bp-relative addressing (LOAD/STORE only)
 *   IR3_VREG_ACCUM (100) — the accumulator; always mapped to physical r0
 *   101, 102, …          — fresh scratch vregs allocated by ir3_new_vreg()
 *   0 – 7                — physical registers (only after linscan_regalloc())
 */

#include "smallcc.h"
#include "ssa.h"

#define IR3_VREG_NONE  (-1)
#define IR3_VREG_BP    (-2)
#define IR3_VREG_BASE   100
#define IR3_VREG_ACCUM  100   /* accumulator → physical r0 */
#define IR3_VREG_IS_VIRT(v)  ((v) >= IR3_VREG_BASE)

/* Basic-block bounds (needed before IR3Inst for phi_ops array size) */
#define BB_MAX_PREDS 16
#define BB_MAX_SUCCS  2

typedef enum {
    IR3_CONST,    /* rd = imm  (integer const)  or  rd = &sym (symbolic) */
    IR3_LOAD,     /* rd = mem[rs1 + imm], size bytes;  rs1=-2 → bp-relative */
    IR3_STORE,    /* mem[rd  + imm] = rs1,  size bytes; rd=-2 → bp-relative  */
    IR3_LEA,      /* rd = bp + imm  (non-promoted local address, no load)    */
    IR3_ALU,      /* rd = rs1  alu_op  rs2                                   */
    IR3_ALU1,     /* rd = alu_op(rs1)   (sxb / sxw / itof / ftoi)           */
    IR3_MOV,      /* rd = rs1                                                */
    IR3_CALL,     /* jl sym  (direct call; result in r0 = IR3_VREG_ACCUM)   */
    IR3_CALLR,    /* jlr     (indirect call through r0)                      */
    IR3_RET,      /* ret                                                     */
    IR3_J,        /* unconditional jump; imm = label id                     */
    IR3_JZ,       /* jump if r0 == 0; imm = label id                        */
    IR3_JNZ,      /* jump if r0 != 0; imm = label id                        */
    IR3_ENTER,    /* enter N                                                 */
    IR3_ADJ,      /* sp += imm                                               */
    IR3_PHI,      /* rd = phi(phi_ops[0..n_phi_ops-1]); eliminated before linscan */
    /* Data-section / label pass-throughs (no virtual regs involved): */
    IR3_SYMLABEL, /* named label (function entry or data label)             */
    IR3_LABEL,    /* numeric label                                           */
    IR3_WORD,
    IR3_BYTE,
    IR3_ALIGN,
    IR3_PUTCHAR,
    IR3_COMMENT,
} IR3Op;

typedef struct IR3Inst {
    IR3Op        op;
    IROp         alu_op;   /* IR3_ALU / IR3_ALU1: original IR op              */
    int          rd;       /* dest vreg (IR3_VREG_NONE = no dest)             */
    int          rs1;      /* source vreg 1                                   */
    int          rs2;      /* source vreg 2                                   */
    int          imm;      /* immediate / byte offset from bp / label id      */
    int          size;     /* 1 / 2 / 4 for LOAD / STORE                      */
    const char  *sym;      /* symbolic name (label, call target)              */
    int          line;     /* source line for annotation                      */
    int          phi_ops[BB_MAX_PREDS]; /* phi source vregs; only for IR3_PHI */
    int          n_phi_ops;             /* number of valid phi_ops entries     */
    struct IR3Inst *next;
} IR3Inst;

/* ----------------------------------------------------------------
 * Basic block — used by build_cfg() and braun_ssa().
 * ---------------------------------------------------------------- */
typedef struct BB {
    int       id;
    int       label_id;          /* IR_LABEL operand of the first instruction, or -1 */
    int       preds[BB_MAX_PREDS];
    int       n_preds;
    int       succs[BB_MAX_SUCCS];
    int       n_succs;
    IRInst   *ir_first;   /* first stack-IR instruction in this block (IR_BB_START) */
    IRInst   *ir_last;    /* last  stack-IR instruction in this block */
    bool      sealed;     /* all predecessors of this block are known */
} BB;

/* Sentinel sym value for IR_LEA instructions emitted for promotable local
 * variables.  braun_ssa() uses pointer identity (p->sym == ir_promote_sentinel)
 * to distinguish promotable LEAs from structural ones (sym == NULL) and from
 * address-taken / aggregate LEAs (sym = variable name string). */
extern const char ir_promote_sentinel[];

/* ----------------------------------------------------------------
 * API
 * ---------------------------------------------------------------- */

/* Global fresh-vreg counter (reset per TU by ir3_reset()). */
int  ir3_new_vreg(void);
void ir3_reset(void);

/* Build CFG from a stack-IR list.  Returns NULL and sets *n_blocks=0
 * for this implementation (blocks are created internally by braun_ssa). */
BB  *build_cfg(IRInst *ir_head, int *n_blocks_out);
void free_cfg(BB *blocks, int n_blocks);

/* Lift stack IR to IR3 (fresh vregs, no phi nodes).
 * braun_ssa consumes the stack-IR list and produces an IR3Inst list.
 * blocks / n_blocks may be NULL / 0 — they are ignored in this version. */
IR3Inst *braun_ssa(BB *blocks, int n_blocks, IRInst *ir_head);

/* IR3-level optimizations: copy prop, constant prop/fold, DCE.
 * Runs after braun_ssa() and before linscan_regalloc().
 * opt_level: 0 = skip, >= 1 = run all passes. */
void ir3_optimize(IR3Inst *head, int opt_level);

/* Linear-scan register allocator: rewrites virtual vregs (>= IR3_VREG_BASE)
 * to physical registers 0-7 in place.  Also rewrites IR3_VREG_ACCUM → 0. */
void linscan_regalloc(IR3Inst *head);

/* Lower IR3Inst list to SSAInst for risc_backend_emit(). */
SSAInst *ir3_lower(IR3Inst *head);

/* Release IR3Inst list (all nodes heap-allocated). */
void free_ir3(IR3Inst *head);

#endif /* IR3_H */
