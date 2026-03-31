/* braun.c — Stack IR → IR3 with true Braun SSA construction.
 *
 * Implements Braun et al. 2013 SSA construction algorithm.
 * Phi nodes are constructed per basic block and then deconstructed
 * (converted to parallel copies) before register allocation.
 *
 * Key invariants:
 *   • Expression stack (vs_reg/vs_depth) is flushed at call sites.
 *   • Promotable LEAs: p->sym == ir_promote_sentinel.
 *   • Structural LEAs: p->sym == NULL.
 *   • Address-taken / aggregate LEAs: p->sym = variable-name string.
 *   • Promoted variables live ONLY in SSA vregs (no memory backing),
 *     except when spilled by irc_regalloc across call sites.
 *   • Non-promoted variables still go through IR3_LOAD / IR3_STORE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include "ir3.h"

/* Allocate a globally-unique label ID (shared counter with codegen.c). */
extern int new_label(void);

/* ================================================================
 * Virtual stack state (expression temporaries, reset per function)
 * ================================================================ */
#define MAX_VDEPTH 8

static int vs_size[MAX_VDEPTH];   /* push size in bytes (2 or 4) for each active depth */
static int vs_depth;              /* current virtual stack depth */
static int vsp;                   /* running (sp - bp) byte offset */

/* ================================================================
 * Promotion slot helpers
 * ================================================================ */
#define PROMOTE_BASE  128
#define PROMOTE_SLOTS 256                        /* bp offsets -128..+127 */
#define VSTACK_BASE   PROMOTE_SLOTS              /* virtual stack slots start here */
#define TOTAL_SLOTS   (PROMOTE_SLOTS + MAX_VDEPTH) /* promoted vars + vstack */

static bool promote_slot(int offset)
{
    int idx = offset + PROMOTE_BASE;
    return (unsigned)idx < PROMOTE_SLOTS;
}

/* Size (1/2/4) of each promoted slot, set on first access */
static int promo_slot_size[PROMOTE_SLOTS];

/* accum_offset: if non-zero, ACCUM holds the ADDRESS of the var at this bp-offset.
 * Cleared by ACCUM-modifying ops; preserved through PUSH. */
static int accum_offset;

/* vreg_lea_offset: tracks which bp-relative variable address a vreg holds
 * (set in IR_PUSH after a promotable LEA). 0 = not tracked.
 * Scratch-allocated per function; size = n_ir_nodes * 4 + 64. */
static int *vreg_lea_offset;
static int  vlo_size;

static int vlo_get(int vreg)
{
    int idx = vreg - IR3_VREG_BASE;
    if (vreg_lea_offset && (unsigned)idx < (unsigned)vlo_size)
        return vreg_lea_offset[idx];
    return 0;
}

static void vlo_set(int vreg, int val)
{
    int idx = vreg - IR3_VREG_BASE;
    if (vreg_lea_offset && (unsigned)idx < (unsigned)vlo_size)
        vreg_lea_offset[idx] = val;
}

/* ================================================================
 * Forward-branch vsp fixup
 * Scratch-allocated per function; size = label span + 1.
 * ================================================================ */
static int   lvsp_base;
static int  *lvsp_val;
static bool *lvsp_set;
static int   lvsp_alloc_size;

static void lvsp_reset(void)
{
    lvsp_base = -1;
    /* lvsp_val/lvsp_set are scratch-allocated; just clear the base */
}

static void lvsp_record(int label_id, int vsp_val_)
{
    if (lvsp_base < 0) lvsp_base = label_id;
    int idx = label_id - lvsp_base;
    /* Negative idx: back-edge to a label already emitted; no fixup needed. */
    if (idx < 0) return;
    if (!lvsp_set || idx >= lvsp_alloc_size) return;  /* prescan was conservative */
    if (!lvsp_set[idx]) {
        lvsp_val[idx] = vsp_val_;
        lvsp_set[idx] = true;
    }
}

static void lvsp_apply(int label_id)
{
    if (lvsp_base < 0 || !lvsp_set) return;
    int idx = label_id - lvsp_base;
    if (idx >= 0 && idx < lvsp_alloc_size && lvsp_set[idx])
        vsp = lvsp_val[idx];
}

/* ================================================================
 * Promoted parameter pre-seeding (deferred until after ENTER)
 * ================================================================ */
#define MAX_PARAM_SEEDS 32
static struct { int offset; int size; int vreg; } param_seeds[MAX_PARAM_SEEDS];
static int n_param_seeds;

static bool func_can_promote;        /* true if promotion is enabled for current function */

/* ================================================================
 * Output IR3 list
 * ================================================================ */
static IR3Inst  *ir3_head;
static IR3Inst **ir3_tail;
static IR3Inst  *ir3_last;   /* last emitted node */
static IR3Inst  *func_ir3_start; /* first IR3 node of current function */

static IR3Inst *new_ir3(IR3Op op)
{
    IR3Inst *s = scratch_alloc(sizeof(IR3Inst));
    s->op  = op;
    s->rd  = IR3_VREG_NONE;
    s->rs1 = IR3_VREG_NONE;
    s->rs2 = IR3_VREG_NONE;
    return s;
}

static IR3Inst *emit_ir3(IR3Op op)
{
    IR3Inst *s = new_ir3(op);
    *ir3_tail = s;
    ir3_tail  = &s->next;
    ir3_last  = s;
    return s;
}

/* Insert a new node AFTER 'after' and BEFORE 'after->next'. */
static IR3Inst *insert_after(IR3Inst *after, IR3Op op)
{
    IR3Inst *s = new_ir3(op);
    s->next = after->next;
    after->next = s;
    if (ir3_tail == &after->next) {
        ir3_last = s;
        ir3_tail = &s->next;
    }
    return s;
}

/* Forward declarations needed by flush_for_call_n */
static int readVariable(int bb_id, int var_slot);
static int current_bb_id;

/* ================================================================
 * flush_for_call_n — push argument vregs onto the machine stack before a call.
 *
 * Outer expression temporaries (at depths 0..first_arg-1) are NOT spilled
 * to memory here.  Their vregs remain live across the call in the Braun SSA
 * representation; IRC will assign physical registers and spill them if needed.
 * After flush, vs_depth is set to first_arg so that the outer temps remain
 * accessible via readVariable(current_bb_id, VSTACK_BASE + k).
 *
 * Returns the actual number of bytes allocated (may be larger than arg_bytes
 * due to alignment padding on CPU4).
 * ================================================================ */
static int flush_for_call_n(int arg_bytes, int line, int *out_arg_sum)
{
    if (vs_depth == 0) { if (out_arg_sum) *out_arg_sum = 0; return 0; }

    int n_args  = 0;
    int arg_sum = 0;
    for (int i = vs_depth - 1; i >= 0 && arg_sum + vs_size[i] <= arg_bytes; i--) {
        arg_sum += vs_size[i];
        n_args++;
    }
    int first_arg = vs_depth - n_args;

    int aligned_arg_sum = 0;
    if (arg_sum > 0) {
        if (g_target_arch == 4) {
            // Two-pass: simulate the store layout to compute actual space needed.
            // 4-byte args must be at 4-byte aligned offsets; simulate from offset 0.
            int sim = 0;
            for (int k = vs_depth - 1; k >= first_arg; k--) {
                if (vs_size[k] == 4 && (sim & 3))
                    sim = (sim + 4) & ~3;
                sim += vs_size[k];
            }
            aligned_arg_sum = (sim + 3) & ~3;
        } else {
            aligned_arg_sum = arg_sum;
        }
        IR3Inst *adj = emit_ir3(IR3_ADJ);
        adj->imm  = -aligned_arg_sum;
        adj->line = line;
        vsp -= aligned_arg_sum;

        // Compute store offsets respecting alignment for CPU4
        int boff = vsp;
        for (int k = vs_depth - 1; k >= first_arg; k--) {
            // CPU4: 4-byte stores must be at 4-byte aligned offsets
            if (g_target_arch == 4 && vs_size[k] == 4 && (boff & 3))
                boff = (boff + 4) & ~3;
            int vreg = readVariable(current_bb_id, VSTACK_BASE + k);
            IR3Inst *st = emit_ir3(IR3_STORE);
            st->rd   = IR3_VREG_BP;
            st->rs1  = vreg;
            st->imm  = boff;
            st->size = vs_size[k];
            st->line = line;
            boff += vs_size[k];
        }
    }

    /* Outer temps (0..first_arg-1) remain in SSA vregs — IRC handles live ranges.
     * Lower vs_depth to first_arg; outer slots still readable via readVariable. */
    vs_depth = first_arg;
    if (out_arg_sum) *out_arg_sum = arg_sum;
    return aligned_arg_sum;
}

/* ================================================================
 * Braun SSA state (per function)
 * All per-block and per-vreg arrays are scratch-allocated once per function
 * in init_fn_arrays() and reclaimed by scratch_reset() at TU end.
 * ================================================================ */

/* Per-block current definition: block_def[bb_id][slot] = vreg or IR3_VREG_NONE.
 * Slots 0..PROMOTE_SLOTS-1: promoted variable SSA defs (indexed by bp_offset + PROMOTE_BASE).
 * Slots VSTACK_BASE..TOTAL_SLOTS-1: virtual expression stack SSA defs (indexed by depth).
 * Pointer to a flat 2-D array; allocated as block_def[n_blocks_max][TOTAL_SLOTS]. */
static int      (*block_def)[TOTAL_SLOTS];

/* Phi node lookup: phi_node_map[vreg - IR3_VREG_BASE] = IR3Inst* or NULL */
static IR3Inst **phi_node_map;
static int      *phi_fwd;       /* phi_fwd[vreg - IR3_VREG_BASE] = simplified vreg, 0 = none */
static int       phi_map_size;  /* capacity of phi_node_map / phi_fwd */

/* Where to insert new phis at the head of each block */
static IR3Inst **phi_insert_point;

/* Pending phis for unsealed blocks (not yet inserted into IR3 list) */
static IR3Inst **pending_phis;

/* Last IR3 node emitted for each block */
static IR3Inst **block_last_ir3;

/* Last IR3 node before the terminator for each block (for phi deconstruction) */
static IR3Inst **block_pretail;

/* Incomplete phis for unsealed blocks */
typedef struct IncPhi {
    int bb_id;
    int var_slot;
    int phi_vreg;
    struct IncPhi *next;
} IncPhi;
static IncPhi *inc_phi_list;

/* How many predecessors of each block have been fully translated */
static int *preds_done;

/* n_blocks / blocks for current function */
static int cur_n_blocks;
static BB *cur_blocks;

/* RPO state */
static int  *rpo_order;
static int   rpo_count;
static bool *rpo_visited;

/* ================================================================
 * Braun SSA helpers
 * ================================================================ */

static int readVariableRecursive(int bb_id, int var_slot);
static void sealBlock(int bb_id);

static void writeVariable(int bb_id, int var_slot, int vreg)
{
    block_def[bb_id][var_slot] = vreg;
}

/* Set phi_insert_point[bb_id] = node and flush any pending phis for this block.
 * Called when a block's insertion point becomes known:
 *   - Entry block: after emitting the IR3_SYMLABEL
 *   - Labeled block: after emitting the IR3_LABEL
 *   - Unlabeled fall-through block: at the start of translate_bb
 */
static void set_phi_insert_point(int bb_id, IR3Inst *node)
{
    phi_insert_point[bb_id] = node;
    /* Flush pending phis in reverse order (they were prepended, so flush in LIFO) */
    IR3Inst *pend = pending_phis[bb_id];
    pending_phis[bb_id] = NULL;
    /* Reverse the pending list first (to maintain correct insertion order) */
    IR3Inst *reversed = NULL;
    while (pend) {
        IR3Inst *nxt = pend->next;
        pend->next = reversed;
        reversed = pend;
        pend = nxt;
    }
    /* Insert each pending phi after phi_insert_point */
    while (reversed) {
        IR3Inst *nxt = reversed->next;
        reversed->next = phi_insert_point[bb_id]->next;
        phi_insert_point[bb_id]->next = reversed;
        if (ir3_tail == &phi_insert_point[bb_id]->next) {
            ir3_last = reversed;
            ir3_tail = &reversed->next;
        }
        phi_insert_point[bb_id] = reversed;
        reversed = nxt;
    }
}

/* Insert a new IR3_PHI node into the IR3 list after phi_insert_point[bb_id].
 * If the block hasn't been translated yet, add to pending_phis instead. */
static IR3Inst *emit_phi_at(int bb_id, int phi_vreg)
{
    IR3Inst *phi = new_ir3(IR3_PHI);
    phi->rd = phi_vreg;
    phi->n_phi_ops = 0;
    phi->imm = bb_id;  /* owner block — used by deconstructPhis */

    /* Register in map */
    int idx = phi_vreg - IR3_VREG_BASE;
    if ((unsigned)idx < phi_map_size) phi_node_map[idx] = phi;

    IR3Inst *after = phi_insert_point[bb_id];
    if (!after) {
        /* Block not yet translated: queue as pending phi */
        phi->next = pending_phis[bb_id];
        pending_phis[bb_id] = phi;
        return phi;
    }

    /* Insert after phi_insert_point */
    phi->next = after->next;
    after->next = phi;
    if (ir3_tail == &after->next) {
        ir3_last = phi;
        ir3_tail = &phi->next;
    }
    phi_insert_point[bb_id] = phi;
    return phi;
}

/* Try to remove a trivial phi (all non-self ops are the same value). */
/* Chase the phi forwarding chain to find the final resolved vreg.
 * If vreg V was simplified to W, and W was simplified to X, resolve(V) = X. */
static int resolve_phi_vreg(int vreg)
{
    for (int depth = 0; depth < 64; depth++) {
        int idx = vreg - IR3_VREG_BASE;
        if ((unsigned)idx >= phi_map_size) return vreg;
        int fwd = phi_fwd[idx];
        if (fwd == 0 || fwd == vreg) return vreg;
        vreg = fwd;
    }
    return vreg;  /* safety: chain too long */
}

static int tryRemoveTrivialPhi(int phi_vreg)
{
    int idx = phi_vreg - IR3_VREG_BASE;
    if ((unsigned)idx >= phi_map_size) return phi_vreg;

    /* If this phi was already simplified, chase the forwarding chain */
    if (phi_fwd[idx] != 0 && phi_fwd[idx] != phi_vreg)
        return resolve_phi_vreg(phi_vreg);

    IR3Inst *phi = phi_node_map[idx];
    if (!phi || phi->op != IR3_PHI) return phi_vreg;

    int unique = IR3_VREG_NONE;
    for (int i = 0; i < phi->n_phi_ops; i++) {
        int op = phi->phi_ops[i];
        if (op == phi_vreg || op == IR3_VREG_NONE) continue;  /* self-ref or undef */
        if (op == unique) continue;
        if (unique != IR3_VREG_NONE) return phi_vreg;  /* 2+ distinct values */
        unique = op;
    }
    if (unique == IR3_VREG_NONE) unique = phi_vreg;  /* all self-refs: keep */

    if (unique == phi_vreg) return phi_vreg;  /* not trivial */

    /* Replace all uses of phi_vreg with unique in all phi nodes */
    for (int i = 0; i < phi_map_size; i++) {
        IR3Inst *p = phi_node_map[i];
        if (!p || p->op != IR3_PHI) continue;
        for (int j = 0; j < p->n_phi_ops; j++)
            if (p->phi_ops[j] == phi_vreg) p->phi_ops[j] = unique;
    }
    /* Also replace in pending phis */
    for (int b = 0; b < cur_n_blocks; b++) {
        for (IR3Inst *pp = pending_phis[b]; pp; pp = pp->next) {
            if (pp->op != IR3_PHI) continue;
            for (int j = 0; j < pp->n_phi_ops; j++)
                if (pp->phi_ops[j] == phi_vreg) pp->phi_ops[j] = unique;
        }
    }
    /* Replace in block_def (covers both promoted-var slots and vstack slots) */
    for (int b = 0; b < cur_n_blocks; b++)
        for (int s = 0; s < TOTAL_SLOTS; s++)
            if (block_def[b][s] == phi_vreg) block_def[b][s] = unique;

    /* Replace all uses of phi_vreg in already-emitted IR3 instructions.
     * This is necessary because readVariable may return a phi vreg that gets
     * embedded in rs1/rs2 of emitted instructions before the phi is eliminated.
     * Scoped to the current function (func_ir3_start) because vreg IDs are
     * reset per function and could collide with vregs from earlier functions. */
    for (IR3Inst *p = func_ir3_start; p; p = p->next) {
        if (p->rs1 == phi_vreg) p->rs1 = unique;
        if (p->rs2 == phi_vreg) p->rs2 = unique;
        if (p->rd  == phi_vreg) p->rd  = unique;
    }

    /* Mark phi dead and record forwarding */
    phi->op = IR3_MOV;
    phi->rd = IR3_VREG_NONE;  /* irc_regalloc will skip rd=NONE */
    phi->rs1 = unique;
    phi_fwd[idx] = unique;
    phi_node_map[idx] = NULL;

    /* Recursively try to simplify phis that used phi_vreg (now updated to unique) */
    for (int i = 0; i < phi_map_size; i++) {
        IR3Inst *p = phi_node_map[i];
        if (!p || p->op != IR3_PHI) continue;
        bool used = false;
        for (int j = 0; j < p->n_phi_ops; j++)
            if (p->phi_ops[j] == unique) { used = true; break; }
        if (used) tryRemoveTrivialPhi(IR3_VREG_BASE + i);
    }

    return unique;
}

static int readVariableRecursive(int bb_id, int var_slot)
{
    assert(bb_id >= 0 && bb_id < cur_n_blocks);
    BB *bb = &cur_blocks[bb_id];
    int val;

    if (!bb->sealed) {
        /* Unsealed block: create incomplete phi */
        val = ir3_new_vreg();
        emit_phi_at(bb_id, val);
        /* Record as incomplete */
        IncPhi *ip = scratch_alloc(sizeof(IncPhi));
        ip->bb_id = bb_id; ip->var_slot = var_slot; ip->phi_vreg = val;
        ip->next = inc_phi_list; inc_phi_list = ip;
    } else if (bb->n_preds == 0) {
        /* Entry block with no local definition: undefined variable.
         * Parameters (positive bp-offsets) should have been written via
         * translate_bb's IR_LEA+LOAD handling (writeVariable called there).
         * If we reach here for a parameter, it means the parameter was never
         * accessed in the entry block before this phi lookup.  Return NONE
         * to indicate "undefined from this predecessor"; the phi resolution
         * will handle it. */
        val = IR3_VREG_NONE;
        return val;
    } else if (bb->n_preds == 1) {
        /* Single predecessor: no phi needed */
        val = readVariable(bb->preds[0], var_slot);
    } else {
        /* Multiple predecessors: create phi, break cycles by writing first */
        val = ir3_new_vreg();
        writeVariable(bb_id, var_slot, val);
        emit_phi_at(bb_id, val);
        /* Fill operands */
        int phi_idx = val - IR3_VREG_BASE;
        IR3Inst *phi = ((unsigned)phi_idx < phi_map_size) ? phi_node_map[phi_idx] : NULL;
        /* Also check pending list */
        if (!phi || phi->op != IR3_PHI) {
            for (IR3Inst *pp = pending_phis[bb_id]; pp; pp = pp->next) {
                if (pp->op == IR3_PHI && pp->rd == val) { phi = pp; break; }
            }
        }
        if (phi && phi->op == IR3_PHI) {
            if (bb->n_preds > BB_MAX_PREDS) {
                fprintf(stderr, "braun: block %d has %d predecessors, exceeds BB_MAX_PREDS %d\n",
                        bb_id, bb->n_preds, BB_MAX_PREDS);
                exit(1);
            }
            phi->n_phi_ops = bb->n_preds;
            for (int i = 0; i < bb->n_preds; i++)
                phi->phi_ops[i] = readVariable(bb->preds[i], var_slot);
        }
        val = tryRemoveTrivialPhi(val);
    }
    writeVariable(bb_id, var_slot, val);
    return val;
}

static int readVariable(int bb_id, int var_slot)
{
    int v = block_def[bb_id][var_slot];
    if (v != IR3_VREG_NONE) {
        /* Chase forwarding chain in case this vreg was a simplified phi */
        int resolved = resolve_phi_vreg(v);
        if (resolved != v) {
            block_def[bb_id][var_slot] = resolved;  /* update in place */
            v = resolved;
        }
        return v;
    }
    v = readVariableRecursive(bb_id, var_slot);
    /* Chase forwarding chain for the recursive result too */
    int resolved = resolve_phi_vreg(v);
    if (resolved != v) {
        block_def[bb_id][var_slot] = resolved;
        v = resolved;
    }
    return v;
}

static void sealBlock(int bb_id)
{
    assert(bb_id >= 0 && bb_id < cur_n_blocks);
    assert(!cur_blocks[bb_id].sealed);
    BB *bb = &cur_blocks[bb_id];

    /* Fill all incomplete phis for this block */
    for (IncPhi *ip = inc_phi_list; ip; ip = ip->next) {
        if (ip->bb_id != bb_id) continue;
        int phi_idx = ip->phi_vreg - IR3_VREG_BASE;
        IR3Inst *phi = ((unsigned)phi_idx < phi_map_size) ? phi_node_map[phi_idx] : NULL;
        /* Also check pending list */
        if (!phi || phi->op != IR3_PHI) {
            for (IR3Inst *pp = pending_phis[bb_id]; pp; pp = pp->next) {
                if (pp->op == IR3_PHI && pp->rd == ip->phi_vreg) { phi = pp; break; }
            }
        }
        if (phi && phi->op == IR3_PHI) {
            if (bb->n_preds > BB_MAX_PREDS) {
                fprintf(stderr, "braun: block %d has %d predecessors, exceeds BB_MAX_PREDS %d\n",
                        bb_id, bb->n_preds, BB_MAX_PREDS);
                exit(1);
            }
            phi->n_phi_ops = bb->n_preds;
            for (int i = 0; i < bb->n_preds; i++)
                phi->phi_ops[i] = readVariable(bb->preds[i], ip->var_slot);
        }
        int new_val = tryRemoveTrivialPhi(ip->phi_vreg);
        if (block_def[bb_id][ip->var_slot] == ip->phi_vreg)
            block_def[bb_id][ip->var_slot] = new_val;
    }
    cur_blocks[bb_id].sealed = true;
}

static void reset_braun_state(void)
{
    /* Per-block and per-vreg arrays are scratch-allocated per function.
     * Just null the pointers; memory is reclaimed by scratch_reset at TU end. */
    block_def        = NULL;
    phi_node_map     = NULL;
    phi_fwd          = NULL;
    phi_map_size     = 0;
    phi_insert_point = NULL;
    pending_phis     = NULL;
    block_last_ir3   = NULL;
    block_pretail    = NULL;
    preds_done       = NULL;
    rpo_order        = NULL;
    rpo_visited      = NULL;
    vreg_lea_offset  = NULL;
    vlo_size         = 0;
    lvsp_val         = NULL;
    lvsp_set         = NULL;
    lvsp_alloc_size  = 0;
    memset(promo_slot_size, 0, sizeof(promo_slot_size));
    current_bb_id = 0;
    cur_n_blocks  = 0;
    cur_blocks    = NULL;
    accum_offset  = 0;
    inc_phi_list  = NULL;
}

/* Allocate all per-function scratch arrays.
 * n          = max number of blocks (n_orig + n_crit critical landing pads)
 * phi_sz     = capacity for phi_node_map / phi_fwd (upper bound on vregs)
 * vlo_sz     = capacity for vreg_lea_offset (same bound)
 * lvsp_sz    = capacity for lvsp arrays (label span + 1)
 */
static void init_fn_arrays(int n, int phi_sz, int vlo_sz, int lvsp_sz)
{
    block_def        = scratch_alloc((size_t)n * TOTAL_SLOTS * sizeof(int));
    phi_insert_point = scratch_alloc((size_t)n * sizeof(IR3Inst *));
    pending_phis     = scratch_alloc((size_t)n * sizeof(IR3Inst *));
    block_last_ir3   = scratch_alloc((size_t)n * sizeof(IR3Inst *));
    block_pretail    = scratch_alloc((size_t)n * sizeof(IR3Inst *));
    preds_done       = scratch_alloc((size_t)n * sizeof(int));
    rpo_order        = scratch_alloc((size_t)n * sizeof(int));
    rpo_visited      = scratch_alloc((size_t)n * sizeof(bool));
    /* block_def must be initialised to IR3_VREG_NONE (-1); others are fine at zero */
    memset(block_def, -1, (size_t)n * TOTAL_SLOTS * sizeof(int));

    phi_map_size = phi_sz;
    phi_node_map = scratch_alloc((size_t)phi_sz * sizeof(IR3Inst *));
    phi_fwd      = scratch_alloc((size_t)phi_sz * sizeof(int));

    vlo_size        = vlo_sz;
    vreg_lea_offset = scratch_alloc((size_t)vlo_sz * sizeof(int));

    lvsp_alloc_size = lvsp_sz;
    lvsp_val        = scratch_alloc((size_t)lvsp_sz * sizeof(int));
    lvsp_set        = scratch_alloc((size_t)lvsp_sz * sizeof(bool));
}

/* ================================================================
 * RPO computation
 * ================================================================ */
static void dfs_rpo(int b)
{
    rpo_visited[b] = true;
    /* Visit successors in REVERSE order so that the fall-through edge
     * (succs[0]) is explored last → appears first in RPO, immediately
     * after the branching block (no extra jump needed). */
    for (int i = cur_blocks[b].n_succs - 1; i >= 0; i--) {
        int s = cur_blocks[b].succs[i];
        if (!rpo_visited[s]) dfs_rpo(s);
    }
    rpo_order[rpo_count++] = b;
}

static void compute_rpo(int n_blocks)
{
    memset(rpo_visited, 0, n_blocks * sizeof(bool));
    rpo_count = 0;
    if (n_blocks > 0) dfs_rpo(0);
    /* Reverse post-order */
    for (int i = 0, j = rpo_count - 1; i < j; i++, j--) {
        int tmp = rpo_order[i]; rpo_order[i] = rpo_order[j]; rpo_order[j] = tmp;
    }
}

/* ================================================================
 * translate_bb — translate one BB's stack IR to IR3
 * ================================================================ */
static void translate_bb(BB *bb, int bb_id)
{
    int local_accum_offset = 0;

    /* For unlabeled fall-through blocks (no IR_LABEL instruction), set the phi
     * insert point now (to the current ir3_last) and flush any pending phis.
     * For labeled blocks, phi_insert_point is set (and pending phis flushed)
     * when the IR_LABEL instruction is emitted below.
     * Entry block (bb_id==0) has phi_insert_point already set before RPO loop. */
    if (bb_id != 0 && cur_blocks[bb_id].label_id < 0 && !phi_insert_point[bb_id]) {
        /* Unlabeled block: phi insert point = current IR3 tail */
        set_phi_insert_point(bb_id, ir3_last);
    }

    IRInst *end = bb->ir_last ? bb->ir_last->next : NULL;
    for (IRInst *p = bb->ir_first ? bb->ir_first->next : NULL;
         p && p != end;
         p = p->next)
    {
        IR3Inst *s = NULL;

        switch (p->op) {
        case IR_NOP:
        case IR_BB_START:
            break;

        case IR_IMM:
            s = emit_ir3(IR3_CONST);
            s->rd  = IR3_VREG_ACCUM;
            s->imm = p->operand;
            s->sym = p->sym;
            s->line = p->line;
            local_accum_offset = 0;
            break;

        case IR_LEA: {
            /* Promotion: scalars that are not address-taken can be promoted
             * to SSA registers.  The ir_promote_sentinel tag is set by
             * gen_varaddr_from_ident() in codegen.c for eligible locals.
             * With linear-scan + spilling, promoted variables that exceed
             * register pressure are automatically spilled to stack slots.
             *
             * Future work: add heuristics to promote only profitable
             * variables (loop counters, frequently-read scalars). */
            bool is_promo = (func_can_promote && p->sym == ir_promote_sentinel);

            /* Peek: if next instruction is a load, collapse to bp-relative load */
            int peek_sz = 0;
            if (p->next && p->next != end) {
                if      (p->next->op == IR_LW) peek_sz = 2;
                else if (p->next->op == IR_LB) peek_sz = 1;
                else if (p->next->op == IR_LL) peek_sz = 4;
            }

            if (peek_sz && is_promo) {
                /* Promotable read: use SSA definition */
                int N    = p->operand;
                int slot = N + PROMOTE_BASE;
                if ((unsigned)slot < PROMOTE_SLOTS)
                    promo_slot_size[slot] = peek_sz;
                int val  = IR3_VREG_NONE;
                if ((unsigned)slot < PROMOTE_SLOTS)
                    val = readVariable(bb_id, slot);
                if (val != IR3_VREG_NONE && val != IR3_VREG_NONE) {
                    /* Hit: variable in a register — emit MOV to ACCUM */
                    IR3Inst *mv = emit_ir3(IR3_MOV);
                    mv->rd   = IR3_VREG_ACCUM;
                    mv->rs1  = val;
                    mv->line = p->line;
                } else {
                    /* Miss: load from memory, record as SSA def */
                    int fresh = ir3_new_vreg();
                    IR3Inst *ld = emit_ir3(IR3_LOAD);
                    ld->rd   = fresh;
                    ld->rs1  = IR3_VREG_BP;
                    ld->imm  = N;
                    ld->size = peek_sz;
                    ld->sym  = p->sym;
                    ld->line = p->line;
                    if ((unsigned)slot < PROMOTE_SLOTS)
                        writeVariable(bb_id, slot, fresh);
                    IR3Inst *mv = emit_ir3(IR3_MOV);
                    mv->rd   = IR3_VREG_ACCUM;
                    mv->rs1  = fresh;
                    mv->line = p->line;
                }
                local_accum_offset = 0;
                p = p->next;  /* consume the load */
                block_last_ir3[bb_id] = ir3_last;
                break;
            }

            if (peek_sz) {
                /* Non-promotable LEA+LOAD: bp-relative load */
                s = emit_ir3(IR3_LOAD);
                s->rd   = IR3_VREG_ACCUM;
                s->rs1  = IR3_VREG_BP;
                s->imm  = p->operand;
                s->size = peek_sz;
                s->sym  = p->sym;
                s->line = p->line;
                local_accum_offset = 0;
                p = p->next;
                break;
            }

            /* Standalone LEA */
            if (is_promo && promote_slot(p->operand)) {
                /* Promotable address: don't emit IR3_LEA — the address is only
                 * needed for a later store which will be promoted (no IR3_STORE).
                 * Just record the offset for the promoted store path. */
                local_accum_offset = p->operand;
            } else {
                s = emit_ir3(IR3_LEA);
                s->rd   = IR3_VREG_ACCUM;
                s->imm  = p->operand;
                s->line = p->line;
                local_accum_offset = 0;
            }
            break;
        }

        case IR_LW: case IR_LB: case IR_LL: {
            int sz = (p->op == IR_LW) ? 2 : (p->op == IR_LB) ? 1 : 4;

            if (local_accum_offset != 0 && promote_slot(local_accum_offset)) {
                int slot = local_accum_offset + PROMOTE_BASE;
                int val  = readVariable(bb_id, slot);
                if (val != IR3_VREG_NONE) {
                    IR3Inst *mv = emit_ir3(IR3_MOV);
                    mv->rd   = IR3_VREG_ACCUM;
                    mv->rs1  = val;
                    mv->line = p->line;
                } else {
                    /* No SSA def yet: load from memory, record as def.
                     * Use bp-relative addressing (the LEA was suppressed). */
                    int fresh = ir3_new_vreg();
                    s = emit_ir3(IR3_LOAD);
                    s->rd   = fresh;
                    s->rs1  = IR3_VREG_BP;
                    s->imm  = local_accum_offset;
                    s->size = sz;
                    s->line = p->line;
                    writeVariable(bb_id, slot, fresh);
                    IR3Inst *mv = emit_ir3(IR3_MOV);
                    mv->rd   = IR3_VREG_ACCUM;
                    mv->rs1  = fresh;
                    mv->line = p->line;
                }
                local_accum_offset = 0;
                block_last_ir3[bb_id] = ir3_last;
                break;
            }

            /* Standard load */
            s = emit_ir3(IR3_LOAD);
            s->rd   = IR3_VREG_ACCUM;
            s->rs1  = IR3_VREG_ACCUM;
            s->imm  = 0;
            s->size = sz;
            s->line = p->line;
            local_accum_offset = 0;
            break;
        }

        case IR_PUSH: case IR_PUSHW: {
            int sz    = (p->op == IR_PUSH) ? 4 : 2;
            int fresh;
            if (local_accum_offset != 0 && promote_slot(local_accum_offset)) {
                /* Promoted address push: no IR3_MOV needed — the address is only
                 * used as a store target.  Use a dummy vreg with vlo tracking. */
                fresh = ir3_new_vreg();
                vlo_set(fresh, local_accum_offset);
            } else {
                fresh = ir3_new_vreg();
                IR3Inst *mov = emit_ir3(IR3_MOV);
                mov->rd   = fresh;
                mov->rs1  = IR3_VREG_ACCUM;
                mov->line = p->line;
            }
            /* accum_offset preserved through PUSH */
            if (vs_depth >= MAX_VDEPTH)
                fprintf(stderr, "braun: virtual stack overflow at depth %d\n", vs_depth);
            writeVariable(current_bb_id, VSTACK_BASE + vs_depth, fresh);
            vs_size[vs_depth] = sz;
            vs_depth++;
            break;
        }

        case IR_POP: case IR_POPW:
            if (vs_depth > 0) {
                vs_depth--;
                s = emit_ir3(IR3_MOV);
                s->rd   = IR3_VREG_ACCUM;
                s->rs1  = readVariable(current_bb_id, VSTACK_BASE + vs_depth);
                s->line = p->line;
                local_accum_offset = 0;
            }
            break;

        case IR_SW: case IR_SB: case IR_SL: {
            if (vs_depth > 0) {
                int sz = (p->op == IR_SW) ? 2 : (p->op == IR_SB) ? 1 : 4;
                vs_depth--;
                int addr_vreg = readVariable(current_bb_id, VSTACK_BASE + vs_depth);
                int N = vlo_get(addr_vreg);

                if (N != 0 && promote_slot(N)) {
                    /* Promoted store: update SSA def, do NOT emit IR3_STORE */
                    int slot = N + PROMOTE_BASE;
                    if ((unsigned)slot < PROMOTE_SLOTS)
                        promo_slot_size[slot] = sz;
                    int fresh = ir3_new_vreg();
                    IR3Inst *mv = emit_ir3(IR3_MOV);
                    mv->rd   = fresh;
                    mv->rs1  = IR3_VREG_ACCUM;
                    mv->line = p->line;
                    writeVariable(bb_id, slot, fresh);
                } else {
                    /* Non-promoted: emit memory store */
                    s = emit_ir3(IR3_STORE);
                    s->rd   = addr_vreg;
                    s->rs1  = IR3_VREG_ACCUM;
                    s->imm  = 0;
                    s->size = sz;
                    s->line = p->line;
                }
            }
            /* ACCUM and accum_offset unchanged by store */
            break;
        }

        /* Binary ALU ops */
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_AND: case IR_OR:  case IR_XOR: case IR_SHL: case IR_SHR:
        case IR_EQ:  case IR_NE:  case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:
        case IR_LTS: case IR_LES: case IR_GTS: case IR_GES:
        case IR_DIVS: case IR_MODS: case IR_SHRS:
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
        case IR_FLT:  case IR_FLE:  case IR_FGT:  case IR_FGE:
            if (vs_depth > 0) {
                vs_depth--;
                s = emit_ir3(IR3_ALU);
                s->alu_op = p->op;
                s->rd     = IR3_VREG_ACCUM;
                s->rs1    = readVariable(current_bb_id, VSTACK_BASE + vs_depth);
                s->rs2    = IR3_VREG_ACCUM;
                s->line   = p->line;
                local_accum_offset = 0;
            }
            break;

        case IR_SXB: case IR_SXW: case IR_ITOF: case IR_FTOI:
            s = emit_ir3(IR3_ALU1);
            s->alu_op = p->op;
            s->rd     = IR3_VREG_ACCUM;
            s->rs1    = IR3_VREG_ACCUM;
            s->line   = p->line;
            local_accum_offset = 0;
            break;

        case IR_JL: case IR_JLI: {
            int arg_bytes = 0;
            for (IRInst *q = p->next; q; q = q->next) {
                if (q->op == IR_NOP || q->op == IR_BB_START) continue;
                if (q->op == IR_ADJ && q->operand > 0) { arg_bytes = q->operand; break; }
                if (q->op == IR_JL || q->op == IR_JLI || q->op == IR_RET ||
                    q->op == IR_J  || q->op == IR_JZ  || q->op == IR_JNZ) break;
            }

            bool save_fp = (p->op == IR_JLI && vs_depth > 0);
            int  fp_vreg = IR3_VREG_NONE;
            if (save_fp) {
                fp_vreg = ir3_new_vreg();
                IR3Inst *mv = emit_ir3(IR3_MOV);
                mv->rd   = fp_vreg;
                mv->rs1  = IR3_VREG_ACCUM;
                mv->line = p->line;
            }

            int actual_arg_sum = 0;
            int aligned_bytes = flush_for_call_n(arg_bytes, p->line, &actual_arg_sum);
            // Update the cleanup ADJ for CPU4: the scanned arg_bytes may include
            // scope-exit adj bytes merged in by the peephole (e.g. arg_cleanup 2 +
            // scope_cleanup 4 → 6).  The correct post-call adj is:
            //   aligned_bytes + (arg_bytes - actual_arg_sum)
            // where the second term carries the merged-in scope cleanup unchanged.
            if (g_target_arch == 4) {
                int extra = arg_bytes - actual_arg_sum;
                int new_cleanup = aligned_bytes + extra;
                if (new_cleanup != arg_bytes) {
                    for (IRInst *q = p->next; q; q = q->next) {
                        if (q->op == IR_NOP || q->op == IR_BB_START) continue;
                        if (q->op == IR_ADJ && q->operand > 0) {
                            q->operand = new_cleanup;
                            break;
                        }
                        if (q->op == IR_JL || q->op == IR_JLI || q->op == IR_RET ||
                            q->op == IR_J  || q->op == IR_JZ  || q->op == IR_JNZ) break;
                    }
                }
            }

            if (save_fp) {
                IR3Inst *mv = emit_ir3(IR3_MOV);
                mv->rd   = IR3_VREG_ACCUM;
                mv->rs1  = fp_vreg;
                mv->line = p->line;
            }

            if (p->op == IR_JL) {
                s = emit_ir3(IR3_CALL);
                s->sym = p->sym;
            } else {
                s = emit_ir3(IR3_CALLR);
            }
            s->line = p->line;

            local_accum_offset = 0;
            break;
        }

        case IR_ENTER:
            vsp = -(int)p->operand;
            s = emit_ir3(IR3_ENTER);
            s->imm  = p->operand;
            s->line = p->line;
            /* After emitting ENTER, emit deferred parameter seed loads and
             * move phi_insert_point[0] past them so that any phis inserted
             * later by readVariableRecursive appear after all seed loads. */
            if (bb_id == 0) {
                for (int pi = 0; pi < n_param_seeds; pi++) {
                    IR3Inst *ld = emit_ir3(IR3_LOAD);
                    ld->rd   = param_seeds[pi].vreg;
                    ld->rs1  = IR3_VREG_BP;
                    ld->imm  = param_seeds[pi].offset;
                    ld->size = param_seeds[pi].size;
                    ld->line = p->line;
                }
                phi_insert_point[0] = ir3_last;
            }
            break;

        case IR_ADJ:
            vsp += p->operand;
            s = emit_ir3(IR3_ADJ);
            s->imm  = p->operand;
            s->line = p->line;
            break;

        case IR_RET:
            block_pretail[bb_id] = ir3_last;
            emit_ir3(IR3_RET)->line = p->line;
            break;

        case IR_J: {
            lvsp_record(p->operand, vsp);
            block_pretail[bb_id] = ir3_last;
            s = emit_ir3(IR3_J);
            s->imm  = p->operand;
            s->line = p->line;
            local_accum_offset = 0;
            break;
        }

        case IR_JZ: case IR_JNZ: {
            lvsp_record(p->operand, vsp);
            block_pretail[bb_id] = ir3_last;
            s = emit_ir3(p->op == IR_JZ ? IR3_JZ : IR3_JNZ);
            s->rs1  = IR3_VREG_ACCUM;
            s->imm  = p->operand;
            s->line = p->line;
            local_accum_offset = 0;
            break;
        }

        case IR_LABEL: {
            lvsp_apply(p->operand);
            s = emit_ir3(IR3_LABEL);
            s->imm  = p->operand;
            s->line = p->line;
            /* Set phi insert point AFTER the label and flush pending phis */
            if (!phi_insert_point[bb_id])
                set_phi_insert_point(bb_id, s);
            break;
        }

        case IR_SYMLABEL:
            /* Should not appear inside a BB */
            break;

        case IR_WORD:
            s = emit_ir3(IR3_WORD);
            s->imm  = p->operand;
            s->sym  = p->sym;
            s->line = p->line;
            break;

        case IR_BYTE:
            s = emit_ir3(IR3_BYTE);
            s->imm  = p->operand;
            s->line = p->line;
            break;

        case IR_ALIGN:
            emit_ir3(IR3_ALIGN)->line = p->line;
            break;


        case IR_PUTCHAR:
            emit_ir3(IR3_PUTCHAR)->line = p->line;
            break;

        case IR_COMMENT:
            s = emit_ir3(IR3_COMMENT);
            s->sym  = p->sym;
            s->line = p->line;
            break;

        default:
            fprintf(stderr, "braun: unhandled IR op %d in bb %d\n", (int)p->op, bb_id);
            break;
        }

        block_last_ir3[bb_id] = ir3_last;
    }

    /* Ensure block_last_ir3 is set even for empty blocks (e.g. unlabeled
     * fall-through blocks).  Without this, insert_phi_copy silently drops
     * phi copies destined for empty predecessor blocks. */
    if (!block_last_ir3[bb_id])
        block_last_ir3[bb_id] = ir3_last;
}

/* ================================================================
 * Critical-edge splitting
 *
 * A critical edge goes from a block with >1 successors to a block
 * with >1 predecessors.  Phi copies inserted into such a predecessor
 * would execute on ALL outgoing paths, not just the one to the merge
 * block, silently corrupting the other successor's values.
 *
 * Fix: insert an empty landing-pad block on every critical edge.
 * The landing pad has a single predecessor (the original pred) and
 * a single successor (the original succ), so phi copies placed there
 * execute on exactly one path.
 *
 * Called after all blocks are translated and before deconstructPhis.
 * ================================================================ */
static void split_critical_edges(BB **blocks_ptr, int *n_blocks_ptr)
{
    BB  *blocks = *blocks_ptr;
    int  n_orig = *n_blocks_ptr;

    /* Count critical edges first so we can realloc once. */
    int n_crit = 0;
    for (int p = 0; p < n_orig; p++) {
        if (blocks[p].n_succs <= 1) continue;
        for (int si = 0; si < blocks[p].n_succs; si++) {
            int s = blocks[p].succs[si];
            if (blocks[s].n_preds > 1) n_crit++;
        }
    }
    if (n_crit == 0) return;

    BB *new_blocks = scratch_alloc((n_orig + n_crit) * sizeof(BB));
    memcpy(new_blocks, blocks, n_orig * sizeof(BB));
    /* new portion already zeroed by scratch_alloc; old blocks abandoned in scratch */
    blocks = new_blocks;
    *blocks_ptr = blocks;

    /* Split each critical edge.  Iterate only original blocks so landing
     * pads (which have n_succs=1) are never re-processed. */
    int n_blocks = n_orig;
    for (int pred = 0; pred < n_orig; pred++) {
        if (blocks[pred].n_succs <= 1) continue;
        for (int si = 0; si < blocks[pred].n_succs; si++) {
            int succ = blocks[pred].succs[si];
            if (blocks[succ].n_preds <= 1) continue;

            /* --- Critical edge pred → succ --- */
            int lbl_new = new_label();
            int landing = n_blocks++;

            /* Landing-pad BB: one pred, one succ. */
            blocks[landing].id       = landing;
            blocks[landing].label_id = lbl_new;
            blocks[landing].n_preds  = 1;
            blocks[landing].preds[0] = pred;
            blocks[landing].n_succs  = 1;
            blocks[landing].succs[0] = succ;
            blocks[landing].sealed   = true;

            /* Rewire: pred → landing, succ replaces pred with landing. */
            blocks[pred].succs[si] = landing;
            for (int pi = 0; pi < blocks[succ].n_preds; pi++) {
                if (blocks[succ].preds[pi] == pred) {
                    blocks[succ].preds[pi] = landing;
                    break;
                }
            }

            /* Retarget pred's JZ/JNZ from succ.label_id → lbl_new.
             * Start from block_pretail[pred]->next (or block_last_ir3 when
             * pretail is NULL), then skip any dead phi→MOV remnants
             * (IR3_MOV with rd==IR3_VREG_NONE) to reach the real terminator. */
            IR3Inst *term = block_pretail[pred] ? block_pretail[pred]->next
                                                 : block_last_ir3[pred];
            while (term && term->op == IR3_MOV && term->rd == IR3_VREG_NONE)
                term = term->next;
            if (term && (term->op == IR3_JZ || term->op == IR3_JNZ)
                     && term->imm == blocks[succ].label_id) {
                term->imm = lbl_new;
            } else {
                /* It's a fall-through edge: the conditional branches to the
                 * OTHER successor, and execution falls through into succ.
                 * Insert an explicit J to the landing pad immediately after
                 * the conditional branch so the fall-through is intercepted. */
                IR3Inst *j_fall = new_ir3(IR3_J);
                j_fall->imm = lbl_new;

                IR3Inst *after_this = block_last_ir3[pred];
                j_fall->next = after_this->next;
                after_this->next = j_fall;
                if (ir3_tail == &after_this->next) {
                    ir3_tail = &j_fall->next;
                    ir3_last = j_fall;
                }

                /* Update trackers so deconstructPhis sees the correct tail. */
                block_pretail[pred] = after_this;
                block_last_ir3[pred] = j_fall;
            }

            /* Append the landing pad at the END of the function's IR3 list:
             *
             *   lbl_new:            ← IR3_LABEL  (block_pretail[landing])
             *   [phi copies go here, inserted later by deconstructPhis]
             *   j succ.label_id     ← IR3_J      (block_last_ir3[landing])
             *
             * Placing it at the end (rather than before succ's label) ensures
             * it is only reachable via the retargeted jump — fall-through paths
             * from other predecessors of succ reach succ's label directly and
             * never execute the landing-pad copies. */
            IR3Inst *lnode = new_ir3(IR3_LABEL);
            lnode->imm = lbl_new;
            *ir3_tail  = lnode;
            ir3_tail   = &lnode->next;
            ir3_last   = lnode;

            IR3Inst *jnode = new_ir3(IR3_J);
            jnode->imm = blocks[succ].label_id;
            *ir3_tail  = jnode;
            ir3_tail   = &jnode->next;
            ir3_last   = jnode;

            /* deconstructPhis will call insert_phi_copy(landing, ...) which
             * inserts copies after block_pretail[landing] (= lnode), stacking
             * them between the label and the J. */
            block_pretail[landing]  = lnode;
            block_last_ir3[landing] = jnode;
        }
    }

    *n_blocks_ptr = n_blocks;
}

/* ================================================================
 * Phi deconstruction
 *
 * For each surviving IR3_PHI node, insert a parallel copy at the
 * tail of each predecessor block (just before the terminator).
 * Then mark the phi node dead.
 * ================================================================ */

/* CopyPair for parallel copy sequencing */
typedef struct { int dst; int src; } CopyPair;

/* Insert IR3_MOV dst, src at the tail of block pred_id (just before terminator).
 * The insertion point is block_pretail[pred_id]; if NULL, use block_last_ir3. */
static void insert_phi_copy(int pred_id, int dst, int src, int line)
{
    if (dst == src) return;  /* identity: skip */

    IR3Inst *after = block_pretail[pred_id];
    if (!after) after = block_last_ir3[pred_id];
    if (!after) return;  /* empty block — shouldn't happen */

    IR3Inst *mv = new_ir3(IR3_MOV);
    mv->rd   = dst;
    mv->rs1  = src;
    mv->line = line;
    mv->next = after->next;
    after->next = mv;
    if (ir3_tail == &after->next) {
        ir3_last = mv;
        ir3_tail = &mv->next;
    }
    /* Update the pretail tracker so subsequent insertions stack before the terminator */
    block_pretail[pred_id] = mv;
}

static void deconstructPhis(BB *blocks, int n_blocks)
{
    /* Pass 1: collect all (pred, dst, src) copy pairs and mark phis dead.
     * Using phi->imm as the owner bb_id (stored by emit_phi_at). */
    typedef struct { int pred; int dst; int src; int line; } PhiCopyRec;

    /* Compute upper bound for copy pairs: sum of n_preds over live phi nodes. */
    int max_copies = 0;
    for (int i = 0; i < phi_map_size; i++) {
        IR3Inst *phi = phi_node_map[i];
        if (!phi || phi->op != IR3_PHI || phi->rd == IR3_VREG_NONE) continue;
        int ob = phi->imm;
        if (ob >= 0 && ob < n_blocks) max_copies += blocks[ob].n_preds;
    }
    max_copies += n_blocks + 8;  /* slack */
    PhiCopyRec *all_copies = scratch_alloc((size_t)max_copies * sizeof(PhiCopyRec));
    int *cps_dst  = scratch_alloc((size_t)max_copies * sizeof(int));
    int *cps_src  = scratch_alloc((size_t)max_copies * sizeof(int));
    int *cps_line = scratch_alloc((size_t)max_copies * sizeof(int));
    int *done     = scratch_alloc((size_t)max_copies * sizeof(int));

    int n_all = 0;

    for (int i = 0; i < phi_map_size; i++) {
        IR3Inst *phi = phi_node_map[i];
        if (!phi || phi->op != IR3_PHI) continue;
        if (phi->rd == IR3_VREG_NONE) continue;

        int dst      = phi->rd;
        int owner_bb = phi->imm;  /* set by emit_phi_at (Change 1) */
        int line     = phi->line;

        /* Fallback: scan IR if owner not recorded (shouldn't happen) */
        if (owner_bb < 0 || owner_bb >= n_blocks) {
            owner_bb = -1;
            int cur = -1;
            for (IR3Inst *q = func_ir3_start; q; q = q->next) {
                if (q->op == IR3_SYMLABEL) { cur = 0; }
                else if (q->op == IR3_LABEL) {
                    int lbl = q->imm;
                    for (int b = 0; b < n_blocks; b++)
                        if (blocks[b].label_id == lbl) { cur = b; break; }
                } else if (q == phi) { owner_bb = cur; break; }
            }
        }

        /* Mark phi dead before collecting copies */
        phi->op  = IR3_MOV;
        phi->rd  = IR3_VREG_NONE;
        phi->rs1 = IR3_VREG_NONE;
        phi_node_map[i] = NULL;

        if (owner_bb < 0 || owner_bb >= n_blocks) continue;

        BB *bb = &blocks[owner_bb];
        for (int pi = 0; pi < bb->n_preds && pi < phi->n_phi_ops; pi++) {
            int pred = bb->preds[pi];
            int src  = phi->phi_ops[pi];
            if (src == IR3_VREG_NONE || dst == src) continue;
            if (n_all < max_copies) {
                all_copies[n_all].pred = pred;
                all_copies[n_all].dst  = dst;
                all_copies[n_all].src  = src;
                all_copies[n_all].line = line;
                n_all++;
            }
        }
    }

    /* Pass 2: for each predecessor block P, sequentialize its copy set
     * (topological sort + temp-vreg cycle breaking) then insert. */
    for (int p = 0; p < n_blocks; p++) {
        /* Gather copies sourced from predecessor p */
        int n_cps = 0;
        for (int i = 0; i < n_all; i++) {
            if (all_copies[i].pred == p) {
                cps_dst[n_cps]  = all_copies[i].dst;
                cps_src[n_cps]  = all_copies[i].src;
                cps_line[n_cps] = all_copies[i].line;
                n_cps++;
            }
        }
        if (n_cps == 0) continue;

        /* Sequentialize: emit copies whose dst is not another undone copy's src.
         * On cycle: save the cycle-entry dst to a fresh vreg and redirect readers. */
        memset(done, 0, n_cps * sizeof(int));
        int emitted = 0;

        while (emitted < n_cps) {
            int progress = 0;
            for (int i = 0; i < n_cps; i++) {
                if (done[i]) continue;
                int blocked = 0;
                for (int j = 0; j < n_cps; j++) {
                    if (!done[j] && j != i && cps_src[j] == cps_dst[i]) {
                        blocked = 1; break;
                    }
                }
                if (!blocked) {
                    insert_phi_copy(p, cps_dst[i], cps_src[i], cps_line[i]);
                    done[i] = 1; emitted++; progress = 1;
                }
            }
            if (!progress) {
                /* Cycle: save first undone copy's dst in a temp vreg,
                 * redirect every undone copy that reads that dst to read tmp. */
                int i;
                for (i = 0; i < n_cps; i++) if (!done[i]) break;
                int tmp = ir3_new_vreg();
                insert_phi_copy(p, tmp, cps_dst[i], cps_line[i]);
                for (int j = 0; j < n_cps; j++) {
                    if (!done[j] && cps_src[j] == cps_dst[i])
                        cps_src[j] = tmp;
                }
                /* cps[i] is now unblocked on the next iteration */
            }
        }
    }
}

/* ================================================================
 * Data section pass-through
 * ================================================================ */
static IRInst *emit_data_node(IRInst *p)
{
    IR3Inst *s = NULL;
    switch (p->op) {
    case IR_LABEL:
        s = emit_ir3(IR3_LABEL);
        s->imm = p->operand; s->line = p->line;
        break;
    case IR_WORD:
        s = emit_ir3(IR3_WORD);
        s->imm = p->operand; s->sym = p->sym; s->line = p->line;
        break;
    case IR_BYTE:
        s = emit_ir3(IR3_BYTE);
        s->imm = p->operand; s->line = p->line;
        break;
    case IR_ALIGN:
        s = emit_ir3(IR3_ALIGN); s->line = p->line;
        break;
    case IR_PUTCHAR:
        s = emit_ir3(IR3_PUTCHAR); s->line = p->line;
        break;
    case IR_COMMENT:
        s = emit_ir3(IR3_COMMENT); s->sym = p->sym; s->line = p->line;
        break;
    case IR_NOP:
    case IR_BB_START:
        break;
    default:
        /* Unexpected node in data area: emit as-is if possible */
        break;
    }
    (void)s;
    return p->next;
}

/* ================================================================
 * is_function_symlabel — detect function vs data label
 * ================================================================ */
static bool is_function_symlabel(IRInst *sym_node)
{
    for (IRInst *q = sym_node->next; q && q->op != IR_SYMLABEL; q = q->next) {
        if (q->op == IR_NOP || q->op == IR_COMMENT || q->op == IR_ALIGN) continue;
        return q->op == IR_ENTER || q->op == IR_BB_START;
    }
    return false;
}

/* ================================================================
 * process_function — translate one function
 * ================================================================ */
static void process_function(IRInst *sym_node)
{
    /* Reset per-function state */
    vs_depth = 0; vsp = 0;
    lvsp_reset();
    reset_braun_state();
    ir3_reset();  /* reset vreg counter per function */

    /* Emit the function's SYMLABEL */
    IR3Inst *symlabel_ir3 = emit_ir3(IR3_SYMLABEL);
    symlabel_ir3->sym  = sym_node->sym;
    symlabel_ir3->line = sym_node->line;
    func_ir3_start = symlabel_ir3;

    /* Build CFG */
    int n_blocks = 0;
    BB *blocks = build_cfg(sym_node, &n_blocks);
    if (!blocks || n_blocks == 0) {
        /* No BBs: pass through instructions linearly (shouldn't happen for real function) */
        for (IRInst *p = sym_node->next; p && p->op != IR_SYMLABEL; p = p->next) {
            switch (p->op) {
            case IR_NOP: case IR_BB_START: break;
            default: {
                /* Just emit as-is via data path — this handles degenerate cases */
                break;
            }
            }
        }
        return;
    }
    /* Pre-scan to determine sizes for scratch arrays.
     * We need:
     *   phi_sz   — upper bound on vreg IDs (per-function; start at IR3_VREG_BASE+1)
     *   vlo_sz   — same (vreg_lea_offset is indexed the same way)
     *   lvsp_sz  — label ID span (lbl_max - lbl_min + 2)
     *   n_crit   — critical edges (landing pads expand block count)
     */
    {
        int n_ir = 0;
        int lbl_min = INT_MAX, lbl_max = INT_MIN;
        for (IRInst *pp = sym_node->next; pp && pp->op != IR_SYMLABEL; pp = pp->next) {
            n_ir++;
            if (pp->op == IR_J || pp->op == IR_JZ || pp->op == IR_JNZ
                || pp->op == IR_LABEL) {
                if (pp->operand < lbl_min) lbl_min = pp->operand;
                if (pp->operand > lbl_max) lbl_max = pp->operand;
            }
        }
        int phi_sz  = n_ir * 4 + 64;
        int vlo_sz  = n_ir * 4 + 64;
        int lvsp_sz = (lbl_min > lbl_max) ? 1 : (lbl_max - lbl_min + 2);

        int n_crit = 0;
        for (int i = 0; i < n_blocks; i++) {
            if (blocks[i].n_succs <= 1) continue;
            for (int si = 0; si < blocks[i].n_succs; si++) {
                if (blocks[blocks[i].succs[si]].n_preds > 1) n_crit++;
            }
        }
        init_fn_arrays(n_blocks + n_crit, phi_sz, vlo_sz, lvsp_sz);
    }

    cur_blocks = blocks;
    cur_n_blocks = n_blocks;

    /* Promotion gating for non-leaf functions.
     *
     * Promoting all scalars in a function with many calls and many locals
     * creates long-lived vregs that saturate the register file.  For large
     * functions (e.g. CoreMark main with a 2000-byte local array) excessive
     * promotion can overflow the stack.
     *
     * Leaf functions: promote unconditionally (no call-spanning issues).
     * Non-leaf functions: promote up to MAX_PROMO_NONLEAF distinct variables.
     * irc_regalloc spills any promoted vregs that span calls.
     */
    #define MAX_PROMO_NONLEAF 8
    bool func_has_calls_scan = false;
    int n_promo_vars = 0;
    int promo_offsets[64];
    for (IRInst *p = sym_node->next;
         p && p->op != IR_SYMLABEL; p = p->next) {
        if (p->op == IR_JL || p->op == IR_JLI)
            func_has_calls_scan = true;
        if (p->op == IR_LEA && p->sym == ir_promote_sentinel) {
            bool dup = false;
            for (int i = 0; i < n_promo_vars; i++) {
                if (promo_offsets[i] == p->operand) { dup = true; break; }
            }
            if (!dup && n_promo_vars < 64)
                promo_offsets[n_promo_vars++] = p->operand;
        }
    }
    func_can_promote = !func_has_calls_scan
                       || n_promo_vars <= MAX_PROMO_NONLEAF;

    /* Pre-seed promoted parameters in the entry block.
     *
     * Parameters get their initial values from the caller via the stack frame,
     * not from an explicit store in the function body.  If a promoted parameter
     * is first read in a non-entry block (e.g. a loop header), the Braun SSA
     * algorithm's readVariableRecursive will trace back to the entry block
     * and find no definition — returning IR3_VREG_NONE.  This produces a
     * non-trivial phi whose entry-block operand is undefined, causing the phi
     * vreg to survive through to assembly as an unrewritten virtual register.
     *
     * Fix: scan the function's stack IR for all IR_LEA instructions marked as
     * promotable (sym == ir_promote_sentinel) with positive bp-relative offsets
     * (parameters).  For each unique parameter, pre-allocate a vreg and call
     * writeVariable; the actual IR3_LOAD is deferred to the IR_ENTER handler
     * (because bp must be set up before bp-relative loads). */
    n_param_seeds = 0;
    for (IRInst *p = sym_node->next;
         p && p->op != IR_SYMLABEL; p = p->next) {
        if (func_can_promote && p->op == IR_LEA && p->sym == ir_promote_sentinel
            && p->operand > 0)  /* positive offset = parameter */
        {
            /* Determine load size from the following instruction */
            int sz = 0;
            IRInst *nxt = p->next;
            if (nxt) {
                if      (nxt->op == IR_LW) sz = 2;
                else if (nxt->op == IR_LB) sz = 1;
                else if (nxt->op == IR_LL) sz = 4;
            }
            if (sz == 0) sz = 2;  /* default to word */

            /* Check for duplicate */
            bool dup = false;
            for (int i = 0; i < n_param_seeds; i++) {
                if (param_seeds[i].offset == p->operand) { dup = true; break; }
            }
            if (!dup && n_param_seeds < MAX_PARAM_SEEDS) {
                int slot = p->operand + PROMOTE_BASE;
                if ((unsigned)slot < PROMOTE_SLOTS) {
                    promo_slot_size[slot] = sz;
                    int fresh = ir3_new_vreg();
                    param_seeds[n_param_seeds].offset = p->operand;
                    param_seeds[n_param_seeds].size   = sz;
                    param_seeds[n_param_seeds].vreg   = fresh;
                    n_param_seeds++;
                    writeVariable(0, slot, fresh);
                }
            }
        }
    }

    /* Set phi insert point for entry block (block 0).
     * Entry block has no label, so phi_insert_point[0] is set to the SYMLABEL node.
     * (Entry block is sealed from the start, so no pending phis will ever be queued.) */
    set_phi_insert_point(0, symlabel_ir3);

    /* Compute RPO */
    compute_rpo(n_blocks);

    /* Seal entry block (no predecessors that haven't been processed) */
    blocks[0].sealed = true;

    /* Process blocks in RPO */
    for (int ri = 0; ri < rpo_count; ri++) {
        int bb_id = rpo_order[ri];
        BB *bb = &blocks[bb_id];

        /* Seal this block if all predecessors are done */
        if (!bb->sealed && preds_done[bb_id] == bb->n_preds)
            sealBlock(bb_id);

        /* translate_bb will set phi_insert_point for this block:
         *   - Labeled blocks: when IR3_LABEL is emitted
         *   - Unlabeled blocks: at the start of translate_bb (using ir3_last)
         * No pre-set needed here. */

        /* Translate this BB */
        current_bb_id = bb_id;
        translate_bb(bb, bb_id);

        /* Mark successors */
        for (int si = 0; si < bb->n_succs; si++) {
            int succ = bb->succs[si];
            preds_done[succ]++;
            if (!blocks[succ].sealed && preds_done[succ] == blocks[succ].n_preds)
                sealBlock(succ);
        }
    }

    /* Seal any remaining unsealed blocks (e.g. unreachable) */
    for (int i = 0; i < n_blocks; i++)
        if (!blocks[i].sealed) sealBlock(i);

    /* Emit unreachable blocks (e.g. data-section blocks appended to the function
     * by mark_basic_blocks — string literals, alignment directives).
     * These blocks have no predecessors in the CFG and are skipped by RPO, but
     * their IR nodes (IR_LABEL, IR_BYTE, IR_ALIGN) must still be emitted. */
    for (int i = 0; i < n_blocks; i++) {
        if (!rpo_visited[i]) {
            /* Unreachable block: emit its nodes without SSA logic */
            current_bb_id = i;
            if (!blocks[i].sealed) blocks[i].sealed = true;
            translate_bb(&blocks[i], i);
        }
    }

    /* Critical-edge splitting: insert landing pads so phi copies don't
     * corrupt paths to the other successor of a multi-way branch. */
    split_critical_edges(&blocks, &n_blocks);

    /* Phi deconstruction: convert phi nodes to parallel copies */
    deconstructPhis(blocks, n_blocks);

    free_cfg(blocks, n_blocks);
}

/* ================================================================
 * braun_ssa — public entry point
 * ================================================================ */
IR3Inst *braun_ssa(BB *blocks_ignored, int n_blocks_ignored, IRInst *ir_head)
{
    (void)blocks_ignored;
    (void)n_blocks_ignored;

    ir3_head = NULL;
    ir3_tail = &ir3_head;
    ir3_last = NULL;
    vs_depth = 0;
    vsp = 0;
    lvsp_reset();
    reset_braun_state();

    IRInst *p = ir_head;
    while (p) {
        if (p->op == IR_SYMLABEL) {
            if (is_function_symlabel(p)) {
                process_function(p);
                /* Advance to next SYMLABEL or data section */
                p = p->next;
                while (p && p->op != IR_SYMLABEL) p = p->next;
            } else {
                /* Data label */
                IR3Inst *s = emit_ir3(IR3_SYMLABEL);
                s->sym  = p->sym;
                s->line = p->line;
                p = p->next;
            }
        } else {
            p = emit_data_node(p);
        }
    }

    return ir3_head;
}
