#include <string.h>
#include "ssa.h"
#include "smallcc.h"

/*
 * oos.c — Out-of-SSA via parallel-copy insertion (Boissinot et al. 2009)
 *
 * For each phi:  v = phi(v1@B1, v2@B2, ...)
 *   Insert a parallel copy  v <- vi  at the end of each predecessor Bi
 *   (before the terminator).
 *
 * After inserting copies:
 *   - Detect and break swap cycles in the copy set on each edge.
 *   - Remove all IK_PHI instructions.
 *   - Remove dead IK_COPY instructions (copy to self).
 */

// ============================================================
// Parallel copy: a set of (dst, src) pairs on one CFG edge
// ============================================================

typedef struct {
    Value *dst;
    Value *src;
} PCopy;

typedef struct {
    PCopy *copies;
    int    n;
    int    cap;
} PCopySet;

static void pcs_add(PCopySet *s, Value *dst, Value *src) {
    if (s->n >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        PCopy *nc_arr = arena_alloc(nc * sizeof(PCopy));
        memcpy(nc_arr, s->copies, s->n * sizeof(PCopy));
        s->copies = nc_arr;
        s->cap = nc;
    }
    s->copies[s->n].dst = dst;
    s->copies[s->n].src = src;
    s->n++;
}

// ============================================================
// Insert a COPY before the terminator of block b
// ============================================================

static void insert_copy_before_terminator(Block *b, Value *dst, Value *src) {
    if (dst == src) return; // trivial

    // dst == src by alias?
    Value *rdst = val_resolve(dst);
    Value *rsrc = val_resolve(src);
    if (rdst == rsrc) return;

    // new_inst's Function* param is unused (see ssa.c), so NULL is fine here
    Inst *copy = new_inst(NULL, b, IK_COPY, dst);
    inst_add_op(copy, src);

    // Insert before terminator
    Inst *term = b->tail;
    if (term) copy->line = term->line;
    if (!term) {
        inst_append(b, copy);
        return;
    }
    // Check if tail is a terminator
    if (term->kind == IK_BR || term->kind == IK_JMP || term->kind == IK_RET) {
        // Insert before term
        copy->prev = term->prev;
        copy->next = term;
        if (term->prev) term->prev->next = copy;
        else             b->head = copy;
        term->prev = copy;
        copy->block = b;
    } else {
        inst_append(b, copy);
    }
}

// ============================================================
// Sequentialize a parallel copy set, breaking swap cycles
// ============================================================
// We use the standard algorithm:
//   ready = {(d,s) : d not in {src of any copy}}
//   to_do = remaining
//   while to_do not empty:
//     while ready not empty:
//       take (d,s) from ready; emit d<-s; remove from to_do
//       if (s,?) is in to_do, add (s,?) to ready
//     if to_do not empty (cycle):
//       pick any (d,s) from to_do
//       emit tmp <- d (break cycle)
//       replace all src==d with src==tmp in to_do
//       add (d,s) to ready

static void sequentialize_copies(Block *b, PCopySet *s, Function *f) {
    int n = s->n;
    if (n == 0) return;

    // Resolve all values first
    for (int i = 0; i < n; i++) {
        s->copies[i].dst = val_resolve(s->copies[i].dst);
        s->copies[i].src = val_resolve(s->copies[i].src);
    }

    // Remove self-copies
    for (int i = 0; i < n; ) {
        if (s->copies[i].dst == s->copies[i].src) {
            s->copies[i] = s->copies[--n];
        } else {
            i++;
        }
    }
    s->n = n;
    if (n == 0) return;

    int *done = arena_alloc(n * sizeof(int));

    // For each copy, check if its dst appears as a src anywhere
    // ready[i] = 1 if copy i can be emitted now (dst not used as src)
    int *ready = arena_alloc(n * sizeof(int));

    for (int i = 0; i < n; i++) {
        int used_as_src = 0;
        for (int j = 0; j < n; j++) {
            if (!done[j] && s->copies[j].src == s->copies[i].dst) {
                used_as_src = 1;
                break;
            }
        }
        ready[i] = !used_as_src;
    }

    int emitted = 0;
    while (emitted < n) {
        // Emit all ready copies
        int progress = 1;
        while (progress) {
            progress = 0;
            for (int i = 0; i < n; i++) {
                if (done[i] || !ready[i]) continue;
                Value *dst = s->copies[i].dst;
                Value *src = s->copies[i].src;
                insert_copy_before_terminator(b, dst, src);
                done[i] = 1;
                emitted++;
                progress = 1;
                // Check if src was a dst waiting for us
                for (int j = 0; j < n; j++) {
                    if (done[j]) continue;
                    if (s->copies[j].dst == src) {
                        // src is now free to be overwritten → j might become ready
                        int still_used = 0;
                        for (int k = 0; k < n; k++) {
                            if (!done[k] && k != j && s->copies[k].src == s->copies[j].dst)
                                { still_used = 1; break; }
                        }
                        if (!still_used) ready[j] = 1;
                    }
                }
            }
        }

        if (emitted < n) {
            // There's a cycle. Break it with a fresh temp.
            // Find first undone copy.
            int idx = -1;
            for (int i = 0; i < n; i++) { if (!done[i]) { idx = i; break; } }
            if (idx < 0) break;

            // Create a fresh temp vreg
            Value *tmp = new_value(f, VAL_INST, s->copies[idx].dst->vtype);
            // Emit: tmp <- dst[idx]  (save the value about to be overwritten)
            insert_copy_before_terminator(b, tmp, s->copies[idx].dst);
            // Replace all src == dst[idx] with tmp
            for (int j = 0; j < n; j++) {
                if (!done[j] && s->copies[j].src == s->copies[idx].dst)
                    s->copies[j].src = tmp;
            }
            // Now idx is ready
            ready[idx] = 1;
        }
    }

}

// ============================================================
// Critical edge splitting
// A "critical edge" is one from a block with >1 successor to a block with >1 predecessor.
// We must split such edges before inserting phi copies, so the copies don't affect
// other outgoing edges from the same source block.
// ============================================================

// Split the edge pred→succ by inserting a new block between them.
// Returns the new intermediate block.
static Block *split_one_critical_edge(Function *f, Block *pred, Block *succ) {
    // Create a new block
    Block *mid = new_block(f);
    mid->sealed = 1;
    mid->filled  = 1;

    // mid jumps unconditionally to succ
    Inst *jmp   = arena_alloc(sizeof(Inst));
    jmp->kind   = IK_JMP;
    jmp->target = succ;
    jmp->block  = mid;
    inst_append(mid, jmp);

    // Update pred's terminator: replace references to succ with mid.
    // The phi for v47 (or any late-added incomplete phi) may be placed AFTER the
    // branch instruction via inst_append when the block was unsealed.  Walk back
    // from the tail to find the actual terminator (IK_BR / IK_JMP / IK_RET).
    Inst *term = pred->tail;
    while (term && term->kind != IK_BR && term->kind != IK_JMP && term->kind != IK_RET)
        term = term->prev;
    if (term) {
        if (term->target  == succ) term->target  = mid;
        if (term->target2 == succ) term->target2 = mid;
    }

    // Update pred's succs array
    for (int i = 0; i < pred->nsuccs; i++) {
        if (pred->succs[i] == succ) { pred->succs[i] = mid; break; }
    }

    // Update succ's preds array: replace pred with mid
    for (int i = 0; i < succ->npreds; i++) {
        if (succ->preds[i] == pred) { succ->preds[i] = mid; break; }
    }

    // Set mid's preds/succs
    block_add_pred(mid, pred);
    block_add_succ(mid, succ);

    return mid;
}

// ============================================================
// Main out-of-SSA pass
// ============================================================

void split_critical_edges(Function *f) {
    // Split critical edges so the IR never has an edge from a multi-successor
    // block to a multi-predecessor block.  Run once after braun_function() so
    // OOS and any future optimisation passes never encounter critical edges.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        if (b->npreds <= 1) continue;

        int has_phi = 0;
        for (Inst *inst = b->head; inst; inst = inst->next)
            if (inst->kind == IK_PHI) { has_phi = 1; break; }
        if (!has_phi) continue;

        for (int k = 0; k < b->npreds; k++) {
            Block *pred = b->preds[k];
            if (pred->nsuccs > 1)
                (void)split_one_critical_edge(f, pred, b);
        }
    }
}

void out_of_ssa(Function *f) {
    // Critical edges were already split by split_critical_edges() which runs
    // right after braun_function().  OOS can proceed without splitting.

    // For each block, collect parallel copies needed on each predecessor edge
    // For each phi in block b:
    //   For each operand phi->ops[k] from predecessor b->preds[k]:
    //     add copy (phi->dst, phi->ops[k]) on edge preds[k]->b

    // We process block by block.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];

        // Collect all phis in this block
        // Count phis first
        int nphi = 0;
        for (Inst *inst = b->head; inst; inst = inst->next)
            if (inst->kind == IK_PHI) nphi++;
        if (nphi == 0) continue;

        // For each predecessor, build a parallel copy set
        PCopySet *pred_copies = arena_alloc(b->npreds * sizeof(PCopySet));

        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->kind != IK_PHI) continue;
            // phi->dst gets value from each predecessor
            // phi->ops[k] corresponds to b->preds[k]
            for (int k = 0; k < b->npreds && k < inst->nops; k++) {
                pcs_add(&pred_copies[k], inst->dst, inst->ops[k]);
            }
        }

        // Sequentialize and insert copies for each predecessor
        for (int k = 0; k < b->npreds; k++)
            sequentialize_copies(b->preds[k], &pred_copies[k], f);
    }

    // Now remove all IK_PHI instructions
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        Inst *inst = b->head;
        while (inst) {
            Inst *next = inst->next;
            if (inst->kind == IK_PHI) {
                // Unlink from list
                if (inst->prev) inst->prev->next = inst->next;
                else            b->head = inst->next;
                if (inst->next) inst->next->prev = inst->prev;
                else            b->tail = inst->prev;
            }
            inst = next;
        }
    }

    // Remove self-copies (dst == src after resolution)
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        Inst *inst = b->head;
        while (inst) {
            Inst *next = inst->next;
            if (inst->kind == IK_COPY && inst->nops > 0) {
                Value *dst = val_resolve(inst->dst);
                Value *src = val_resolve(inst->ops[0]);
                if (dst == src) {
                    if (inst->prev) inst->prev->next = inst->next;
                    else            b->head = inst->next;
                    if (inst->next) inst->next->prev = inst->prev;
                    else            b->tail = inst->prev;
                }
            }
            inst = next;
        }
    }
}
