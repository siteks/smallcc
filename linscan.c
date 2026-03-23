/* linscan.c — Linear-scan register allocator for IR3.
 *
 * Implements the Poletto/Sarkar (1999) linear-scan algorithm.
 *
 * Virtual register encoding before this pass:
 *   IR3_VREG_ACCUM (100) — accumulator, always maps to physical r0.
 *   101, 102, …          — fresh scratch vregs.
 *   IR3_VREG_NONE (-1)   — no register.
 *   IR3_VREG_BP   (-2)   — bp-relative (LOAD/STORE only; pass through).
 *
 * After this pass:
 *   IR3_VREG_ACCUM → 0 (physical r0)
 *   Fresh vregs → 1–6 (physical r1–r6)
 *   r7 is reserved as scratch for spill loads/reloads.
 *   -1, -2 unchanged.
 *
 * Spilling: when the register pool is exhausted, the longest-lived active
 * interval is spilled to a bp-relative stack slot.  Phase 5 walks the IR3
 * list and inserts IR3_STORE (spill) after each DEF and IR3_LOAD (reload)
 * before each USE of the spilled vreg, using r7 as the scratch register.
 * The function's ENTER node is expanded to cover the spill area.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir3.h"

/* ----------------------------------------------------------------
 * Live interval
 * ---------------------------------------------------------------- */
typedef struct {
    int vreg;       /* virtual register ID (>= IR3_VREG_BASE) */
    int start;      /* instruction serial of first definition  */
    int end;        /* instruction serial of last use           */
    int phys;       /* assigned physical register (0-6), -1 = spilled */
    int spill_off;  /* bp-relative offset of spill slot (-1 = none) */
} Interval;

#define MAX_INTERVALS 4096
static Interval intervals[MAX_INTERVALS];
static int      n_intervals;

/* Map vreg → interval index for fast lookup during scan. */
#define VREG_MAP_SIZE (MAX_INTERVALS + IR3_VREG_BASE + 8)
static int vreg_map[VREG_MAP_SIZE];  /* vreg → index in intervals[]; -1 = none */

/* Physical register pool: r1..r6 (r0 = ACCUM, r7 = spill scratch). */
#define NPHYS 6
#define SPILL_SCRATCH 7   /* r7 reserved for spill loads/stores */
static int phys_pool[NPHYS];
static int pool_size;

static void pool_init(void) {
    pool_size = NPHYS;
    for (int i = 0; i < NPHYS; i++)
        phys_pool[i] = NPHYS - i;  /* r6, r5, ..., r1 (r1 given out first via pop) */
}

static int pool_alloc(void) {
    if (pool_size > 0)
        return phys_pool[--pool_size];
    return -1;   /* pool empty */
}

static void pool_free(int reg) {
    if (reg >= 1 && reg <= NPHYS)
        phys_pool[pool_size++] = reg;
}

/* Active set — intervals sorted by end (earliest end first). */
#define MAX_ACTIVE 64
static int active[MAX_ACTIVE];   /* indices into intervals[] */
static int n_active;

static void active_add(int idx) {
    /* Insert keeping sorted by end (earliest end first). */
    int k = n_active++;
    while (k > 0 && intervals[active[k-1]].end > intervals[idx].end) {
        active[k] = active[k-1];
        k--;
    }
    active[k] = idx;
}

static void active_remove(int pos) {
    for (int i = pos; i < n_active - 1; i++)
        active[i] = active[i+1];
    n_active--;
}

/* Spill state: bump allocator growing downward from the function's frame. */
static int spill_next_off;   /* next available bp-relative offset (negative) */
static int n_spills;         /* number of spill slots allocated */

static int alloc_spill_slot(void) {
    /* All spill slots are 4 bytes (long-sized) for simplicity. */
    spill_next_off -= 4;
    /* Ensure 4-byte alignment */
    spill_next_off &= ~3;
    n_spills++;
    return spill_next_off;
}

/* ----------------------------------------------------------------
 * Helpers to record definition / use of a vreg
 * ---------------------------------------------------------------- */
static void record_def(int vreg, int serial) {
    if (vreg <= IR3_VREG_ACCUM) return;  /* skip -2, -1, physical, and ACCUM (fixed r0) */
    int idx = vreg - IR3_VREG_BASE;
    if (idx >= MAX_INTERVALS) {
        fprintf(stderr, "linscan: vreg %d out of range\n", vreg);
        return;
    }
    if (vreg_map[idx] == -1) {
        /* First definition: create interval. */
        if (n_intervals >= MAX_INTERVALS) {
            fprintf(stderr, "linscan: too many intervals\n");
            return;
        }
        int k = n_intervals++;
        intervals[k].vreg      = vreg;
        intervals[k].start     = serial;
        intervals[k].end       = serial;
        intervals[k].phys      = -1;
        intervals[k].spill_off = -1;
        vreg_map[idx]          = k;
    } else {
        int k = vreg_map[idx];
        if (serial < intervals[k].start) intervals[k].start = serial;
        if (serial > intervals[k].end)   intervals[k].end   = serial;
    }
}

static void record_use(int vreg, int serial) {
    if (vreg <= IR3_VREG_ACCUM) return;  /* ACCUM and below are not allocated */
    int idx = vreg - IR3_VREG_BASE;
    if (idx >= MAX_INTERVALS) return;
    if (vreg_map[idx] == -1) return;  /* undefined vreg used — should not happen */
    int k = vreg_map[idx];
    if (serial > intervals[k].end) intervals[k].end = serial;
}

/* Comparator for sorting by start position. */
static int cmp_by_start(const void *a, const void *b) {
    const Interval *ia = (const Interval *)a;
    const Interval *ib = (const Interval *)b;
    return ia->start - ib->start;
}

/* ----------------------------------------------------------------
 * Rewrite a single vreg field to its physical register.
 * IR3_VREG_ACCUM → 0; virtual → assigned phys; spilled → SPILL_SCRATCH (r7).
 * ---------------------------------------------------------------- */
static int rewrite_reg(int reg)
{
    if (reg == IR3_VREG_ACCUM) return 0;   /* accumulator → r0 */
    if (reg < IR3_VREG_BASE)   return reg; /* -2, -1, or already physical */
    int idx = reg - IR3_VREG_BASE;
    if (idx < 0 || idx >= MAX_INTERVALS) return reg;
    int k = vreg_map[idx];
    if (k == -1) return reg;
    if (intervals[k].phys >= 0) return intervals[k].phys;
    /* Spilled: use r7 as scratch.  Phase 5 will insert the actual
     * spill loads/stores around this instruction. */
    return SPILL_SCRATCH;
}

/* ----------------------------------------------------------------
 * Check if a vreg is spilled (has a spill slot assigned).
 * ---------------------------------------------------------------- */
static int is_spilled(int vreg)
{
    if (vreg <= IR3_VREG_ACCUM) return 0;
    int idx = vreg - IR3_VREG_BASE;
    if (idx < 0 || idx >= MAX_INTERVALS) return 0;
    int k = vreg_map[idx];
    if (k == -1) return 0;
    return intervals[k].spill_off != -1;
}

/* Get spill offset for a vreg (must be spilled). */
static int get_spill_off(int vreg)
{
    int idx = vreg - IR3_VREG_BASE;
    int k = vreg_map[idx];
    return intervals[k].spill_off;
}

/* ----------------------------------------------------------------
 * IR3 node allocation helper (for inserting spill loads/stores)
 * ---------------------------------------------------------------- */
static IR3Inst *new_ir3(IR3Op op)
{
    IR3Inst *n = calloc(1, sizeof(IR3Inst));
    n->op  = op;
    n->rd  = IR3_VREG_NONE;
    n->rs1 = IR3_VREG_NONE;
    n->rs2 = IR3_VREG_NONE;
    n->imm = 0;
    return n;
}

/* Insert node `ins` after `after` in the linked list. */
static void insert_after(IR3Inst *after, IR3Inst *ins)
{
    ins->next = after->next;
    after->next = ins;
}

/* Insert node `ins` before `target` in the linked list.
 * `prev` is the node before `target` (NULL if target is head). */
static void insert_before(IR3Inst *prev, IR3Inst *target, IR3Inst *ins)
{
    ins->next = target;
    if (prev) prev->next = ins;
}

/* ----------------------------------------------------------------
 * Allocate registers for one function's IR3Inst list.
 * func_head: first instruction after the SYMLABEL.
 * func_end:  first instruction of the NEXT function (or NULL).
 * enter_node: the IR3_ENTER node for this function (for frame expansion).
 * ---------------------------------------------------------------- */
static void alloc_function(IR3Inst *func_head, IR3Inst *func_end,
                           IR3Inst *enter_node)
{
    /* --- Phase 1: number instructions and compute live intervals --- */
    n_intervals = 0;
    n_spills    = 0;
    memset(vreg_map, -1, sizeof(vreg_map));

    /* Scan the function to find the deepest bp-relative offset used by
     * locals.  This includes both the ENTER frame and any inner-scope
     * ADJ allocations.  Spill slots must go below all of these.
     *
     * We track two things:
     *  1. The most negative bp-relative offset in LOAD/STORE/LEA nodes.
     *  2. The cumulative ADJ depth (enter_N + nested adj -N adjustments)
     *     which represents the maximum frame extent at any point.
     */
    int enter_N = enter_node ? enter_node->imm : 0;
    int min_bp_off = -enter_N;   /* deepest bp-relative offset */
    int adj_depth  = -enter_N;   /* running bp-relative sp position */

    for (IR3Inst *p = func_head; p != func_end; p = p->next) {
        if (p->op == IR3_LOAD && p->rs1 == IR3_VREG_BP) {
            int off = p->imm;
            /* Account for the size of the access — the slot extends
             * from imm to imm + size - 1, so the deepest byte is at imm. */
            if (off < min_bp_off) min_bp_off = off;
        }
        if (p->op == IR3_STORE && p->rd == IR3_VREG_BP) {
            if (p->imm < min_bp_off) min_bp_off = p->imm;
        }
        if (p->op == IR3_LEA) {
            if (p->imm < min_bp_off) min_bp_off = p->imm;
        }
        if (p->op == IR3_ADJ) {
            adj_depth += p->imm;  /* adj -N makes it more negative */
            if (adj_depth < min_bp_off) min_bp_off = adj_depth;
        }
    }

    /* Start spill slots below the deepest existing frame usage,
     * aligned to a 4-byte boundary. */
    spill_next_off = min_bp_off & ~3;
    if (spill_next_off >= min_bp_off)
        spill_next_off -= 4;  /* ensure strictly below */

    /* Build label_id → serial mapping for back-edge detection. */
    #define MAX_LABELS 1024
    int label_serial[MAX_LABELS];
    memset(label_serial, -1, sizeof(label_serial));

    int serial = 0;
    for (IR3Inst *p = func_head; p != func_end; p = p->next, serial++) {
        /* Record label positions. */
        if (p->op == IR3_LABEL && p->imm >= 0 && p->imm < MAX_LABELS)
            label_serial[p->imm] = serial;
        /* Definition */
        if (p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP)
            record_def(p->rd, serial);
        /* Uses */
        if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP)
            record_use(p->rs1, serial);
        if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP)
            record_use(p->rs2, serial);
    }

    if (n_intervals == 0) return;   /* no virtual regs in this function */

    /* --- Phase 1b: extend intervals across loop back-edges ---
     *
     * A back-edge is a J/JZ/JNZ whose target label has a lower serial number
     * than the jump itself.  Any vreg whose interval overlaps the loop body
     * [header_serial, back_edge_serial] must have its end extended to at least
     * the back-edge serial, because the vreg will be needed again on the next
     * loop iteration.  Iterate until stable (handles nested loops). */
    {
        /* Collect back-edges: (header_serial, jump_serial) pairs. */
        #define MAX_BACK_EDGES 64
        struct { int header; int jump; } back_edges[MAX_BACK_EDGES];
        int n_back_edges = 0;

        serial = 0;
        for (IR3Inst *p = func_head; p != func_end; p = p->next, serial++) {
            if ((p->op == IR3_J || p->op == IR3_JZ || p->op == IR3_JNZ)
                && p->imm >= 0 && p->imm < MAX_LABELS
                && label_serial[p->imm] >= 0
                && label_serial[p->imm] < serial)
            {
                if (n_back_edges < MAX_BACK_EDGES) {
                    back_edges[n_back_edges].header = label_serial[p->imm];
                    back_edges[n_back_edges].jump   = serial;
                    n_back_edges++;
                }
            }
        }

        /* Extend intervals.  Repeat until no changes (handles nested loops
         * where extending one interval opens up another). */
        for (int iter = 0; iter < 4 && n_back_edges > 0; iter++) {
            int changed = 0;
            for (int bi = 0; bi < n_back_edges; bi++) {
                int hdr = back_edges[bi].header;
                int jmp = back_edges[bi].jump;
                for (int i = 0; i < n_intervals; i++) {
                    /* If interval is live at loop header (starts <= header
                     * and ends >= header), extend to back-edge. */
                    if (intervals[i].start <= hdr && intervals[i].end >= hdr
                        && intervals[i].end < jmp) {
                        intervals[i].end = jmp;
                        changed = 1;
                    }
                }
            }
            if (!changed) break;
        }
    }

    /* --- Phase 2: sort intervals by start --- */
    qsort(intervals, n_intervals, sizeof(Interval), cmp_by_start);

    /* Rebuild vreg_map after sort. */
    memset(vreg_map, -1, sizeof(vreg_map));
    for (int i = 0; i < n_intervals; i++) {
        int idx = intervals[i].vreg - IR3_VREG_BASE;
        if (idx >= 0 && idx < MAX_INTERVALS)
            vreg_map[idx] = i;
    }

    /* --- Phase 3: linear scan allocation --- */
    pool_init();
    n_active = 0;

    for (int i = 0; i < n_intervals; i++) {
        Interval *iv = &intervals[i];
        int start = iv->start;

        /* Expire old intervals whose end < current start */
        for (int j = 0; j < n_active; ) {
            int aj = active[j];
            if (intervals[aj].end < start) {
                pool_free(intervals[aj].phys);
                active_remove(j);
            } else {
                j++;
            }
        }

        /* Allocate a physical register */
        int reg = pool_alloc();
        if (reg < 0) {
            /* Pool empty: spill the active interval with the largest end. */
            int spill_pos = -1;
            int spill_end = -1;
            for (int j = 0; j < n_active; j++) {
                if (intervals[active[j]].end > spill_end) {
                    spill_end = intervals[active[j]].end;
                    spill_pos = j;
                }
            }
            if (spill_pos >= 0 && intervals[active[spill_pos]].end > iv->end) {
                /* Spill the active one with the larger end; give its reg to iv. */
                int sp_idx = active[spill_pos];
                reg = intervals[sp_idx].phys;
                intervals[sp_idx].spill_off = alloc_spill_slot();
                intervals[sp_idx].phys = -1;
                active_remove(spill_pos);
                fprintf(stderr, "linscan: spilling vreg %d to [bp%d]\n",
                        intervals[sp_idx].vreg, intervals[sp_idx].spill_off);
            } else {
                /* Spill current interval. */
                iv->spill_off = alloc_spill_slot();
                iv->phys = -1;
                fprintf(stderr, "linscan: spilling vreg %d to [bp%d]\n",
                        iv->vreg, iv->spill_off);
                continue;
            }
        }

        iv->phys = reg;
        active_add(i);
    }

    /* --- Phase 4: rewrite IR3Inst registers --- */
    /* Save original vregs before rewriting so Phase 5 can find spilled ones. */
    /* We store them in a parallel array indexed by instruction serial. */
    int n_insts = serial;
    int *orig_rd  = NULL;
    int *orig_rs1 = NULL;
    int *orig_rs2 = NULL;

    if (n_spills > 0) {
        orig_rd  = calloc(n_insts, sizeof(int));
        orig_rs1 = calloc(n_insts, sizeof(int));
        orig_rs2 = calloc(n_insts, sizeof(int));
        int s = 0;
        for (IR3Inst *p = func_head; p != func_end; p = p->next, s++) {
            orig_rd[s]  = p->rd;
            orig_rs1[s] = p->rs1;
            orig_rs2[s] = p->rs2;
        }
    }

    for (IR3Inst *p = func_head; p != func_end; p = p->next) {
        if (p->rd  != IR3_VREG_NONE && p->rd  != IR3_VREG_BP)
            p->rd  = rewrite_reg(p->rd);
        if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP)
            p->rs1 = rewrite_reg(p->rs1);
        if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP)
            p->rs2 = rewrite_reg(p->rs2);
    }

    /* --- Phase 5a: expand ENTER and shift flush offsets BEFORE spill
     * insertion, so that spill stores/loads (which already have correct
     * bp-relative offsets from alloc_spill_slot) are not double-shifted. */
    if (n_spills > 0 && enter_node) {
        int orig_N = enter_node->imm;
        int total_frame = -spill_next_off;
        total_frame = (total_frame + 3) & ~3;  /* align up */
        int expansion = total_frame - orig_N;

        if (expansion > 0) {
            enter_node->imm = total_frame;

            /* Shift flush offsets: any bp-relative offset < -orig_N
             * was a call-site flush and must move down by expansion.
             * This runs on the pre-spill IR3, so only braun.c's flush
             * stores are affected — spill stores are added in Phase 5b. */
            for (IR3Inst *p = func_head; p != func_end; p = p->next) {
                if (p->op == IR3_LOAD && p->rs1 == IR3_VREG_BP
                    && p->imm < -orig_N)
                    p->imm -= expansion;
                if (p->op == IR3_STORE && p->rd == IR3_VREG_BP
                    && p->imm < -orig_N)
                    p->imm -= expansion;
                if (p->op == IR3_LEA && p->imm < -orig_N)
                    p->imm -= expansion;
            }
        }
    }

    /* --- Phase 5b: insert spill stores and reload loads --- */
    if (n_spills > 0) {
        int s = 0;
        IR3Inst *prev = NULL;
        for (IR3Inst *p = func_head; p != func_end; ) {
            IR3Inst *next = p->next;
            int o_rd  = orig_rd[s];
            int o_rs1 = orig_rs1[s];
            int o_rs2 = orig_rs2[s];

            /* Insert reload BEFORE this instruction for spilled source operands.
             * Load from spill slot into r7 (SPILL_SCRATCH). */
            if (o_rs1 != IR3_VREG_NONE && o_rs1 != IR3_VREG_BP && is_spilled(o_rs1)) {
                IR3Inst *ld = new_ir3(IR3_LOAD);
                ld->rd   = SPILL_SCRATCH;
                ld->rs1  = IR3_VREG_BP;
                ld->imm  = get_spill_off(o_rs1);
                ld->size = 4;
                ld->line = p->line;
                insert_before(prev, p, ld);
                if (!prev) func_head = ld;  /* shouldn't happen in practice */
                prev = ld;
            }
            if (o_rs2 != IR3_VREG_NONE && o_rs2 != IR3_VREG_BP && is_spilled(o_rs2)) {
                /* If rs1 is also spilled to r7, we have a conflict.
                 * This shouldn't happen in practice because braun.c
                 * generates at most one virtual-stack operand per ALU op,
                 * and the other is always ACCUM (r0). Warn if it happens. */
                if (o_rs1 != IR3_VREG_NONE && o_rs1 != IR3_VREG_BP && is_spilled(o_rs1)) {
                    fprintf(stderr, "linscan: WARNING: two spilled operands "
                            "sharing r7 scratch in same instruction\n");
                }
                IR3Inst *ld = new_ir3(IR3_LOAD);
                ld->rd   = SPILL_SCRATCH;
                ld->rs1  = IR3_VREG_BP;
                ld->imm  = get_spill_off(o_rs2);
                ld->size = 4;
                ld->line = p->line;
                insert_before(prev, p, ld);
                if (!prev) func_head = ld;
                prev = ld;
            }

            /* Insert spill store AFTER this instruction for spilled destination. */
            if (o_rd != IR3_VREG_NONE && o_rd != IR3_VREG_BP && is_spilled(o_rd)) {
                IR3Inst *st = new_ir3(IR3_STORE);
                st->rd   = IR3_VREG_BP;
                st->rs1  = SPILL_SCRATCH;
                st->imm  = get_spill_off(o_rd);
                st->size = 4;
                st->line = p->line;
                insert_after(p, st);
                /* Skip past the store we just inserted */
                prev = st;
                s++;
                p = st->next;
                continue;
            }

            prev = p;
            s++;
            p = next;
        }

        free(orig_rd);
        free(orig_rs1);
        free(orig_rs2);
    }
}

/* ----------------------------------------------------------------
 * linscan_regalloc — public entry point.
 * Scans the full IR3 list and runs alloc_function() on each
 * function (between IR3_SYMLABEL boundaries).
 * ---------------------------------------------------------------- */
void linscan_regalloc(IR3Inst *head)
{
    IR3Inst *func_start = NULL;
    IR3Inst *enter_node = NULL;

    for (IR3Inst *p = head; p; p = p->next) {
        if (p->op == IR3_SYMLABEL) {
            /* Close previous function if any. */
            if (func_start)
                alloc_function(func_start, p, enter_node);
            func_start = p->next;   /* function body starts after the label */
            /* Find the ENTER node */
            enter_node = NULL;
            for (IR3Inst *q = p->next; q; q = q->next) {
                if (q->op == IR3_ENTER) { enter_node = q; break; }
                if (q->op == IR3_SYMLABEL) break;  /* next function */
            }
        }
    }
    /* Handle last function. */
    if (func_start)
        alloc_function(func_start, NULL, enter_node);

    /* Final pass: rewrite ACCUM everywhere (even in data / label nodes). */
    for (IR3Inst *p = head; p; p = p->next) {
        if (p->rd  == IR3_VREG_ACCUM) p->rd  = 0;
        if (p->rs1 == IR3_VREG_ACCUM) p->rs1 = 0;
        if (p->rs2 == IR3_VREG_ACCUM) p->rs2 = 0;
    }
}
