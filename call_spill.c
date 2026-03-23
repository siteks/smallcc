/* call_spill.c — Insert STORE/LOAD pairs around call sites for vregs
 * that are live across calls.
 *
 * Runs after ir3_optimize() and before linscan_regalloc().  Uses the
 * promo_vreg_info[] table (populated by braun_ssa via writeVariable)
 * to find each vreg's bp-relative save slot and size.
 *
 * Design: same-vreg reload.  For each vreg V live across a call:
 *   STORE [bp+offset], V    before the call
 *   LOAD  V, [bp+offset]    after the call
 * linscan allocates V a physical register for its entire interval.
 * The STORE/LOAD pair preserves the value across the caller-saved call.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir3.h"

/* ----------------------------------------------------------------
 * Live interval (subset of linscan's, just for call-spanning check)
 * ---------------------------------------------------------------- */
typedef struct {
    int vreg;
    int start;      /* first def serial */
    int end;        /* last use serial */
} CSInterval;

#define MAX_CS_INTERVALS 4096
static CSInterval cs_intervals[MAX_CS_INTERVALS];
static int        n_cs_intervals;

/* Map vreg → interval index */
#define CS_MAP_SIZE (MAX_CS_INTERVALS + IR3_VREG_BASE + 8)
static int cs_vreg_map[CS_MAP_SIZE];

static void cs_record_def(int vreg, int serial)
{
    if (vreg <= IR3_VREG_ACCUM) return;
    int idx = vreg - IR3_VREG_BASE;
    if (idx < 0 || idx >= MAX_CS_INTERVALS) return;
    if (cs_vreg_map[idx] == -1) {
        if (n_cs_intervals >= MAX_CS_INTERVALS) return;
        int k = n_cs_intervals++;
        cs_intervals[k].vreg  = vreg;
        cs_intervals[k].start = serial;
        cs_intervals[k].end   = serial;
        cs_vreg_map[idx] = k;
    } else {
        int k = cs_vreg_map[idx];
        if (serial < cs_intervals[k].start) cs_intervals[k].start = serial;
        if (serial > cs_intervals[k].end)   cs_intervals[k].end   = serial;
    }
}

static void cs_record_use(int vreg, int serial)
{
    if (vreg <= IR3_VREG_ACCUM) return;
    int idx = vreg - IR3_VREG_BASE;
    if (idx < 0 || idx >= MAX_CS_INTERVALS) return;
    if (cs_vreg_map[idx] == -1) return;
    int k = cs_vreg_map[idx];
    if (serial > cs_intervals[k].end) cs_intervals[k].end = serial;
}

/* ----------------------------------------------------------------
 * Lookup promo_vreg_info for a given vreg.
 * Returns pointer to entry, or NULL if not found.
 * ---------------------------------------------------------------- */
static PromoVregInfo *find_promo_info(int vreg, const char *func_sym)
{
    for (int i = 0; i < n_promo_vreg_info; i++) {
        if (promo_vreg_info[i].vreg == vreg
            && promo_vreg_info[i].func_sym == func_sym)
            return &promo_vreg_info[i];
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * IR3 node helpers
 * ---------------------------------------------------------------- */
static IR3Inst *cs_new_ir3(IR3Op op)
{
    IR3Inst *n = calloc(1, sizeof(IR3Inst));
    if (!n) { fprintf(stderr, "call_spill: out of memory\n"); exit(1); }
    n->op  = op;
    n->rd  = IR3_VREG_NONE;
    n->rs1 = IR3_VREG_NONE;
    n->rs2 = IR3_VREG_NONE;
    return n;
}

static void cs_insert_before(IR3Inst *prev, IR3Inst *target, IR3Inst *ins)
{
    ins->next = target;
    if (prev) prev->next = ins;
}

static void cs_insert_after(IR3Inst *after, IR3Inst *ins)
{
    ins->next = after->next;
    after->next = ins;
}

/* ----------------------------------------------------------------
 * Process one function
 * ---------------------------------------------------------------- */
static void spill_function(IR3Inst *func_head, IR3Inst *func_end, const char *func_sym)
{
    /* Phase 1: check for calls */
    bool has_calls = false;
    for (IR3Inst *p = func_head; p && p != func_end; p = p->next) {
        if (p->op == IR3_CALL || p->op == IR3_CALLR) {
            has_calls = true;
            break;
        }
    }
    if (!has_calls) return;

    /* Phase 2: number instructions and compute live intervals */
    n_cs_intervals = 0;
    memset(cs_vreg_map, -1, sizeof(cs_vreg_map));

    #define CS_MAX_LABELS 1024
    int label_serial[CS_MAX_LABELS];
    memset(label_serial, -1, sizeof(label_serial));

    int serial = 0;
    for (IR3Inst *p = func_head; p && p != func_end; p = p->next, serial++) {
        if (p->op == IR3_LABEL && p->imm >= 0 && p->imm < CS_MAX_LABELS)
            label_serial[p->imm] = serial;
        if (p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP)
            cs_record_def(p->rd, serial);
        if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP)
            cs_record_use(p->rs1, serial);
        if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP)
            cs_record_use(p->rs2, serial);
        /* IR3_STORE: rd is a read (base address), count as use */
        if (p->op == IR3_STORE && p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP)
            cs_record_use(p->rd, serial);
    }

    if (n_cs_intervals == 0) return;

    /* Phase 3: back-edge extension */
    #define CS_MAX_BACK_EDGES 64
    struct { int header; int jump; } back_edges[CS_MAX_BACK_EDGES];
    int n_back_edges = 0;

    serial = 0;
    for (IR3Inst *p = func_head; p && p != func_end; p = p->next, serial++) {
        if ((p->op == IR3_J || p->op == IR3_JZ || p->op == IR3_JNZ)
            && p->imm >= 0 && p->imm < CS_MAX_LABELS
            && label_serial[p->imm] >= 0
            && label_serial[p->imm] < serial)
        {
            if (n_back_edges < CS_MAX_BACK_EDGES) {
                back_edges[n_back_edges].header = label_serial[p->imm];
                back_edges[n_back_edges].jump   = serial;
                n_back_edges++;
            }
        }
    }

    for (int iter = 0; iter < 4 && n_back_edges > 0; iter++) {
        int changed = 0;
        for (int bi = 0; bi < n_back_edges; bi++) {
            int hdr = back_edges[bi].header;
            int jmp = back_edges[bi].jump;
            for (int i = 0; i < n_cs_intervals; i++) {
                if (cs_intervals[i].start <= hdr && cs_intervals[i].end >= hdr
                    && cs_intervals[i].end < jmp) {
                    cs_intervals[i].end = jmp;
                    changed = 1;
                }
            }
        }
        if (!changed) break;
    }

    /* Phase 4: collect call sites and their serials */
    #define CS_MAX_CALLS 256
    struct { int serial; IR3Inst *node; IR3Inst *prev; } calls[CS_MAX_CALLS];
    int n_calls = 0;

    serial = 0;
    IR3Inst *prev = NULL;
    for (IR3Inst *p = func_head; p && p != func_end; prev = p, p = p->next, serial++) {
        if (p->op == IR3_CALL || p->op == IR3_CALLR) {
            if (n_calls < CS_MAX_CALLS) {
                calls[n_calls].serial = serial;
                calls[n_calls].node   = p;
                calls[n_calls].prev   = prev;
                n_calls++;
            }
        }
    }

    /* Phase 5: insert STORE/LOAD for each live-across-call vreg.
     *
     * Multiple SSA vregs may map to the same promoted variable (same
     * bp_offset).  Back-edge live-range extension can make more than one
     * of them appear live at a call site.  We must save/restore each
     * promo slot at most once per call — using the vreg with the LATEST
     * definition (highest interval start), since that is the most recent
     * SSA definition of the variable. */
    #define CS_MAX_SLOTS 256
    for (int ci = 0; ci < n_calls; ci++) {
        int S = calls[ci].serial;
        IR3Inst *call_node = calls[ci].node;

        /* Collect live-across-call vregs, dedup by bp_offset */
        struct { int vreg; int bp_offset; int size; int start; } slots[CS_MAX_SLOTS];
        int n_slots = 0;

        for (int i = 0; i < n_cs_intervals; i++) {
            if (cs_intervals[i].start <= S && cs_intervals[i].end > S) {
                int vreg = cs_intervals[i].vreg;
                PromoVregInfo *pi = find_promo_info(vreg, func_sym);
                if (!pi) {
                    fprintf(stderr, "call_spill: vreg %d live across call at serial %d "
                            "has no promo info (skipping)\n", vreg, S);
                    continue;
                }

                /* Check if we already have an entry for this bp_offset */
                int found = -1;
                for (int j = 0; j < n_slots; j++) {
                    if (slots[j].bp_offset == pi->bp_offset) { found = j; break; }
                }
                if (found >= 0) {
                    /* Keep the vreg with the latest definition */
                    if (cs_intervals[i].start > slots[found].start) {
                        slots[found].vreg  = vreg;
                        slots[found].start = cs_intervals[i].start;
                    }
                } else if (n_slots < CS_MAX_SLOTS) {
                    slots[n_slots].vreg      = vreg;
                    slots[n_slots].bp_offset = pi->bp_offset;
                    slots[n_slots].size      = pi->size;
                    slots[n_slots].start     = cs_intervals[i].start;
                    n_slots++;
                }
            }
        }

        /* Insert STORE/LOAD for each unique slot */
        for (int si = 0; si < n_slots; si++) {
            /* Insert STORE before call */
            IR3Inst *store = cs_new_ir3(IR3_STORE);
            store->rd   = IR3_VREG_BP;
            store->rs1  = slots[si].vreg;
            store->imm  = slots[si].bp_offset;
            store->size = slots[si].size;
            store->line = call_node->line;

            /* Find the node before call_node in the current list.
             * We walk from func_head because prev pointers may have shifted
             * due to earlier insertions. */
            IR3Inst *before_call = NULL;
            for (IR3Inst *q = func_head; q && q != func_end; q = q->next) {
                if (q->next == call_node) { before_call = q; break; }
            }
            if (before_call) {
                cs_insert_after(before_call, store);
            }

            /* Insert LOAD after call (same vreg) */
            IR3Inst *load = cs_new_ir3(IR3_LOAD);
            load->rd   = slots[si].vreg;
            load->rs1  = IR3_VREG_BP;
            load->imm  = slots[si].bp_offset;
            load->size = slots[si].size;
            load->line = call_node->line;
            cs_insert_after(call_node, load);
        }
    }
}

/* ----------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------- */
void call_spill_insert(IR3Inst *head)
{
    IR3Inst *func_start = NULL;

    for (IR3Inst *p = head; p; p = p->next) {
        if (p->op == IR3_SYMLABEL) {
            /* Check if this is a function (has ENTER) */
            IR3Inst *q = p->next;
            while (q && q->op == IR3_COMMENT) q = q->next;
            if (q && q->op == IR3_ENTER) {
                if (func_start) {
                    /* Process previous function */
                    spill_function(func_start, p, func_start->sym);
                }
                func_start = p;
            } else {
                if (func_start) {
                    spill_function(func_start, p, func_start->sym);
                }
                func_start = NULL;
            }
        }
    }
    /* Process last function */
    if (func_start) {
        spill_function(func_start, NULL, func_start->sym);
    }
}
