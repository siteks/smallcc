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
    bv[id / 32] |= (1u << (id % 32));
}

static void bv_clear(uint32_t *bv, int id) {
    bv[id / 32] &= ~(1u << (id % 32));
}

static int bv_test(uint32_t *bv, int id) {
    return (bv[id / 32] >> (id % 32)) & 1;
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
                // If t is precolored to the same register as u (precolored), merging
                // would add edge u-t between two same-color precolored nodes, which
                // means a non-precolored value would be coalesced into a precolored
                // group while also interfering with another member of that same group.
                if (g->precolored[u] >= 0 && g->precolored[t] >= 0 &&
                    g->precolored[t] == g->precolored[u])
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
// call_site_live[v] == 1 means value v is live across a call — it must stay
// in a callee-saved register and cannot be coalesced into a caller-saved one.
static void coalesce_copies(IGraph *g, Function *f, int K, int *call_site_live) {
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

                // If u is precolored to a caller-saved register (r0-r3) and v is
                // live across a call, coalescing would clobber v at call sites.
                if (g->precolored[u] >= 0 && g->precolored[u] < IRC_CALLER_REGS &&
                    call_site_live && call_site_live[v]) continue;

                if (can_coalesce_george(g, u, v, K)) {
                    ig_merge(g, u, v);
                    inst->is_dead = 1;
                    changed = 1;
                }
            }
        }
    }
}

// Build interference graph for one function
static IGraph *build_interference_graph(Function *f) {
    int nv = f->nvalues;
    IGraph *g = ig_new(nv);

    // Pre-color: values with phys_reg >= 0 set before IRC (e.g. IK_PARAM)
    // Mark them in the interference graph's precolored array.
    for (int i = 0; i < nv; i++) {
        if (f->values[i]->phys_reg >= 0) {
            g->precolored[i] = f->values[i]->phys_reg;
            g->color[i]      = f->values[i]->phys_reg;
        }
    }

    // Live-range interference: for each instruction, the def interferes
    // with everything live at that point.

    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        int nw = b->nwords;

        // Start with live_out, walk backward
        uint32_t *live = arena_alloc(nw * sizeof(uint32_t));
        memcpy(live, b->live_out, nw * sizeof(uint32_t));

        // Also add phi defs of successors' phis to live_out
        // (handled by liveness already)

        for (Inst *inst = b->tail; inst; inst = inst->prev) {
            // Handle call: caller-saved regs interfere with all live vregs
            if (inst->kind == IK_CALL || inst->kind == IK_ICALL) {
                // All values live at this call interfere with r0-r3
                // We represent this by marking bits 0..IRC_CALLER_REGS-1
                // as forced conflicts. We'll handle this via precolored nodes
                // or by just not assigning caller-saved regs to values live across calls.
                // For simplicity: any value live at a call site that later has
                // the caller-saved regs assigned will conflict — this is enforced
                // by adding interference edges to virtual "register nodes" 0-3.
                // We use value IDs 0..K-1 as precolored physical register nodes.
                // But our values start at id 0 for the first value...
                // This is tricky without reserved slots. We'll use a simpler approach:
                // mark live values as needing to avoid r0-r3.
                // For now: record that these live values cannot use r0-r3.
                // We do this by adding interference with a sentinel.
                // TODO: proper call interference via precolored nodes.
            }

            // The def interferes with all currently live values (except itself)
            if (inst->dst && inst->dst->id >= 0 && inst->dst->id < nv) {
                int d = inst->dst->id;
                for (int w = 0; w < nw; w++) {
                    uint32_t word = live[w];
                    while (word) {
                        int bit = __builtin_ctz(word);
                        int vid = w * 32 + bit;
                        if (vid != d && vid < nv)
                            ig_add_edge(g, d, vid);
                        word &= word - 1;
                    }
                }
                // Remove def from live
                bv_clear(live, d);
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
static int assign_colors(IGraph *g, Function *f, int K,
                          int *call_site_live_any) {
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
            g->spilled[worst] = 1;
            removed[worst] = 1;
            remaining--;
            // Update neighbors' degrees
            for (int w = 0; w < g->nwords; w++) {
                uint32_t word = g->adj[worst][w];
                while (word) {
                    int bit = __builtin_ctz(word);
                    int vid = w * 32 + bit;
                    if (vid < nv && !removed[vid]) degree[vid]--;
                    word &= word - 1;
                }
            }
            continue;
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

        // Find which colors are forbidden (used by neighbors)
        uint8_t forbidden[MAX_REGS] = {0};

        for (int w = 0; w < g->nwords; w++) {
            uint32_t word = g->adj[v][w];
            while (word) {
                int bit = __builtin_ctz(word);
                int uid = w * 32 + bit;
                if (uid < nv && g->color[uid] >= 0)
                    forbidden[g->color[uid]] = 1;
                word &= word - 1;
            }
        }

        // Also check precolored neighbors
        for (int w = 0; w < g->nwords; w++) {
            uint32_t word = g->adj[v][w];
            while (word) {
                int bit = __builtin_ctz(word);
                int uid = w * 32 + bit;
                if (uid < nv && g->precolored[uid] >= 0)
                    forbidden[g->precolored[uid]] = 1;
                word &= word - 1;
            }
        }

        // Values live at a call site must avoid r0-r3 (caller-saved).
        // Use callee-saved (r4-r7) only; if none available, spill — do not
        // fall back to caller-saved registers (they get clobbered by callees).
        if (call_site_live_any && call_site_live_any[v]) {
            for (int r = 4; r < K; r++) {
                if (!forbidden[r]) { g->color[v] = r; break; }
            }
            // If no callee-saved available: leave uncolored so spill fires
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

// Insert spill load/store for a spilled value
// Returns a fresh Value* that holds the loaded value
static Value *insert_spill_load(Function *f, Block *b, Inst *before,
                                 Value *spilled_val, int slot_offset) {
    // Create a fresh value
    Value *tmp = new_value(f, VAL_INST, spilled_val->vtype);
    Inst  *load = new_inst(f, b, IK_LOAD, tmp);
    load->imm  = slot_offset;
    load->size = (spilled_val->vtype == VT_F32 || spilled_val->vtype == VT_I32 ||
                  spilled_val->vtype == VT_U32) ? 4 : 2;
    // NULL base signals bp-relative spill load (emit.c checks for nops<1 or NULL base)
    inst_add_op(load, NULL);
    // Insert before 'before'
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
    return tmp;
}

static void insert_spill_store(Function *f, Block *b, Inst *after,
                                Value *src, int slot_offset) {
    Inst *store = new_inst(f, b, IK_STORE, NULL);
    store->imm  = slot_offset;
    store->size = (src->vtype == VT_F32 || src->vtype == VT_I32 ||
                   src->vtype == VT_U32) ? 4 : 2;
    inst_add_op(store, src);  // address comes from frame (imm = offset)
    store->block = b;
    if (!after) {
        inst_append(b, store);
    } else {
        store->prev = after;
        store->next = after->next;
        if (after->next) after->next->prev = store;
        else             b->tail = store;
        after->next = store;
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
            if (inst->dst) {
                int id = inst->dst->id;
                if (id < nv && g->spilled[id]) {
                    insert_spill_store(f, b, inst, inst->dst, spill_offset[id]);
                    inst = inst->next; // skip the just-inserted store
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
        case IK_BR: case IK_JMP: case IK_RET:
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

    // Compute call-site liveness (which values are live at any call)
    int nv = f->nvalues;
    int *call_site_live = arena_alloc(nv * sizeof(int));
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        int nw = b->nwords;
        uint32_t *live = arena_alloc(nw * sizeof(uint32_t));
        memcpy(live, b->live_out, nw * sizeof(uint32_t));
        for (Inst *inst = b->tail; inst; inst = inst->prev) {
            if (inst->kind == IK_CALL || inst->kind == IK_ICALL) {
                for (int w = 0; w < nw; w++) {
                    uint32_t word = live[w];
                    while (word) {
                        int bit = __builtin_ctz(word);
                        int vid = w * 32 + bit;
                        if (vid < nv) call_site_live[vid] = 1;
                        word &= word - 1;
                    }
                }
            }
            if (inst->dst && inst->dst->id >= 0 && inst->dst->id < nv)
                bv_clear(live, inst->dst->id);
            for (int oi = 0; oi < inst->nops; oi++) {
                Value *v = val_resolve(inst->ops[oi]);
                if (v && v->kind == VAL_INST && v->id >= 0 && v->id < nv)
                    bv_set(live, v->id);
            }
        }
    }

    // Iterate until no spills
    int K = IRC_K;
    int max_iter = 20;
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

        coalesce_copies(g, f, K, call_site_live);

        int ok = assign_colors(g, f, K, call_site_live);

        if (ok) {
            // Apply colors to values
            for (int i = 0; i < f->nvalues && i < g->nv; i++) {
                if (g->color[i] >= 0)
                    f->values[i]->phys_reg = g->color[i];
            }
            ig_free(g);
            break;
        }

        // Rewrite spills and try again
        rewrite_spills(f, g);

        // Re-run liveness on the expanded function (old live_in/live_out stay in arena)
        compute_liveness(f);

        ig_free(g);

        // Reassign call_site_live for expanded function (fresh arena allocation)
        nv = f->nvalues;
        call_site_live = arena_alloc(nv * sizeof(int));
        // (simplified: just re-run the call site scan)
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            int nw = b->nwords;
            uint32_t *live = arena_alloc(nw * sizeof(uint32_t));
            memcpy(live, b->live_out, nw * sizeof(uint32_t));
            for (Inst *inst = b->tail; inst; inst = inst->prev) {
                if (inst->kind == IK_CALL || inst->kind == IK_ICALL) {
                    for (int w = 0; w < nw; w++) {
                        uint32_t word = live[w];
                        while (word) {
                            int bit = __builtin_ctz(word);
                            int vid = w * 32 + bit;
                            if (vid < nv) call_site_live[vid] = 1;
                            word &= word - 1;
                        }
                    }
                }
                if (inst->dst && inst->dst->id >= 0 && inst->dst->id < nv)
                    bv_clear(live, inst->dst->id);
                for (int oi = 0; oi < inst->nops; oi++) {
                    Value *v = val_resolve(inst->ops[oi]);
                    if (v && v->kind == VAL_INST && v->id >= 0 && v->id < nv)
                        bv_set(live, v->id);
                }
            }
        }
    }
}
