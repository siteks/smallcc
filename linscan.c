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
 *   Fresh vregs → 1–7 (physical r1–r7)
 *   -1, -2 unchanged.
 *
 * No spilling: the Sethi-Ullman guarantee keeps expression depth ≤ 3,
 * so at most 3 scratch vregs are live simultaneously.  If the pool
 * runs out (shouldn't happen with SU) we spill to a stack slot.
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
    int phys;       /* assigned physical register (0-7), -1 = unassigned */
    int spill_off;  /* bp-relative offset of spill slot (-1 = none) */
} Interval;

#define MAX_INTERVALS 4096
static Interval intervals[MAX_INTERVALS];
static int      n_intervals;

/* Map vreg → interval index for fast lookup during scan. */
#define VREG_MAP_SIZE (MAX_INTERVALS + IR3_VREG_BASE + 8)
static int vreg_map[VREG_MAP_SIZE];  /* vreg → index in intervals[]; -1 = none */

/* Physical register pool: r1..r7 (r0 is reserved for ACCUM). */
#define NPHYS 7
static int phys_pool[NPHYS];
static int pool_size;

static void pool_init(void) {
    pool_size = NPHYS;
    for (int i = 0; i < NPHYS; i++)
        phys_pool[i] = NPHYS - i;  /* r7, r6, ..., r1 (r1 given out first via pop) */
}

static int pool_alloc(void) {
    if (pool_size > 0)
        return phys_pool[--pool_size];
    return -1;   /* pool empty */
}

static void pool_free(int reg) {
    if (reg >= 1 && reg <= 7)
        phys_pool[pool_size++] = reg;
}

/* Active set — intervals sorted by end (earliest end first).
 * Small fixed-size array since depth ≤ 3 in practice. */
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

/* Spill state: we use a simple bump allocator growing from the current
 * function's frame.  In practice spilling should never be needed with
 * Sethi-Ullman labelling. */
static int spill_next_off;   /* next available bp-relative offset (negative) */

static int alloc_spill_slot(int size) {
    spill_next_off -= size;
    /* Keep alignment */
    if (size == 4 && (spill_next_off & 3))
        spill_next_off &= ~3;
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
 * IR3_VREG_ACCUM → 0; virtual → assigned phys; others unchanged.
 * ---------------------------------------------------------------- */
static int rewrite_reg(int reg)
{
    if (reg == IR3_VREG_ACCUM) return 0;   /* accumulator → r0 */
    if (reg < IR3_VREG_BASE)   return reg; /* -2, -1, or already physical */
    int idx = reg - IR3_VREG_BASE;
    if (idx < 0 || idx >= MAX_INTERVALS) return reg;
    int k = vreg_map[idx];
    if (k == -1) return reg;
    return (intervals[k].phys >= 0) ? intervals[k].phys : 0;
}

/* ----------------------------------------------------------------
 * Allocate registers for one function's IR3Inst list.
 * Called by linscan_regalloc() once per function (between SYMLABEL
 * entries).
 *
 * spill_base: the bp-relative offset just below the frame's existing
 * locals, used as the starting point for any spill slots.
 * For this implementation, spilling is a fallback; in practice
 * with SU it should not fire.
 * ---------------------------------------------------------------- */
static void alloc_function(IR3Inst *func_head, IR3Inst *func_end)
{
    /* --- Phase 1: number instructions and compute live intervals --- */
    n_intervals = 0;
    memset(vreg_map, -1, sizeof(vreg_map));

    int serial = 0;
    for (IR3Inst *p = func_head; p != func_end; p = p->next, serial++) {
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
    n_active    = 0;
    spill_next_off = -256;  /* conservative start; real frame analysis not done */

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
            /* Find longest-lived active interval. */
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
                intervals[sp_idx].spill_off = alloc_spill_slot(4);
                intervals[sp_idx].phys = -1;
                active_remove(spill_pos);
                /* Warn: in practice SU should prevent this. */
                fprintf(stderr, "linscan: spilling vreg %d (depth overflow)\n",
                        intervals[sp_idx].vreg);
            } else {
                /* Spill current interval. */
                iv->spill_off = alloc_spill_slot(4);
                iv->phys = -1;
                fprintf(stderr, "linscan: spilling vreg %d\n", iv->vreg);
                continue;
            }
        }

        iv->phys = reg;
        active_add(i);
    }

    /* --- Phase 4: rewrite IR3Inst registers --- */
    for (IR3Inst *p = func_head; p != func_end; p = p->next) {
        if (p->rd  != IR3_VREG_NONE && p->rd  != IR3_VREG_BP)
            p->rd  = rewrite_reg(p->rd);
        if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP)
            p->rs1 = rewrite_reg(p->rs1);
        if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP)
            p->rs2 = rewrite_reg(p->rs2);
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

    for (IR3Inst *p = head; p; p = p->next) {
        if (p->op == IR3_SYMLABEL) {
            /* Close previous function if any. */
            if (func_start)
                alloc_function(func_start, p);
            func_start = p->next;   /* function body starts after the label */
        }
    }
    /* Handle last function. */
    if (func_start)
        alloc_function(func_start, NULL);

    /* Final pass: rewrite ACCUM everywhere (even in data / label nodes). */
    for (IR3Inst *p = head; p; p = p->next) {
        if (p->rd  == IR3_VREG_ACCUM) p->rd  = 0;
        if (p->rs1 == IR3_VREG_ACCUM) p->rs1 = 0;
        if (p->rs2 == IR3_VREG_ACCUM) p->rs2 = 0;
    }
}
