#ifndef OPT_H
#define OPT_H

#include "ssa.h"

// R2A: Replace IK_BR with a VAL_CONST condition by IK_JMP to the taken target.
void opt_fold_branches(Function *f);

// R2B: Remove blocks with no predecessors (except entry block).
void opt_remove_dead_blocks(Function *f);

// R2D: Collapse IK_COPY chains; rewrite operands to canonical values; recount use_count.
void opt_copy_prop(Function *f);

// R2E: Hash-based CSE; eliminates duplicate pure computations across dominating blocks.
void opt_cse(Function *f);

#endif
