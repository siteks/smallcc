#ifndef OOS_H
#define OOS_H

#include "ssa.h"

/* Out-of-SSA: insert parallel copies for phi nodes, then remove all phis */
void out_of_ssa(Function *f);

#endif // OOS_H
