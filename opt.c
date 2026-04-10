#include "opt.h"
#include "ssa.h"

/*
 * opt.c — Post-OOS SSA-level optimisation passes
 *
 * R2A  opt_fold_branches():    Replace IK_BR with a compile-time-constant
 *      condition by IK_JMP to the taken target.  Removes the dead predecessor
 *      edge from the dropped successor.
 *
 * R2B  opt_remove_dead_blocks(): Remove blocks with npreds == 0 that are not
 *      the entry block.  Strips their outgoing edges and iterates until stable.
 */

// ── R2A: Constant-condition branch folding ────────────────────────────────

void opt_fold_branches(Function *f) {
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b    = f->blocks[bi];
        Inst  *term = b->tail;
        if (!term || term->kind != IK_BR || term->nops < 1) continue;

        Value *cond = val_resolve(term->ops[0]);
        if (!cond || cond->kind != VAL_CONST) continue;

        Block *taken   = cond->iconst ? term->target  : term->target2;
        Block *dropped = cond->iconst ? term->target2 : term->target;

        // Remove b from dropped->preds
        for (int k = 0; k < dropped->npreds; k++) {
            if (dropped->preds[k] == b) {
                dropped->preds[k] = dropped->preds[--dropped->npreds];
                break;
            }
        }
        // Remove dropped from b->succs
        for (int k = 0; k < b->nsuccs; k++) {
            if (b->succs[k] == dropped) {
                b->succs[k] = b->succs[--b->nsuccs];
                break;
            }
        }

        // Rewrite terminator to unconditional jump
        term->kind    = IK_JMP;
        term->target  = taken;
        term->target2 = NULL;
        term->nops    = 0;  // drop the condition operand reference
    }
}

// ── R2B: Dead block elimination ───────────────────────────────────────────

void opt_remove_dead_blocks(Function *f) {
    int changed = 1;
    while (changed) {
        changed = 0;
        int new_n = 0;
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            if (bi == 0 || b->npreds > 0) {
                f->blocks[new_n++] = b;
            } else {
                // Dead block: strip outgoing edges from successors' pred lists
                for (int k = 0; k < b->nsuccs; k++) {
                    Block *succ = b->succs[k];
                    for (int j = 0; j < succ->npreds; j++) {
                        if (succ->preds[j] == b) {
                            succ->preds[j] = succ->preds[--succ->npreds];
                            break;
                        }
                    }
                }
                changed = 1;
            }
        }
        f->nblocks = new_n;
    }
}
