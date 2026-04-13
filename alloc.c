#include <string.h>
#include <stdio.h>
#include "alloc.h"
#include "dom.h"

// ============================================================
// Bitvector helpers (indexed by Value.id)
// ============================================================

static int bv_nwords(int nvalues) {
    return (nvalues + 31) / 32;
}

static void bv_set(uint32_t *bv, int id) {
    bv[id >> 5] |= (1u << (id & 31));
}

static void bv_clear(uint32_t *bv, int id) {
    bv[id >> 5] &= ~(1u << (id & 31));
}

static int bv_test(uint32_t *bv, int id) {
    return (bv[id >> 5] >> (id & 31)) & 1;
}

// bv_or: dst |= src; return 1 if dst changed
static int bv_or(uint32_t *dst, const uint32_t *src, int nwords) {
    int changed = 0;
    for (int i = 0; i < nwords; i++) {
        uint32_t old = dst[i];
        dst[i] |= src[i];
        if (dst[i] != old) changed = 1;
    }
    return changed;
}

// ============================================================
// Compute liveness (backward dataflow)
// ============================================================

void compute_liveness(Function *f) {
    int nv = f->nvalues;
    int nw = bv_nwords(nv);

    // Allocate bitvectors for each block
    for (int i = 0; i < f->nblocks; i++) {
        Block *b = f->blocks[i];
        b->nwords = nw;
        b->live_in  = arena_alloc(nw * sizeof(uint32_t));
        b->live_out = arena_alloc(nw * sizeof(uint32_t));
    }

    // Iterate to fixed point
    int changed = 1;
    while (changed) {
        changed = 0;
        // Process in reverse RPO (post-order) for fast convergence
        for (int bi = f->nblocks - 1; bi >= 0; bi--) {
            Block *b = f->blocks[bi];

            // Compute live_out = union of live_in of successors
            uint32_t *new_out = arena_alloc(nw * sizeof(uint32_t));
            for (int si = 0; si < b->nsuccs; si++) {
                Block *s = b->succs[si];
                // For phi operands: the operand from this predecessor is live out of b
                // but we handle this by including phi uses in the successor's live_in
                bv_or(new_out, s->live_in, nw);
            }

            // live_in = use(b) union (live_out - def(b))
            // We compute this by walking instructions backward
            uint32_t *live = arena_alloc(nw * sizeof(uint32_t));
            memcpy(live, new_out, nw * sizeof(uint32_t));

            // Walk instructions in reverse
            for (Inst *inst = b->tail; inst; inst = inst->prev) {
                // kill def
                if (inst->dst && inst->dst->id >= 0 && inst->dst->id < nv)
                    bv_clear(live, inst->dst->id);

                // gen uses
                for (int oi = 0; oi < inst->nops; oi++) {
                    Value *v = val_resolve(inst->ops[oi]);
                    if (v && v->kind == VAL_INST && v->id >= 0 && v->id < nv)
                        bv_set(live, v->id);
                }
            }

            // Also handle phi operands for successors:
            // The operand corresponding to this block is used in the successor's phi,
            // so it's live at the end of this block.
            for (int si = 0; si < b->nsuccs; si++) {
                Block *s = b->succs[si];
                // Find which predecessor index we are
                int pidx = -1;
                for (int pi = 0; pi < s->npreds; pi++) {
                    if (s->preds[pi] == b) { pidx = pi; break; }
                }
                if (pidx < 0) continue;
                for (Inst *inst = s->head; inst; inst = inst->next) {
                    if (inst->kind != IK_PHI) continue;
                    if (pidx < inst->nops) {
                        Value *v = val_resolve(inst->ops[pidx]);
                        if (v && v->kind == VAL_INST && v->id >= 0 && v->id < nv)
                            bv_set(new_out, v->id);
                    }
                }
            }

            // Update live_in and live_out
            if (memcmp(b->live_in, live, nw * sizeof(uint32_t)) != 0) {
                memcpy(b->live_in, live, nw * sizeof(uint32_t));
                changed = 1;
            }
            if (memcmp(b->live_out, new_out, nw * sizeof(uint32_t)) != 0) {
                memcpy(b->live_out, new_out, nw * sizeof(uint32_t));
                changed = 1;
            }

        }
    }
}

// ============================================================
// IRC register allocator
// ============================================================

// We use a simplified IRC that:
// 1. Builds interference graph
// 2. Iterates Simplify/SelectSpill until the graph is K-colorable
// 3. Assigns colors
// 4. Rewrites spills if needed

#define MAX_REGS IRC_K   // 8 physical registers

// Interference graph: adjacency sets using bit vectors
typedef struct {
    int        nv;           // number of virtual registers (values)
    uint32_t **adj;          // adj[v] = bitvector of interfering values
    int        nwords;       // words per bitvector
    int       *degree;       // degree[v] = count of interfering regs
    int       *color;        // assigned physical register (-1 = unassigned)
    int       *precolored;   // precolored[v] = forced register, or -1
    int       *spilled;      // spilled[v] = 1 if spilled
    int       *alias;        // alias[v] = canonical rep after coalescing (init: v)
} IGraph;

static IGraph *ig_new(int nv) {
    IGraph *g = arena_alloc(sizeof(IGraph));
    g->nv     = nv;
    g->nwords = bv_nwords(nv);
    g->adj    = arena_alloc(nv * sizeof(uint32_t *));
    for (int i = 0; i < nv; i++)
        g->adj[i] = arena_alloc(g->nwords * sizeof(uint32_t));
    g->degree     = arena_alloc(nv * sizeof(int));
    g->color      = arena_alloc(nv * sizeof(int));
    g->precolored = arena_alloc(nv * sizeof(int));
    g->spilled    = arena_alloc(nv * sizeof(int));
    g->alias      = arena_alloc(nv * sizeof(int));
    for (int i = 0; i < nv; i++) {
        g->color[i]      = -1;
        g->precolored[i] = -1;
        g->alias[i]      = i;
    }
    return g;
}

static void ig_free(IGraph *g) {
    (void)g; // arena-allocated; nothing to free
}

static void ig_add_edge(IGraph *g, int u, int v) {
    if (u == v) return;
    if (u >= g->nv || v >= g->nv) return;
    if (!bv_test(g->adj[u], v)) {
        bv_set(g->adj[u], v);
        bv_set(g->adj[v], u);
        // Don't count edges to/from precolored as degree
        if (g->precolored[u] < 0) g->degree[u]++;
        if (g->precolored[v] < 0) g->degree[v]++;
    }
}

// Canonical representative lookup with path compression
static int ig_find(IGraph *g, int v) {
    while (g->alias[v] != v) {
        g->alias[v] = g->alias[g->alias[v]];  // path compression (halving)
        v = g->alias[v];
    }
    return v;
}

// Merge node v into node u: redirect all of v's interference edges to u.
// After this, alias[v] = u and v is effectively removed from the graph.
static void ig_merge(IGraph *g, int u, int v) {
    for (int w = 0; w < g->nwords; w++) {
        uint32_t word = g->adj[v][w];
        while (word) {
            int bit = __builtin_ctz(word); word &= word - 1;
            int t = w * 32 + bit;
            if (t >= g->nv || t == u) continue;
            // Remove v-t edge (update t's degree)
            bv_clear(g->adj[t], v);
            if (g->precolored[t] < 0) g->degree[t]--;
            // Add u-t edge if not present
            ig_add_edge(g, u, t);
        }
    }
    memset(g->adj[v], 0, g->nwords * sizeof(uint32_t));
    g->degree[v] = 0;
    g->alias[v]  = u;
}

// George's conservative coalescing test: can we safely merge v into u?
// Safe if every neighbor t of v either already interferes with u, or has degree < K.
// Precondition: u and v are canonical (ig_find) and do not interfere.
static int can_coalesce_george(IGraph *g, int u, int v, int K) {
    for (int w = 0; w < g->nwords; w++) {
        uint32_t word = g->adj[v][w];
        while (word) {
            int bit = __builtin_ctz(word); word &= word - 1;
            int t = w * 32 + bit;
            if (t >= g->nv || t == u) continue;
            if (!bv_test(g->adj[u], t)) {
                // Same-color precolored pair: merging would create a self-edge.
                if (g->precolored[u] >= 0 && g->precolored[t] >= 0 &&
                    g->precolored[t] == g->precolored[u])
                    return 0;
                // Precolored nodes (including phantom register nodes) have infinite
                // effective degree — they can never be simplified away, so treating
                // them as degree >= K is always conservative and correct.
                if (g->precolored[t] >= 0)
                    return 0;
                if (g->degree[t] >= K)
                    return 0;
            }
        }
    }
    return 1;
}

// Coalesce IK_COPY instructions using George's conservative test.
// Runs to fixpoint; marks coalesced copies is_dead = 1.
// Call-site ABI constraints are encoded directly in the interference graph
// via phantom nodes for r0-r3 (added in build_interference_graph), so no
// side-channel is needed here.
static void coalesce_copies(IGraph *g, Function *f, int K) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->kind != IK_COPY || inst->is_dead || inst->nops < 1) continue;
                Value *vdst = val_resolve(inst->dst);
                Value *vsrc = val_resolve(inst->ops[0]);
                if (!vdst || !vsrc || vdst == vsrc) { inst->is_dead = 1; changed = 1; continue; }
                if (vdst->kind != VAL_INST || vsrc->kind != VAL_INST) continue;
                if (vdst->id < 0 || vsrc->id < 0 ||
                    vdst->id >= g->nv || vsrc->id >= g->nv) continue;

                int u = ig_find(g, vdst->id);
                int v = ig_find(g, vsrc->id);
                if (u == v) { inst->is_dead = 1; changed = 1; continue; }

                // Both precolored: only coalesce if same forced register
                if (g->precolored[u] >= 0 && g->precolored[v] >= 0) {
                    if (g->precolored[u] == g->precolored[v])
                        { inst->is_dead = 1; changed = 1; }
                    continue;
                }
                // Already interfere: cannot coalesce
                if (bv_test(g->adj[u], v)) continue;

                // Precolored node must be the "stay" side (u)
                if (g->precolored[v] >= 0) { int tmp = u; u = v; v = tmp; }

                // Among two non-precolored: put higher-degree as u for George's test
                if (g->precolored[u] < 0 && g->degree[v] > g->degree[u]) {
                    int tmp = u; u = v; v = tmp;
                }

                if (can_coalesce_george(g, u, v, K)) {
                    ig_merge(g, u, v);
                    inst->is_dead = 1;
                    changed = 1;
                }
            }
        }
    }
}

// Build interference graph for one function.
//
// The graph has f->nvalues virtual-register nodes (indexed by Value.id)
// plus IRC_K phantom nodes at indices f->nvalues..f->nvalues+IRC_K-1.
// Each phantom node is pre-colored with its physical register number.
//
// Call-site ABI: at each IK_CALL/IK_ICALL, values live through the call
// receive interference edges to the phantom nodes for r0..IRC_CALLER_REGS-1.
// This forces them to be colored with callee-saved registers (r4-r7) or
// spilled — no separate call_site_live side-channel needed.
static IGraph *build_interference_graph(Function *f) {
    int nv      = f->nvalues;   // virtual register count
    int phantom = nv;            // base index of phantom physical-register nodes
    IGraph *g   = ig_new(nv + IRC_K);

    // Mark pre-colored virtual values (call landings, params)
    for (int i = 0; i < nv; i++) {
        if (f->values[i]->phys_reg >= 0) {
            g->precolored[i] = f->values[i]->phys_reg;
            g->color[i]      = f->values[i]->phys_reg;
        }
    }

    // Initialize phantom nodes — permanently pre-colored, never simplified
    for (int r = 0; r < IRC_K; r++) {
        g->precolored[phantom + r] = r;
        g->color[phantom + r]      = r;
    }

    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        int nw = b->nwords;

        uint32_t *live = arena_alloc(nw * sizeof(uint32_t));
        memcpy(live, b->live_out, nw * sizeof(uint32_t));

        for (Inst *inst = b->tail; inst; inst = inst->prev) {
            // Def interferes with all currently live values
            if (inst->dst && inst->dst->id >= 0 && inst->dst->id < nv) {
                int d = inst->dst->id;
                for (int w = 0; w < nw; w++) {
                    uint32_t word = live[w];
                    while (word) {
                        int bit = __builtin_ctz(word); word &= word - 1;
                        int vid = w * 32 + bit;
                        if (vid != d && vid < nv)
                            ig_add_edge(g, d, vid);
                    }
                }
                bv_clear(live, d);
            }

            // Call-site interference: values that survive a call must not
            // occupy r0-r3 (caller-saved).  Add edges to phantom r0-r3 nodes
            // for every value live after (and therefore through) the call.
            if (inst->kind == IK_CALL || inst->kind == IK_ICALL || inst->kind == IK_SWITCH) {
                // IK_SWITCH dispatch uses one scratch from {r0,r1}; phantom-interfere
                // with those two so live-through values avoid them.
                int max_r = (inst->kind == IK_SWITCH) ? 2 : IRC_CALLER_REGS;
                for (int w = 0; w < nw; w++) {
                    uint32_t word = live[w];
                    while (word) {
                        int bit = __builtin_ctz(word); word &= word - 1;
                        int vid = w * 32 + bit;
                        if (vid < nv) {
                            for (int r = 0; r < max_r; r++)
                                ig_add_edge(g, vid, phantom + r);
                        }
                    }
                }
            }

            // Add uses to live
            for (int oi = 0; oi < inst->nops; oi++) {
                Value *v = val_resolve(inst->ops[oi]);
                if (v && v->kind == VAL_INST && v->id >= 0 && v->id < nv)
                    bv_set(live, v->id);
            }
        }
    }

    return g;
}

// Assign colors using a simple greedy graph coloring
// Returns 1 if all values colored, 0 if spills needed
static int assign_colors(IGraph *g, Function *f, int K) {
    // Simplification order: build a stack using degree < K heuristic
    int nv = g->nv;
    int *stack    = arena_alloc(nv * sizeof(int));
    int  stack_top = 0;
    int *removed  = arena_alloc(nv * sizeof(int));
    int *degree   = arena_alloc(nv * sizeof(int));
    memcpy(degree, g->degree, nv * sizeof(int));

    // Worklist: all non-precolored, non-aliased values participate in simplify
    int remaining = 0;
    for (int i = 0; i < nv; i++) {
        if (g->precolored[i] >= 0) { removed[i] = 1; } // pre-colored: skip
        else if (f->values[i]->kind != VAL_INST) { removed[i] = 1; }
        else if (ig_find(g, i) != i) { removed[i] = 1; } // merged away by coalescing
        else remaining++;
    }

    // Build copy-hint table for biased coloring: for each value that is the
    // destination of an IK_COPY, record the source value's id.
    int *copy_hint = arena_alloc(nv * sizeof(int));
    for (int i = 0; i < nv; i++) copy_hint[i] = -1;
    for (int i = 0; i < f->nvalues; i++) {
        Value *val = f->values[i];
        if (val->def && val->def->kind == IK_COPY && !val->def->is_dead &&
            val->def->nops >= 1) {
            Value *src = val_resolve(val->def->ops[0]);
            if (src && src->kind == VAL_INST && src->id >= 0 && src->id < nv)
                copy_hint[i] = src->id;
        }
    }

    while (remaining > 0) {
        // Find a node with degree < K
        int found = -1;
        for (int i = 0; i < nv; i++) {
            if (!removed[i] && degree[i] < K) { found = i; break; }
        }

        if (found < 0) {
            // All remaining have degree >= K: spill one.
            // Heuristic: minimize spill_cost/degree where
            //   spill_cost = use_count * 2^loop_depth
            // Values used heavily in inner loops are expensive to spill; high
            // degree means removing the node gives the most coloring benefit.
            int worst = -1;
            long long cost_worst = 0;
            for (int i = 0; i < nv; i++) {
                if (removed[i]) continue;
                Value *vi = f->values[i];
                int depth = (vi->def && vi->def->block)
                            ? vi->def->block->loop_depth : 0;
                if (depth > 20) depth = 20;  // clamp: 2^20 * use_count fits long long
                long long cost_i = (long long)vi->use_count << depth;
                // Prefer minimum cost_i/degree[i]; compare via cross-multiply
                // to avoid division: cost_i/deg_i < cost_w/deg_w
                //   ⟺  cost_i * deg_w < cost_w * deg_i
                if (worst < 0 ||
                    cost_i * (long long)degree[worst] <
                    cost_worst * (long long)degree[i]) {
                    worst = i;
                    cost_worst = cost_i;
                }
            }
            if (worst < 0) break;
            // Optimistic spilling (Briggs): push onto stack instead of
            // immediately marking as spilled.  During select, we'll try to
            // color it; only if no color is available does it actually spill.
            found = worst;
        }

        // Push onto stack and remove from graph
        stack[stack_top++] = found;
        removed[found] = 1;
        remaining--;
        // Update neighbors' degrees
        for (int w = 0; w < g->nwords; w++) {
            uint32_t word = g->adj[found][w];
            while (word) {
                int bit = __builtin_ctz(word);
                int vid = w * 32 + bit;
                if (vid < nv && !removed[vid]) degree[vid]--;
                word &= word - 1;
            }
        }
    }

    // Assign colors from stack (pop order)
    for (int i = stack_top - 1; i >= 0; i--) {
        int v = stack[i];
        if (g->spilled[v]) continue;

        // Collect forbidden colors from all colored neighbors.
        // Phantom nodes (indices nv..nv+K-1) are included since uid < g->nv,
        // and their g->color[] entries are set to their physical register —
        // so values that interfere with phantom-r0..r3 (because they're live
        // across a call) automatically have r0..r3 added to forbidden.
        uint8_t forbidden[MAX_REGS] = {0};
        for (int w = 0; w < g->nwords; w++) {
            uint32_t word = g->adj[v][w];
            while (word) {
                int bit = __builtin_ctz(word); word &= word - 1;
                int uid = w * 32 + bit;
                if (uid < nv && g->color[uid] >= 0)
                    forbidden[g->color[uid]] = 1;
            }
        }

        // Biased coloring: if this value is the destination of a copy,
        // prefer the source's register.  This eliminates copies that George's
        // test couldn't coalesce, without affecting colorability.
        int preferred = -1;
        if (v < f->nvalues && copy_hint[v] >= 0) {
            int p = ig_find(g, copy_hint[v]);
            if (g->color[p] >= 0 && !forbidden[g->color[p]])
                preferred = g->color[p];
        }

        // Pick a register: prefer copy partner's register (biased coloring),
        // else lowest unforbidden.  If this value is live across a call,
        // r0-r3 are already forbidden (via phantom-node interference).
        if (preferred >= 0) {
            g->color[v] = preferred;
        } else {
            for (int r = 0; r < K; r++) {
                if (!forbidden[r]) { g->color[v] = r; break; }
            }
        }
        if (g->color[v] < 0) {
            g->spilled[v] = 1; // couldn't color
        }
    }

    // Apply precolored
    for (int i = 0; i < nv; i++) {
        if (g->precolored[i] >= 0)
            g->color[i] = g->precolored[i];
    }

    // Propagate colors (and spill marks) to nodes merged away by coalescing
    for (int i = 0; i < nv; i++) {
        if (ig_find(g, i) != i) {
            int canon = ig_find(g, i);
            g->color[i]   = g->color[canon];
            g->spilled[i] = g->spilled[canon];
        }
    }

    // Check if any spills occurred
    for (int i = 0; i < nv; i++) {
        if (g->spilled[i]) return 0;
    }
    return 1;
}

// F2 bp-relative range: same thresholds as emit.c
// When a spill offset falls outside these ranges, emit.c cannot use the F2
// encoding and instead picks an arbitrary tmp register — potential clobber.
// Inserting an explicit IK_ADDR avoids the problem entirely.
static int spill_in_f2_range(int off, int sz) {
    if (sz == 4) return (off % 4) == 0 && off >= -256 && off <= 252;
    if (sz == 2) return (off % 2) == 0 && off >= -128 && off <= 126;
    return off >= -64 && off <= 63;
}

// Insert an IK_ADDR instruction to compute bp+slot_offset into a fresh register.
static Value *insert_spill_addr(Function *f, Block *b, Inst *anchor, int slot_offset) {
    Value *addr  = new_value(f, VAL_INST, VT_PTR);
    Inst  *la    = new_inst(f, b, IK_ADDR, addr);
    la->imm      = slot_offset;
    la->line     = anchor->line;
    la->block    = b;
    la->prev     = anchor->prev;
    la->next     = anchor;
    if (anchor->prev) anchor->prev->next = la;
    else              b->head = la;
    anchor->prev = la;
    return addr;
}

// Insert spill load/store for a spilled value
// Returns a fresh Value* that holds the loaded value
static Value *insert_spill_load(Function *f, Block *b, Inst *before,
                                 Value *spilled_val, int slot_offset) {
    int sz = (spilled_val->vtype == VT_F32 || spilled_val->vtype == VT_I32 ||
              spilled_val->vtype == VT_U32) ? 4 : 2;

    Value *tmp  = new_value(f, VAL_INST, spilled_val->vtype);
    Inst  *load = new_inst(f, b, IK_LOAD, tmp);
    load->size  = sz;
    if (before) load->line = before->line;

    // Insert before 'before' first (sets load->block, prev/next)
    load->block = b;
    if (!before) {
        inst_append(b, load);
    } else {
        load->prev = before->prev;
        load->next = before;
        if (before->prev) before->prev->next = load;
        else              b->head = load;
        before->prev = load;
    }

    if (spill_in_f2_range(slot_offset, sz)) {
        // F2 bp-relative: NULL base + imm encodes the offset
        load->imm = slot_offset;
        inst_add_op(load, NULL);
    } else {
        // Out of F2 range: explicit IK_ADDR so IRC allocates the address register.
        // Insert IK_ADDR before the load (which is now already in the block).
        Value *addr = insert_spill_addr(f, b, load, slot_offset);
        load->imm   = 0;
        inst_add_op(load, addr);
    }
    return tmp;
}

static void insert_spill_store(Function *f, Block *b, Inst *after,
                                Value *src, int slot_offset) {
    int sz = (src->vtype == VT_F32 || src->vtype == VT_I32 ||
              src->vtype == VT_U32) ? 4 : 2;

    Inst *store  = new_inst(f, b, IK_STORE, NULL);
    store->size  = sz;
    store->block = b;
    if (after) store->line = after->line;

    // Link after 'after' first
    if (!after) {
        inst_append(b, store);
    } else {
        store->prev = after;
        store->next = after->next;
        if (after->next) after->next->prev = store;
        else             b->tail = store;
        after->next = store;
    }

    if (spill_in_f2_range(slot_offset, sz)) {
        // F2 bp-relative: NULL base signals bp-relative encoding in emit.c
        store->imm = slot_offset;
        inst_add_op(store, src);
    } else {
        // Out of F2 range: explicit IK_ADDR so IRC allocates the address register
        // rather than emit.c picking an arbitrary (potentially live) tmp register.
        Value *addr = insert_spill_addr(f, b, store, slot_offset);
        store->imm  = 0;
        inst_add_op(store, addr);
        inst_add_op(store, src);
    }
}

// Rewrite function: replace spilled values with loads/stores
static void rewrite_spills(Function *f, IGraph *g) {
    int nv = g->nv;

    // Assign spill slots
    int *spill_offset = arena_alloc(nv * sizeof(int));

    for (int i = 0; i < nv; i++) {
        if (!g->spilled[i]) continue;
        Value *v = f->values[i];
        int sz = (v->vtype == VT_F32 || v->vtype == VT_I32 || v->vtype == VT_U32) ? 4 : 2;
        f->frame_size += sz;
        // Align: 4-byte spills need 4-byte alignment, 2-byte spills need 2-byte alignment
        if (sz == 4 && (f->frame_size % 4) != 0)
            f->frame_size += 2;
        if (sz == 2 && (f->frame_size % 2) != 0)
            f->frame_size += 1;
        spill_offset[i] = -(f->frame_size);  // negative = below bp
        v->spill_slot = spill_offset[i];
    }

    // For each instruction, replace uses of spilled values with loads,
    // and insert stores after defs of spilled values.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            // Replace uses
            for (int oi = 0; oi < inst->nops; oi++) {
                Value *v = val_resolve(inst->ops[oi]);
                if (!v || v->id >= nv) continue;
                if (g->spilled[v->id]) {
                    // When the use is inside a run of IK_COPY instructions
                    // (OOS phi copies), insert the spill load before the entire
                    // run.  Otherwise the emit-time scratch register for the lea
                    // in an out-of-F2-range load can clobber an earlier phi-copy
                    // destination that happens to share the same physical register.
                    Inst *insert_before = inst;
                    if (inst->kind == IK_COPY) {
                        while (insert_before->prev &&
                               insert_before->prev->kind == IK_COPY)
                            insert_before = insert_before->prev;
                    }
                    Value *tmp = insert_spill_load(f, b, insert_before, v, spill_offset[v->id]);
                    inst->ops[oi] = tmp;
                }
            }

            // Insert store after def.
            // IMPORTANT: advance inst past the inserted store so that the loop's
            // "inst = inst->next" doesn't re-process it.  If we did process it,
            // its src operand (the spilled value) would trigger another LOAD
            // insertion, creating a bogus LOAD-STORE pair with no actual store of
            // the freshly computed value.
            //
            // For out-of-F2-range spill slots, insert_spill_store inserts an
            // IK_ADDR *before* the IK_STORE (making copy->next = ADDR, not STORE).
            // A single inst->next only reaches the ADDR; the for-loop increment
            // would then land on the STORE and trigger the bogus LOAD above.
            // Skip the IK_ADDR too so the for-loop increment lands on STORE->next.
            if (inst->dst) {
                int id = inst->dst->id;
                if (id < nv && g->spilled[id]) {
                    insert_spill_store(f, b, inst, inst->dst, spill_offset[id]);
                    inst = inst->next; // skip to IK_ADDR (out-of-range) or IK_STORE (in-range)
                    if (inst && inst->kind == IK_ADDR) inst = inst->next; // skip IK_ADDR → land on IK_STORE
                }
            }
        }
    }

}

// ============================================================
// ============================================================
// Simple DCE: remove pure instructions with no uses
// ============================================================

static int is_pure_inst(Inst *inst) {
    switch (inst->kind) {
        case IK_STORE: case IK_CALL: case IK_ICALL:
        case IK_BR: case IK_JMP: case IK_RET: case IK_SWITCH:
        case IK_PUTCHAR: case IK_MEMCPY:
            return 0;
        default:
            return inst->dst != NULL;
    }
}

static void dce_pass(Function *f) {
    int nv = f->nvalues;

    // Recompute use_count from scratch
    for (int i = 0; i < nv; i++)
        f->values[i]->use_count = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            for (int oi = 0; oi < inst->nops; oi++) {
                Value *v = val_resolve(inst->ops[oi]);
                if (v && v->kind == VAL_INST)
                    v->use_count++;
            }
        }
    }

    // Iteratively remove dead pure instructions
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            Inst *inst = b->head;
            while (inst) {
                Inst *next = inst->next;
                if (!inst->is_dead && is_pure_inst(inst) &&
                    inst->dst && inst->dst->use_count == 0) {
                    // Remove this instruction
                    inst->is_dead = 1;
                    // Decrement operand use counts
                    for (int oi = 0; oi < inst->nops; oi++) {
                        Value *v = val_resolve(inst->ops[oi]);
                        if (v && v->kind == VAL_INST && v->use_count > 0)
                            v->use_count--;
                    }
                    // Unlink from block
                    if (inst->prev) inst->prev->next = inst->next;
                    else            b->head = inst->next;
                    if (inst->next) inst->next->prev = inst->prev;
                    else            b->tail = inst->prev;
                    changed = 1;
                }
                inst = next;
            }
        }
    }
}

// ============================================================
// Main IRC entry point
// ============================================================

void irc_allocate(Function *f) {
    // Pre-color certain values as r0:
    // (1) BR conditions: CPU4 jnz/jz test r0 implicitly.
    // (2) CALL/ICALL results: the processor ABI always returns to r0.
    //
    // We do NOT pre-color RET values: the emitter moves the return value
    // to r0 at emit time (clobbering r0 right before ret is always safe).
    //
    // Function parameters use a "landing + copy" scheme in braun.c:
    //   - landing value: pre-colored to r{idx} (the ABI register)
    //   - working copy: freely allocated by IRC
    // Since the working copy (the long-lived parameter value) may conflict
    // with r0 (via the BR condition's pre-coloring), IRC naturally assigns it
    // to a non-r0 register.  The IK_COPY emitter handles the entry-point move.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            // IK_BR condition: jnz now takes any register; no pre-coloring needed.
            if ((inst->kind == IK_CALL || inst->kind == IK_ICALL) && inst->dst) {
                if (inst->dst->kind == VAL_INST && inst->dst->phys_reg < 0)
                    inst->dst->phys_reg = 0;  // call result always in r0
            }
        }
    }

    // Remove dead pure instructions before liveness (eliminates dead readbacks)
    dce_pass(f);

    // Need liveness first
    compute_liveness(f);

    // Tiered IRC spill loop with guaranteed convergence.
    //
    // Tier 1 (iter 0-4): Full aggressive IRC — George coalescing, optimistic
    //   Briggs push, cost/degree spill selection.  Handles all observed cases.
    // Tier 2 (iter 5-7): Coalescing disabled — stabilizes graph topology by
    //   preventing coalescing-induced degree inflation between iterations.
    //   Copies survive as mov instructions but the graph stops shifting.
    // Tier 3 (iter 8+): Monotonic spilling — values spilled in any previous
    //   iteration are forced to stay spilled.  Each iteration can only add
    //   spills, guaranteeing termination.
    //
    // Biased coloring (in assign_colors) is always active: when choosing a
    // color, prefer the register of a copy partner to eliminate copies without
    // coalescing.

    int K = IRC_K;
    int max_iter = 20;

    // Persistent spill set for Tier 3: once a value is spilled, it stays
    // spilled.  Indexed by Value.id; only valid for ids < nv_at_entry.
    int nv_at_entry = f->nvalues;
    uint8_t *persist_spill = arena_alloc(nv_at_entry * sizeof(uint8_t));

    for (int iter = 0; iter < max_iter; iter++) {
        // Reset is_dead on all still-linked IK_COPY instructions before each
        // coalescing pass. DCE already unlinked truly dead instructions; any
        // IK_COPY still in the block was only killed by a prior coalescing
        // iteration and must be re-evaluated after spill rewrite changed the
        // interference graph.
        for (int bi = 0; bi < f->nblocks; bi++) {
            for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next)
                if (inst->kind == IK_COPY) inst->is_dead = 0;
        }

        IGraph *g = build_interference_graph(f);

        // Tier 3 (iter >= 8): force-spill persistent set before coloring.
        // Mark these values as already spilled in the graph so assign_colors
        // skips them and rewrite_spills inserts load/store for them.
        if (iter >= 8) {
            for (int i = 0; i < nv_at_entry && i < g->nv; i++) {
                if (persist_spill[i])
                    g->spilled[i] = 1;
            }
        }

        // Tier 1 (iter 0-4): full George coalescing
        // Tier 2 (iter 5-7): coalescing disabled — graph topology stabilizes
        // Tier 3 (iter 8+):  coalescing disabled
        if (iter < 5)
            coalesce_copies(g, f, K);

        int ok = assign_colors(g, f, K);

        if (ok) {
            // Apply colors to values
            for (int i = 0; i < f->nvalues && i < g->nv; i++) {
                if (g->color[i] >= 0)
                    f->values[i]->phys_reg = g->color[i];
            }
            ig_free(g);
            break;
        }

        // Tier 3: record newly spilled values into the persistent set
        if (iter >= 7) {
            for (int i = 0; i < nv_at_entry && i < g->nv; i++) {
                if (g->spilled[i])
                    persist_spill[i] = 1;
            }
        }

        // Rewrite spills and try again
        rewrite_spills(f, g);

        // Re-run liveness on the expanded function (old live_in/live_out stay in arena)
        compute_liveness(f);

        ig_free(g);
    }
}
