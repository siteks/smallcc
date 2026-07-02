#ifndef LEGALIZE_H
#define LEGALIZE_H

#include "ssa.h"

/*
 * legalize_function — ABI materialisation and IR normalisation pass.
 *
 * Runs between out_of_ssa() and irc_allocate() so IRC sees every
 * register constraint as an explicit pre-coloured virtual value.
 *
 * Transforms applied:
 *   A. IK_PARAM  → set phys_reg = param_idx  (r1/r2/r3 ABI registers)
 *   B. IK_CALL / IK_ICALL args → insert pre-coloured IK_COPY instructions
 *      (replaces emit_reg_arg_copies that used to live in braun.c)
 *   C. IK_ZEXT / IK_TRUNC with mask < 4 bytes
 *      → IK_CONST(mask) + IK_AND  (removes PUSH_SCRATCH from emit.c)
 */
void legalize_function(Function *f);

#endif // LEGALIZE_H
