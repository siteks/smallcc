/* regalloc.c — Physical register allocation for the CPU4 RISC backend.
 *
 * Input:  SSAInst list with virtual regs (rd/rs1/rs2 >= VREG_START=8).
 * Output: Same list, virtual regs replaced by physical regs (0-7) in-place.
 *         Special values -1 and -2 are passed through unchanged.
 *
 * Initial strategy: trivial depth-based mapping.
 *   physical(vreg) = vreg - VREG_START
 * Produces identical assembly to the old implicit allocation.
 * Sethi-Ullman depth <= 3 guarantees no interference conflicts; r0-r3 suffice.
 *
 * To improve: replace alloc_reg() with a linear-scan or graph-coloring
 * implementation without touching ssa.c or risc_backend.c. */
#include "ssa.h"

static int alloc_reg(int vreg)
{
    if (vreg < VREG_START) return vreg;   /* -2, -1, or already physical */
    return vreg - VREG_START;
}

void regalloc(SSAInst *head)
{
    for (SSAInst *p = head; p; p = p->next) {
        if (p->rd  >= VREG_START) p->rd  = alloc_reg(p->rd);
        if (p->rs1 >= VREG_START) p->rs1 = alloc_reg(p->rs1);
        if (p->rs2 >= VREG_START) p->rs2 = alloc_reg(p->rs2);
    }
}
