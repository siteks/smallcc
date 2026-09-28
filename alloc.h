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

#include "target.h"
#define IRC_K           (g_target->nregs)   /* physical registers (the target's) */

void compute_liveness(Function *f);
void irc_allocate(Function *f);

// OR extra caller-saved clobber bits into a function's recorded clobber
// mask. Called by emit_function AFTER emission: scratch registers borrowed
// at emit time (find_free_scratch / pick_scratch / const-base paths) are
// invisible to the IR-level record_function_clobbers walk, and callers
// compiled later must not keep values in them across a call.
void irc_add_clobbers(const char *name, regmask_t mask);

#endif // ALLOC_H
