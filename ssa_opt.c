/* ssa_opt.c — SSA-level optimizations (operate on virtual registers,
 * before physical register allocation). */
#include "ssa.h"

/* ----------------------------------------------------------------
 * ssa_peephole — copy propagation pass (in-place)
 *
 * Eliminates trivial identity moves: MOV v, v.
 * Future: add constant propagation, dead-code elimination.
 * ---------------------------------------------------------------- */
void ssa_peephole(SSAInst *head)
{
    for (SSAInst *p = head; p; p = p->next)
    {
        /* Kill: mov rd, rs1 where rd == rs1 (identity move) */
        if (p->op == SSA_MOV && p->rd == p->rs1 && p->rd >= 0)
            p->op = (SSAOp)-1;  /* mark dead; risc_backend skips negative ops */
    }
}
