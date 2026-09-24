#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "alloc.h"
#include "dom.h"

// ============================================================
// Per-function clobber-set record
// ============================================================
//
// After allocating function F, we record which physical registers F may
// clobber (transitively, including functions F calls).  At call sites in
// other functions, we then add phantom-register interference edges only
// for the registers the callee actually clobbers, instead of unconditionally
// reserving all of r0-r3.  This lets live-through values keep caller-saved
// registers across calls whose callees happen not to use them.
//
// Fallback for unknown callees (not yet seen, ICALL, cross-TU before the
// callee has been compiled): 0x0F = full caller-saved (r0-r3).

typedef struct ClobberEntry {
    char *name;
    uint8_t mask;
    struct ClobberEntry *next;
} ClobberEntry;

static ClobberEntry *clobber_head = NULL;

static ClobberEntry *find_clobber_entry(const char *name) {
    if (!name) return NULL;
    for (ClobberEntry *e = clobber_head; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

static uint8_t lookup_clobbers(const char *name) {
    ClobberEntry *e = find_clobber_entry(name);
    if (e) return e->mask;
    return 0x0F;  // unknown: conservatively assume all caller-saved
}

static void record_clobbers(const char *name, uint8_t mask) {
    if (!name) return;
    ClobberEntry *e = find_clobber_entry(name);
    if (e) { e->mask = mask; return; }
    e = (ClobberEntry *)malloc(sizeof(ClobberEntry));
    e->name = strdup(name);
    e->mask = mask;
    e->next = clobber_head;
    clobber_head = e;
}

void irc_add_clobbers(const char *name, uint8_t mask) {
    if (!name || !mask) return;
    ClobberEntry *e = find_clobber_entry(name);
    if (e) e->mask |= mask;
    else   record_clobbers(name, (uint8_t)(mask | 0x0F));  // unknown base: conservative
}

static void record_function_clobbers(Function *f) {
    if (!f->name) return;
    uint8_t mask = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            // Only caller-saved registers are clobbers: r4-r7 are saved and
            // restored by the callee's own prologue/epilogue, so a value the
            // caller keeps there survives the call.  Recording them here made
            // callers spill everything live across a call to any callee that
            // happened to use all four.
            if (inst->dst && inst->dst->phys_reg >= 0 && inst->dst->phys_reg < IRC_CALLER_REGS)
                mask |= (1u << inst->dst->phys_reg);
            if (inst->kind == IK_CALL && inst->fname)
                mask |= lookup_clobbers(inst->fname);
            else if (inst->kind == IK_ICALL)
                mask |= 0x0F;  // unknown target: assume all caller-saved
        }
    }
    if (getenv("DBG_IRC")) fprintf(stderr, "IRC clobbers %s = 0x%02x\n", f->name, mask);
    record_clobbers(f->name, mask);
}

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

            // No phi handling needed: compute_liveness only runs from
            // irc_allocate, strictly after out_of_ssa has removed all phis.

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
                // IK_FMADD/IK_FMSUB are two-address (dst wants ops[0]'s register):
                // treat (dst, ops[0]) as move-related; on success nothing is
                // deleted, emission just sees the registers coincide.
                int is_copy = (inst->kind == IK_COPY);
                int is_acc  = (inst->kind == IK_FMADD || inst->kind == IK_FMSUB);
                if ((!is_copy && !is_acc) || inst->is_dead || inst->nops < 1) continue;
                Value *vdst = val_resolve(inst->dst);
                Value *vsrc = val_resolve(inst->ops[0]);
                if (!vdst || !vsrc || vdst == vsrc) { if (is_copy) { inst->is_dead = 1; changed = 1; } continue; }
                if (vdst->kind != VAL_INST || vsrc->kind != VAL_INST) continue;
                if (vdst->id < 0 || vsrc->id < 0 ||
                    vdst->id >= g->nv || vsrc->id >= g->nv) continue;

                int u = ig_find(g, vdst->id);
                int v = ig_find(g, vsrc->id);
                if (u == v) { if (is_copy) { inst->is_dead = 1; changed = 1; } continue; }

                // Both precolored: only coalesce if same forced register
                if (g->precolored[u] >= 0 && g->precolored[v] >= 0) {
                    if (g->precolored[u] == g->precolored[v] && is_copy)
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
                    if (is_copy) inst->is_dead = 1;
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
            // Def interferes with all currently live values.
            // Exception (Appel & George §11.3): for IK_COPY dst←src,
            // dst does NOT interfere with src.  Both can share a register.
            if (inst->dst && inst->dst->id >= 0 && inst->dst->id < nv) {
                int d = inst->dst->id;
                int copy_src = -1;
                if (inst->kind == IK_COPY && inst->nops >= 1) {
                    Value *sv = val_resolve(inst->ops[0]);
                    if (sv && sv->kind == VAL_INST && sv->id >= 0 && sv->id < nv)
                        copy_src = sv->id;
                }
                for (int w = 0; w < nw; w++) {
                    uint32_t word = live[w];
                    while (word) {
                        int bit = __builtin_ctz(word); word &= word - 1;
                        int vid = w * 32 + bit;
                        if (vid != d && vid < nv && vid != copy_src)
                            ig_add_edge(g, d, vid);
                    }
                }
                bv_clear(live, d);

                // IK_MEMCPY's dst is the scratch for the inline copy loop
                // (attached by legalize Pass B2). It is written between
                // reads of the pointer operands, so it must not share a
                // register with them even though their live ranges may
                // end at this instruction.
                if (inst->kind == IK_MEMCPY) {
                    for (int oi = 0; oi < inst->nops; oi++) {
                        Value *ov = val_resolve(inst->ops[oi]);
                        if (ov && ov->kind == VAL_INST &&
                            ov->id >= 0 && ov->id < nv && ov->id != d)
                            ig_add_edge(g, d, ov->id);
                    }
                }
            }

            // Call-site interference: values that survive a call must not
            // occupy any register the callee clobbers.  Use the per-callee
            // clobber mask recorded by record_function_clobbers at the end
            // of its irc_allocate; unknown/ICALL fall back to all of r0-r3.
            if (inst->kind == IK_CALL || inst->kind == IK_ICALL || inst->kind == IK_SWITCH) {
                uint8_t cmask;
                if (inst->kind == IK_SWITCH)
                    cmask = 0x03;  // dispatch uses a scratch from {r0,r1}
                else if (inst->kind == IK_ICALL)
                    cmask = 0x0F;  // unknown target: all caller-saved
                else
                    cmask = lookup_clobbers(inst->fname) & 0x0F;   // caller-saved only
                for (int w = 0; w < nw; w++) {
                    uint32_t word = live[w];
                    while (word) {
                        int bit = __builtin_ctz(word); word &= word - 1;
                        int vid = w * 32 + bit;
                        if (vid < nv) {
                            uint8_t m = cmask;
                            while (m) {
                                int r = __builtin_ctz(m); m &= m - 1;
                                ig_add_edge(g, vid, phantom + r);
                            }
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

static int is_rematerializable(Value *v);   /* forward decl: defined below */

// Assign colors using a simple greedy graph coloring
// Returns 1 if all values colored, 0 if spills needed.
//
// `persist_spill` (may be NULL) is the persistent-spill set used by the IRC
// outer loop's tier-3 spilling. When provided, the success check uses it
// to distinguish "this value was spilled in a prior iteration and has
// already been processed by rewrite_spills" (success-compatible) from
// "this value was newly spilled in *this* assign_colors call and rewrite
// hasn't run yet" (must return 0). Without that distinction,
// rematerializable values — which never get a frame slot via
// rewrite_spills — would either look like fresh spills forever (old
// behaviour, EXHAUST 500) or like immediate successes before rewrite
// inserted their per-use clones (the alternative also-broken).
static int assign_colors(IGraph *g, Function *f, int K, int pessimistic,
                          const uint8_t *persist_spill, int persist_cap) {
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
    // Also hint in-place ISA operations (shrsi/shli/andi/sxb/sxw/inc/dec/addi)
    // so IRC prefers assigning dst the same register as the non-const operand,
    // eliminating the mov that emit.c must insert for cross-register cases.
    int *copy_hint = arena_alloc(nv * sizeof(int));
    for (int i = 0; i < nv; i++) copy_hint[i] = -1;

    // Pre-scan (leaf functions only): OOS phi elimination creates multiple defs
    // for loop-carried values.  val->def points to the last def (typically the
    // back-edge copy), missing the entry copy from a precolored param register.
    // Scan all instructions for IK_COPY whose source traces (through at most
    // one intermediate non-coalesced copy) to a precolored value.  These hints
    // get priority — matching the param register avoids entry-block shuffles.
    //
    // Restricted to leaf functions: in non-leaf functions, call-site phantom
    // interference already constrains register choice, and pre-scan hints can
    // interfere with pressure dynamics around calls.
    //
    // Phase 1: copies directly from precolored (or coalesced-with-precolored).
    // Phase 2: copies from a value that phase 1 already hinted to precolored.
    // This covers the common param→working→loop chain (IK_PARAM→IK_COPY→IK_COPY).
    int is_leaf = 1;
    for (int bi = 0; bi < f->nblocks && is_leaf; bi++)
        for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next)
            if (inst->kind == IK_CALL || inst->kind == IK_ICALL)
                { is_leaf = 0; break; }
    for (int phase = 0; phase < 2 && is_leaf; phase++) {
        for (int bi = 0; bi < f->nblocks; bi++) {
            for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next) {
                if (inst->is_dead || inst->kind != IK_COPY || inst->nops < 1) continue;
                if (!inst->dst || inst->dst->kind != VAL_INST) continue;
                int did = inst->dst->id;
                if (did < 0 || did >= nv) continue;
                // Resolve to canonical (coalescing may have merged dst away)
                int did_canon = ig_find(g, did);
                if (did_canon < 0 || did_canon >= nv || copy_hint[did_canon] >= 0) continue;
                Value *src = val_resolve(inst->ops[0]);
                if (!src || src->kind != VAL_INST || src->id < 0 || src->id >= nv) continue;
                int sid = src->id;
                int src_canon = ig_find(g, sid);
                int precolored_src = -1;
                if (g->precolored[src_canon] >= 0) {
                    precolored_src = src_canon;
                } else if (phase == 1) {
                    // Phase 2: check if source's canonical has a hint
                    // that chains to a precolored value
                    int sh = copy_hint[src_canon];
                    if (sh < 0 && sid != src_canon)
                        sh = copy_hint[sid];
                    if (sh >= 0 && g->precolored[ig_find(g, sh)] >= 0)
                        precolored_src = ig_find(g, sh);
                }
                // Store the precolored value directly as the hint so that
                // the coloring phase can resolve it immediately (precolored
                // values are always colored), without needing intermediate
                // values to be colored first.
                if (precolored_src >= 0)
                    copy_hint[did_canon] = precolored_src;
            }
        }
    }

    for (int i = 0; i < f->nvalues; i++) {
        if (copy_hint[i] >= 0) continue;  // preserve precolored-source hint
        Value *val = f->values[i];
        if (!val->def || val->def->is_dead) continue;
        Inst *def = val->def;
        if (def->kind == IK_COPY && def->nops >= 1) {
            Value *src = val_resolve(def->ops[0]);
            if (src && src->kind == VAL_INST && src->id >= 0 && src->id < nv)
                copy_hint[i] = src->id;
            continue;
        }
        // In-place peephole candidates: hint dst → non-const operand.
        // IK_SEXT8/IK_SEXT16: always in-place on ops[0].
        if ((def->kind == IK_SEXT8 || def->kind == IK_SEXT16) && def->nops >= 1) {
            Value *src = val_resolve(def->ops[0]);
            if (src && src->kind == VAL_INST && src->id >= 0 && src->id < nv)
                copy_hint[i] = src->id;
            continue;
        }
        // IK_SHL/IK_SHR/IK_USHR with const shift → shli/shrsi: in-place on ops[0].
        // IK_AND with const mask → andi/zxb/zxw: in-place on non-const operand.
        // IK_ADD/IK_SUB with small const → inc/dec/addi: in-place on non-const operand.
        if ((def->kind == IK_SHL || def->kind == IK_SHR || def->kind == IK_USHR ||
             def->kind == IK_AND || def->kind == IK_ADD || def->kind == IK_SUB) &&
            def->nops >= 2) {
            Value *o0 = val_resolve(def->ops[0]);
            Value *o1 = val_resolve(def->ops[1]);
            int k, c0 = get_iconst(o0, &k), c1 = get_iconst(o1, &k);
            // Exactly one const operand → hint the non-const one.
            Value *src = NULL;
            if (c1 && !c0) src = o0;
            else if (c0 && !c1) src = o1;
            if (src && src->kind == VAL_INST && src->id >= 0 && src->id < nv)
                copy_hint[i] = src->id;
        }
    }

    // Check if function has any loops (for simplification heuristic).
    int has_loops = 0;
    for (int bi = 0; bi < f->nblocks && !has_loops; bi++)
        if (f->blocks[bi]->loop_depth > 0) has_loops = 1;

    while (remaining > 0) {
        // Find a node with degree < K.
        // In loop-free leaf functions, defer pre-scan-hinted values so
        // they are pushed later (and thus colored earlier during select).
        // This ensures param-linked values get first pick at their
        // hinted register, eliminating unnecessary prologue shuffles.
        // Restricted to loop-free functions because in loop-heavy code,
        // keeping params in their original registers can force worse
        // loop-body allocation (loop constants can't live in param regs).
        int found = -1;
        if (is_leaf && !has_loops) {
            int hinted_fallback = -1;
            for (int i = 0; i < nv; i++) {
                if (removed[i] || degree[i] >= K) continue;
                if (copy_hint[i] >= 0 && i < f->nvalues) {
                    int hc = ig_find(g, copy_hint[i]);
                    if (g->precolored[hc] >= 0) {
                        if (hinted_fallback < 0) hinted_fallback = i;
                        continue;
                    }
                }
                found = i; break;
            }
            if (found < 0) found = hinted_fallback;
        } else {
            for (int i = 0; i < nv; i++) {
                if (!removed[i] && degree[i] < K) { found = i; break; }
            }
        }

        if (found < 0) {
            // All remaining have degree >= K: spill one.
            // Heuristic: minimize spill_cost/degree.
            // spill_cost = max(def_cost, use_cost) where:
            //   def_cost  = use_count * 2^def_block_loop_depth
            //   use_cost  = sum over uses of 2^use_block_loop_depth
            // The max ensures outer-loop defs with inner-loop uses get
            // high cost (use_cost dominates) while values with many uses
            // in their def block also stay protected (def_cost dominates).
            // Spill temps (reloads/remats inserted by a previous
            // rewrite_spills) are excluded in the first pass: their tiny
            // use_count makes them the cheapest candidate every time, and
            // re-spilling a reload livelocks the whole allocation (each
            // iteration spills the previous iteration's temp — fuzz seed
            // 40156 span 500 iterations this way). Fall back to allowing
            // them only if no real value is left to spill.
            int worst = -1;
            long long cost_worst = 0;
            for (int allow_tmp = 0; allow_tmp < 2 && worst < 0; allow_tmp++) {
                for (int i = 0; i < nv; i++) {
                    if (removed[i]) continue;
                    Value *vi = f->values[i];
                    if (vi->is_spill_tmp && !allow_tmp) continue;
                    int depth = (vi->def && vi->def->block)
                                ? vi->def->block->loop_depth : 0;
                    if (depth > 20) depth = 20;
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
            }
            if (worst < 0) break;
            if (pessimistic) {
                // Pessimistic spilling: mark as spilled immediately.
                // Used in tier 3 to break cascade growth by spilling all
                // high-degree values at once rather than one per iteration.
                g->spilled[worst] = 1;
                removed[worst] = 1;
                remaining--;
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

        // Biased coloring: prefer the hint source's register.
        // If the direct hint target isn't colored yet, chase one level
        // through the hint chain (transitive hint).  This handles chains
        // like SHR→AND where both hint to the same source register but
        // the intermediate value hasn't been colored yet.
        int preferred = -1;
        if (v < f->nvalues && copy_hint[v] >= 0) {
            int p = ig_find(g, copy_hint[v]);
            if (g->color[p] >= 0 && !forbidden[g->color[p]])
                preferred = g->color[p];
            else if (p < f->nvalues && copy_hint[p] >= 0) {
                int pp = ig_find(g, copy_hint[p]);
                if (g->color[pp] >= 0 && !forbidden[g->color[pp]])
                    preferred = g->color[pp];
            }
        }

        // Pick a register: prefer copy partner's register (biased coloring),
        // else lowest unforbidden.
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

    // Check if any NEW spills occurred (values that weren't pre-marked by
    // the persistent set). A pre-marked spill that stays spilled is expected;
    // only a newly-failed coloring means we need another rewrite round.
    //
    // The "is this a new spill" test uses two signals:
    //   - persist_spill[i] is set — this value was spilled in a prior IRC
    //                              iteration and rewrite_spills has already
    //                              processed it (allocated a slot, or for
    //                              rematerializable values inserted the
    //                              per-use clones). Treat as success.
    //   - v->spill_slot != -1     — for non-remat values, equivalent to
    //                              the persistent check (rewrite gave it
    //                              a slot). Used as a fallback when no
    //                              persist set is supplied.
    //
    // Without this distinction, remat values — which never get a frame slot
    // — either look like fresh spills forever (old behaviour, EXHAUST 500
    // on njRowIDCT) or like immediate successes on iter 0 before rewrite
    // had a chance to insert their per-use remat clones.
    for (int i = 0; i < nv; i++) {
        if (g->spilled[i] && g->color[i] < 0 && g->precolored[i] < 0) {
            Value *v = f->values[i];
            int already_persistent = (persist_spill && i < persist_cap && persist_spill[i]);
            if (!already_persistent && v->spill_slot == -1)
                return 0;  // new spill — need another round
        }
    }
    return 1;
}

// Spill slot size in bytes: 32-bit types take 4, everything else takes 2.
// Deliberately min-2 (do NOT use vtype_size): sub-word values spill as a
// full 16-bit word.
static int spill_size(ValType vt) {
    // ILP32: VT_PTR must be in the 4-byte set — a spilled pointer in a
    // 2-byte slot round-trips through lw/sw and loses its high half.
    return (vt == VT_F32 || vt == VT_I32 || vt == VT_U32 || vt == VT_PTR) ? 4 : 2;
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
    inst_insert_before(anchor, la);
    return addr;
}

// Insert spill load/store for a spilled value
// Returns a fresh Value* that holds the loaded value
static Value *insert_spill_load(Function *f, Block *b, Inst *before,
                                 Value *spilled_val, int slot_offset) {
    int sz = spill_size(spilled_val->vtype);

    Value *tmp  = new_value(f, VAL_INST, spilled_val->vtype);
    tmp->is_spill_tmp = 1;
    Inst  *load = new_inst(f, b, IK_LOAD, tmp);
    load->size  = sz;
    if (before) load->line = before->line;

    // Insert before 'before' first (sets load->block, prev/next)
    if (!before) inst_append(b, load);
    else         inst_insert_before(before, load);

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
    int sz = spill_size(src->vtype);

    Inst *store  = new_inst(f, b, IK_STORE, NULL);
    store->size  = sz;
    if (after) store->line = after->line;

    // Link after 'after' first
    if (!after) inst_append(b, store);
    else        inst_insert_after(after, store);

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

// Can this value be rematerialized (recomputed) at each use site instead
// of spilling to a stack slot?  Only trivial single-instruction values
// with no operands qualify: IK_CONST and IK_GADDR.
static int is_rematerializable(Value *v) {
    if (!v || !v->def) return 0;
    return v->def->kind == IK_CONST || v->def->kind == IK_GADDR;
}

// Insert a rematerialized copy of 'orig_def' before 'before' in block 'b'.
// Returns the fresh value that replaces the spilled original.
static Value *insert_remat(Function *f, Block *b, Inst *before, Value *orig) {
    Inst *def = orig->def;
    Value *tmp = new_value(f, VAL_INST, orig->vtype);
    tmp->is_spill_tmp = 1;
    Inst *clone = new_inst(f, b, def->kind, tmp);
    clone->imm   = def->imm;
    clone->fname = def->fname;
    if (before) clone->line = before->line;

    // Insert before 'before'
    if (!before) inst_append(b, clone);
    else         inst_insert_before(before, clone);
    return tmp;
}

// Rewrite function: replace spilled values with loads/stores (or
// rematerialization for IK_CONST/IK_GADDR).
static void rewrite_spills(Function *f, IGraph *g) {
    int nv = g->nv;

    // Assign spill slots (skip rematerializable values — they don't need slots)
    int *spill_offset = arena_alloc(nv * sizeof(int));
    // Track which values are NEWLY spilled this iteration (need load/store insertion).
    // Values already spilled in a prior iteration have loads/stores in place.
    uint8_t *newly_spilled = arena_alloc(nv * sizeof(uint8_t));

    for (int i = 0; i < nv; i++) {
        if (!g->spilled[i]) continue;
        Value *v = f->values[i];
        if (is_rematerializable(v)) {
            newly_spilled[i] = 1;  // remat values always need per-use insertion
            continue;
        }
        if (v->spill_slot != -1) {
            // Already spilled in a prior iteration — reuse existing slot,
            // skip instruction rewriting (loads/stores already in place).
            spill_offset[i] = v->spill_slot;
            continue;
        }
        newly_spilled[i] = 1;
        int sz = spill_size(v->vtype);
        // Align FIRST so the resulting offset is naturally aligned for `sz`.
        // The previous `+= 2` / `+= 1` heuristics only worked when frame_size
        // was already at a specific residue; under ILP32 the new mix of int
        // (4-byte) and short (2-byte) and char (1-byte) spills hits all four
        // residues mod 4. Round frame_size up to the next multiple of `sz`,
        // then add `sz`. spill_offset = -frame_size lands at a -multiple-of-sz
        // boundary as required by the F2 / F3c bp-relative ops.
        if (sz == 4) f->frame_size = (f->frame_size + 3) & ~3;
        else if (sz == 2) f->frame_size = (f->frame_size + 1) & ~1;
        f->frame_size += sz;
        spill_offset[i] = -(f->frame_size);  // negative = below bp
        v->spill_slot = spill_offset[i];
    }

    // For each instruction, replace uses of spilled values with loads
    // (or rematerializations), and insert stores after defs of spilled values.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            // Replace uses
            for (int oi = 0; oi < inst->nops; oi++) {
                Value *v = val_resolve(inst->ops[oi]);
                if (!v || v->id >= nv) continue;
                if (g->spilled[v->id] && newly_spilled[v->id]) {
                    if (is_rematerializable(v)) {
                        // Rematerialize: clone the IK_CONST/IK_GADDR before this use
                        Value *tmp = insert_remat(f, b, inst, v);
                        inst->ops[oi] = tmp;
                        if (v->use_count > 0) v->use_count--;
                        tmp->use_count++;
                    } else {
                        // Spill load from stack slot
                        Inst *insert_before = inst;
                        if (inst->kind == IK_COPY) {
                            while (insert_before->prev &&
                                   insert_before->prev->kind == IK_COPY)
                                insert_before = insert_before->prev;
                        }
                        Value *tmp = insert_spill_load(f, b, insert_before, v, spill_offset[v->id]);
                        inst->ops[oi] = tmp;
                        if (v->use_count > 0) v->use_count--;
                        tmp->use_count++;
                    }
                }
            }

            // Insert store after def (skip rematerializable — no slot to store to)
            if (inst->dst) {
                int id = inst->dst->id;
                if (id < nv && g->spilled[id] && newly_spilled[id] && !is_rematerializable(inst->dst)) {
                    insert_spill_store(f, b, inst, inst->dst, spill_offset[id]);
                    inst = inst->next;
                    if (inst && inst->kind == IK_ADDR) inst = inst->next;
                }
            }
        }
    }

    // Rematerialized values: every use has been replaced by a clone at the
    // use site, so the original def is now unused. Mark it dead so it is
    // neither colored by the next IRC iteration nor emitted.
    for (int i = 0; i < nv; i++) {
        if (!g->spilled[i] || !newly_spilled[i]) continue;
        Value *v = f->values[i];
        if (is_rematerializable(v) && v->use_count == 0 && v->def)
            v->def->is_dead = 1;
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

    // Pre-pass: unlink instructions already marked dead by earlier passes
    // (e.g. CSE).  These must be removed before liveness analysis because
    // their operand references create spurious live ranges and interference.
    // Do NOT decrement use_counts here — CSE/copy_prop already recounted
    // use_counts excluding dead instructions.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        Inst *inst = b->head;
        while (inst) {
            Inst *next = inst->next;
            if (inst->is_dead) {
                if (inst->prev) inst->prev->next = inst->next;
                else            b->head = inst->next;
                if (inst->next) inst->next->prev = inst->prev;
                else            b->tail = inst->prev;
            }
            inst = next;
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
    int max_iter = 500;
    // Persistent spill set for Tier 3: once a value is spilled, it stays
    // spilled.  Dynamically grown as rewrite_spills adds new values.
    int persist_cap = f->nvalues + 64;  // initial capacity with headroom
    uint8_t *persist_spill = arena_alloc(persist_cap * sizeof(uint8_t));

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
        //
        // Skip values that already have a slot: rewrite_spills has shortened
        // their live range to [def, spill-store] in a prior iteration, and
        // they need a register for that brief window so emit.c can read the
        // def's output and write it to the slot. Marking them spilled here
        // would leave them uncolored, and emit.c's preg() fallback to r0
        // would collide with the IK_ADDR that the spill-store uses.
        if (iter >= 8) {
            int lim = persist_cap < g->nv ? persist_cap : g->nv;
            for (int i = 0; i < lim; i++) {
                if (!persist_spill[i]) continue;
                if (i < f->nvalues && f->values[i]->spill_slot != -1) continue;
                g->spilled[i] = 1;
            }
        }

        // Tier 1 (iter 0-4): full George coalescing
        // Tier 2 (iter 5-7): coalescing disabled — graph topology stabilizes
        // Tier 3 (iter 8+):  coalescing disabled
        if (iter < 5)
            coalesce_copies(g, f, K);

        // Tier 3 (iter >= 8): pessimistic spilling to break cascade
        int pessimistic = (iter >= 8);
        int ok = assign_colors(g, f, K, pessimistic,
                               iter >= 8 ? persist_spill : NULL,
                               iter >= 8 ? persist_cap : 0);

        if (ok) {
            // Apply colors to values
            for (int i = 0; i < f->nvalues && i < g->nv; i++) {
                if (g->color[i] >= 0)
                    f->values[i]->phys_reg = g->color[i];
            }
            ig_free(g);
            record_function_clobbers(f);
            break;
        }

        // Tier 3: record newly spilled values into the persistent set.
        // Grow the set if needed to cover spill temps from rewrite_spills.
        if (iter >= 7) {
            if (g->nv > persist_cap) {
                uint8_t *old = persist_spill;
                persist_spill = arena_alloc(g->nv * sizeof(uint8_t));
                for (int i = 0; i < persist_cap; i++)
                    persist_spill[i] = old[i];
                persist_cap = g->nv;
            }
            for (int i = 0; i < g->nv; i++) {
                if (g->spilled[i])
                    persist_spill[i] = 1;
            }
        }

        if (getenv("DBG_IRC")) {
            int spilled_count = 0;
            int new_spills    = 0;       /* spills not yet in persistent set */
            int uncolored_no_slot = 0;   /* values that the success check rejects as "new" */
            for (int i = 0; i < g->nv; i++) {
                if (g->spilled[i]) spilled_count++;
                if (g->spilled[i] && i < persist_cap && !persist_spill[i]) new_spills++;
                if (g->spilled[i] && g->color[i] < 0 && g->precolored[i] < 0 &&
                    i < f->nvalues && f->values[i]->spill_slot == -1) uncolored_no_slot++;
            }
            fprintf(stderr, "IRC %s iter=%d ok=%d nv=%d/%d spilled=%d new_persist=%d uncolored_no_slot=%d\n",
                    f->name, iter, ok, g->nv, f->nvalues, spilled_count, new_spills, uncolored_no_slot);
        }

        // Rewrite spills and try again
        rewrite_spills(f, g);

        // Re-run liveness on the expanded function (old live_in/live_out stay in arena)
        compute_liveness(f);

        ig_free(g);

        if (iter == max_iter - 1)
            error("IRC: %s failed to converge after %d iterations "
                  "(nvalues=%d) — refusing to emit code with uncolored "
                  "values", f->name, max_iter, f->nvalues);
    }
}
