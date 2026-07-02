#ifndef VERIFY_H
#define VERIFY_H

#include "ssa.h"

/*
 * verify.h — IR consistency verifier (debug facility)
 *
 * Enabled by the IR_VERIFY environment variable; the pipeline calls
 * verify_function after each pass group. On any violation it prints
 * every finding for the function (prefixed with the stage name) and
 * exits, so the FIRST failing stage names the pass that broke the IR.
 *
 * Checks (phase-dependent):
 *   - pred/succ symmetry, branch targets belong to the function
 *   - exactly one terminator per block; post-OOS it must be the tail
 *   - phi arity == npreds before OOS; no phis at all after OOS
 *   - operands of live instructions do not resolve to dead defs
 *   - stored use_count matches a fresh recount (the P5-fusion invariant)
 *   - post-IRC: every live def and use has phys_reg in 0..7
 */

typedef enum {
    VERIFY_PRE_OOS,   // true SSA: phis present; use_count approximate
    VERIFY_OOS,       // phis eliminated; use_count still approximate
                      // (Braun's counts are only trued up by the first
                      //  recount_uses, inside opt_copy_prop)
    VERIFY_POST_OOS,  // phis eliminated, virtual registers, exact use_count
    VERIFY_POST_IRC,  // physical registers assigned
} VerifyPhase;

// Returns quietly if the IR is consistent; prints and exits otherwise.
void verify_function(Function *f, const char *stage, VerifyPhase phase);

// True if IR_VERIFY is set (cached on first call).
int ir_verify_enabled(void);

#endif // VERIFY_H
