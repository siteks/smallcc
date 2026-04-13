#include <string.h>
#include "dom.h"
#include "smallcc.h"

// ============================================================
// RPO computation (DFS-based)
// ============================================================

static void dfs(Block *b, Block **rpo, int *rpo_idx, int nblocks) {
    if (b->rpo_index != -1) return;  // already visited or in-progress
    b->rpo_index = -2; // mark in-progress (avoid infinite recursion on back edges)

    for (int i = 0; i < b->nsuccs; i++)
        dfs(b->succs[i], rpo, rpo_idx, nblocks);

    // Post-order: assign from the end
    b->rpo_index = --(*rpo_idx);
    rpo[b->rpo_index] = b;
}

static int build_rpo(Function *f, Block **rpo) {
    // Initialize rpo_index to -1 (unvisited)
    for (int i = 0; i < f->nblocks; i++)
        f->blocks[i]->rpo_index = -1;

    int rpo_idx = f->nblocks;
    if (f->nblocks > 0)
        dfs(f->blocks[0], rpo, &rpo_idx, f->nblocks);

    // Compact: some blocks may be unreachable
    int n = f->nblocks - rpo_idx;
    if (rpo_idx > 0) {
        // Shift down
        memmove(rpo, rpo + rpo_idx, n * sizeof(Block *));
        // Re-number
        for (int i = 0; i < n; i++)
            rpo[i]->rpo_index = i;
    }
    return n;
}

// ============================================================
// Cooper-Harvey-Kennedy iterative dominator algorithm
// ============================================================

static Block *intersect(Block *b1, Block *b2) {
    while (b1 != b2) {
        while (b1->rpo_index > b2->rpo_index)
            b1 = b1->idom;
        while (b2->rpo_index > b1->rpo_index)
            b2 = b2->idom;
    }
    return b1;
}

static void compute_idom(Function *f __attribute__((unused)), Block **rpo, int nrpo) {
    // Initialize: entry block dominates itself
    rpo[0]->idom = rpo[0];

    int changed = 1;
    while (changed) {
        changed = 0;
        // Process in RPO order (skip entry block at index 0)
        for (int i = 1; i < nrpo; i++) {
            Block *b = rpo[i];
            Block *new_idom = NULL;

            // Find first processed predecessor
            for (int j = 0; j < b->npreds; j++) {
                Block *p = b->preds[j];
                if (p->idom != NULL) {
                    new_idom = p;
                    break;
                }
            }

            // Intersect with all other processed predecessors
            for (int j = 0; j < b->npreds; j++) {
                Block *p = b->preds[j];
                if (p->idom != NULL && p != new_idom)
                    new_idom = intersect(p, new_idom);
            }

            if (b->idom != new_idom) {
                b->idom = new_idom;
                changed = 1;
            }
        }
    }

    // Entry block's idom is NULL (it has no dominator)
    rpo[0]->idom = NULL;
}

// ============================================================
// Build dominator-tree children lists
// ============================================================

static void build_dom_children(Function *f, Block **rpo, int nrpo) {
    // Count children for each block
    for (int i = 0; i < f->nblocks; i++) {
        f->blocks[i]->ndom_children = 0;
        f->blocks[i]->dom_children = NULL;
    }

    for (int i = 1; i < nrpo; i++) {
        Block *b = rpo[i];
        if (b->idom) b->idom->ndom_children++;
    }

    for (int i = 0; i < nrpo; i++) {
        Block *b = rpo[i];
        if (b->ndom_children > 0)
            b->dom_children = arena_alloc(b->ndom_children * sizeof(Block *));
        b->ndom_children = 0; // reset for fill pass
    }

    for (int i = 1; i < nrpo; i++) {
        Block *b = rpo[i];
        if (b->idom) {
            Block *p = b->idom;
            p->dom_children[p->ndom_children++] = b;
        }
    }
}

// ============================================================
// DFS over dominator tree to assign dom_pre / dom_post
// ============================================================

static int g_pre_counter;
static int g_post_counter;

static void dom_dfs(Block *b) {
    b->dom_pre = g_pre_counter++;
    for (int i = 0; i < b->ndom_children; i++)
        dom_dfs(b->dom_children[i]);
    b->dom_post = g_post_counter++;
}

// ============================================================
// Loop depth via back-edge detection
// ============================================================

static void mark_loop_depth(Block **rpo, int nrpo) {
    // A back edge is one where the successor dominates the source (b -> h, h dom b).
    // For each back edge, find the natural loop (all blocks that can reach b without
    // going through h) and increment their loop_depth.  Uses a worklist-based
    // predecessor walk.

    for (int i = 0; i < nrpo; i++)
        rpo[i]->loop_depth = 0;

    // Worklist for natural loop discovery (reused across back-edges)
    Block **wl = malloc(nrpo * sizeof(Block *));
    int *in_loop = calloc(nrpo, sizeof(int));   // per-back-edge membership
    if (!wl || !in_loop) { free(wl); free(in_loop); return; }

    int gen = 0;  // generation counter to avoid clearing in_loop each iteration

    for (int i = 0; i < nrpo; i++) {
        Block *b = rpo[i];
        for (int j = 0; j < b->nsuccs; j++) {
            Block *h = b->succs[j];
            if (h->rpo_index <= b->rpo_index && dominates(h, b)) {
                // Found back edge b → h.  Discover natural loop body.
                gen++;
                int wl_top = 0;
                h->loop_depth++;
                in_loop[h->rpo_index] = gen;
                if (b != h && in_loop[b->rpo_index] != gen) {
                    in_loop[b->rpo_index] = gen;
                    b->loop_depth++;
                    wl[wl_top++] = b;
                }
                while (wl_top > 0) {
                    Block *cur = wl[--wl_top];
                    for (int p = 0; p < cur->npreds; p++) {
                        Block *pred = cur->preds[p];
                        if (pred->rpo_index < 0 || pred->rpo_index >= nrpo)
                            continue;  // unreachable block
                        if (in_loop[pred->rpo_index] != gen) {
                            in_loop[pred->rpo_index] = gen;
                            pred->loop_depth++;
                            wl[wl_top++] = pred;
                        }
                    }
                }
            }
        }
    }
    free(wl);
    free(in_loop);
}

// ============================================================
// Entry point
// ============================================================

void compute_dominators(Function *f) {
    if (f->nblocks == 0) return;

    Block **rpo = arena_alloc(f->nblocks * sizeof(Block *));
    int nrpo = build_rpo(f, rpo);

    // Clear idom for all blocks
    for (int i = 0; i < nrpo; i++)
        rpo[i]->idom = NULL;

    compute_idom(f, rpo, nrpo);
    build_dom_children(f, rpo, nrpo);

    g_pre_counter  = 0;
    g_post_counter = 0;
    dom_dfs(rpo[0]);

    mark_loop_depth(rpo, nrpo);
}
