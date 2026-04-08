#ifndef ALLOC_H
#define ALLOC_H

#include "ssa.h"

/*
 * alloc.h — Liveness analysis + IRC register allocator
 *
 * K = 8 allocatable registers (r0-r7 on CPU4)
 * r0-r3: caller-saved
 * r4-r7: callee-saved
 *
 * After irc_allocate():
 *   Value.phys_reg — physical register (0-7), or -1 if spilled
 *   Value.spill_slot — frame offset for spills, or -1
 *
 * Spilled values get load/store instructions inserted.
 * Frame_size is updated for spill slots.
 */

#define IRC_K           8   /* total physical registers */
#define IRC_CALLER_REGS 4   /* r0-r3 are caller-saved */

void compute_liveness(Function *f);
void irc_allocate(Function *f);

#endif // ALLOC_H
