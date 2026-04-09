#ifndef IRSIM_H
#define IRSIM_H

/*
 * irsim.h — IR interpreter for post-OOS and post-IRC Function* forms.
 *
 * The interpreter executes Function* IR in-memory without generating CPU4
 * assembly.  It supports both post-OOS (phys_reg == -1) and post-IRC
 * (phys_reg set, spill slots populated) IR: the same run_function
 * implementation handles both modes because values are indexed by id not
 * phys_reg, and spill loads/stores inserted by IRC are executed naturally.
 *
 * Activated via:
 *   ./smallcc -arch cpu4 -runoos t.c   # interpret post-OOS IR
 *   ./smallcc -arch cpu4 -runirc t.c   # interpret post-IRC IR
 * Both print "r0:XXXXXXXX" to stdout on completion (matches sim_c format).
 */

#include <stdint.h>
#include "ssa.h"
#include "sx.h"

typedef struct IrSim IrSim;

/* Lifecycle */
IrSim   *irsim_new(void);
void     irsim_free(IrSim *sim);

/* Registration — call for every Function* and every TU's sx_prog */
void     irsim_add_function(IrSim *sim, Function *f);
void     irsim_populate_globals(IrSim *sim, Sx *program);

/* Execution — finds "main" and calls it, returns r0 value */
uint32_t irsim_call_main(IrSim *sim);

#endif /* IRSIM_H */
