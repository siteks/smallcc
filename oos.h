#ifndef OOS_H
#define OOS_H

#include "ssa.h"

/* Split all critical edges in f.  Run once after braun_function() so
   subsequent passes (OOS, optimisation) never encounter critical edges. */
void split_critical_edges(Function *f);

/* Out-of-SSA: insert parallel copies for phi nodes, then remove all phis */
void out_of_ssa(Function *f);

#endif // OOS_H
