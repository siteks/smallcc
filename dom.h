#ifndef DOM_H
#define DOM_H

#include "ssa.h"

/*
 * dom.h — Dominator tree + RPO + loop-depth computation
 *
 * Cooper, Harvey & Kennedy 2001 iterative algorithm.
 * After compute_dominators():
 *   Block.idom         — immediate dominator (NULL for entry)
 *   Block.dom_children — children in dominator tree
 *   Block.dom_pre      — RPO-based pre-order number (for dominates() check)
 *   Block.dom_post     — post-order number
 *   Block.rpo_index    — reverse post-order index (entry = 0)
 *   Block.loop_depth   — nesting depth (0 = not in loop)
 */

void compute_dominators(Function *f);

/* Returns true if block a dominates block b (a != b implies a strictly dominates b) */
static inline int dominates(Block *a, Block *b) {
    return a->dom_pre <= b->dom_pre && b->dom_post <= a->dom_post;
}

#endif // DOM_H
