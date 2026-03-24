/* linscan.c — Split-interval linear-scan register allocator for IR3.
 *
 * Implements a second-chance linear-scan algorithm with interval splitting.
 * When the register pool is exhausted, the victim interval is split rather
 * than fully spilled: the current segment ends, a STORE saves the value,
 * and a new segment begins at the next use with a LOAD.  Because the
 * STORE uses the register being reclaimed (still assigned at that point)
 * and the LOAD uses whatever register the new segment receives, no
 * dedicated scratch register is needed.
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
 *   -1, -2 unchanged.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir3.h"

/* ----------------------------------------------------------------
 * Segment: a sub-range of a vreg's live interval with a register
 * assignment.  Segments are linked chronologically per vreg.
 * ---------------------------------------------------------------- */
typedef struct Segment {
    int start;
    int end;
    int phys;             /* assigned physical register (1-NPHYS), -1 = unassigned */
    struct Segment *next;
} Segment;

/* ----------------------------------------------------------------
 * SplitInterval: one per virtual register.
 * ---------------------------------------------------------------- */
typedef struct {
    int vreg;
    int orig_start;       /* original interval start (for sorting) */
    int orig_end;         /* original interval end */
    Segment *segments;    /* linked list of segments */
    int spill_off;        /* bp-relative spill slot (-1 = none) */
} SplitInterval;

#define MAX_INTERVALS 4096
static SplitInterval intervals[MAX_INTERVALS];
static int n_intervals;

/* Map vreg → interval index. */
#define VREG_MAP_SIZE (MAX_INTERVALS + IR3_VREG_BASE + 8)
static int vreg_map[VREG_MAP_SIZE];

/* Physical register pool: r1..r6.  r0 = ACCUM. */
#define NPHYS 6
static int phys_pool[NPHYS];
static int pool_size;

static void pool_init(void) {
    pool_size = NPHYS;
    for (int i = 0; i < NPHYS; i++)
        phys_pool[i] = NPHYS - i;  /* r6, r5, ..., r1 (r1 given out first) */
}

static int pool_alloc(void) {
    if (pool_size > 0)
        return phys_pool[--pool_size];
    return -1;
}

static void pool_free(int reg) {
    if (reg >= 1 && reg <= NPHYS)
        phys_pool[pool_size++] = reg;
}

/* ----------------------------------------------------------------
 * Active set — tracks (interval index, active segment) pairs,
 * sorted by segment end (earliest end first).
 * ---------------------------------------------------------------- */
typedef struct {
    int idx;
    Segment *seg;
} ActiveEntry;

#define MAX_ACTIVE 64
static ActiveEntry active[MAX_ACTIVE];
static int n_active;

static void active_add(int idx, Segment *seg) {
    int k = n_active++;
    while (k > 0 && active[k-1].seg->end > seg->end) {
        active[k] = active[k-1];
        k--;
    }
    active[k].idx = idx;
    active[k].seg = seg;
}

static void active_remove(int pos) {
    for (int i = pos; i < n_active - 1; i++)
        active[i] = active[i+1];
    n_active--;
}

/* ----------------------------------------------------------------
 * Reference positions — per-vreg list of instruction serials where the
 * vreg is referenced (def or use). Needed for "next reference after P"
 * queries during interval splitting.
 * ---------------------------------------------------------------- */
#define MAX_REF_POS 32768
static int ref_positions[MAX_REF_POS];
static int ref_is_def[MAX_REF_POS];     /* 1 = def, 0 = use */
static int ref_pos_start[MAX_INTERVALS]; /* start index into ref_positions[] */
static int ref_pos_count[MAX_INTERVALS]; /* number of reference positions */
static int n_ref_positions;

/* Find the first reference of interval idx that is > serial.
 * Returns the ref serial, or orig_end if the interval extends beyond
 * (e.g. via back-edge extension), or INT_MAX if truly dead.
 * If out_is_def is non-NULL, *out_is_def is set to 1 if the ref is a def. */
static int next_ref_after(int idx, int serial, int *out_is_def) {
    int base = ref_pos_start[idx];
    int count = ref_pos_count[idx];
    /* Binary search for first entry > serial */
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ref_positions[base + mid] <= serial)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < count) {
        if (out_is_def) *out_is_def = ref_is_def[base + lo];
        return ref_positions[base + lo];
    }
    /* No explicit reference after serial, but if the interval extends
     * beyond (due to back-edge extension), treat orig_end as the next
     * reference so reload segments are created correctly. */
    if (intervals[idx].orig_end > serial) {
        if (out_is_def) *out_is_def = 0; /* treat as use for safety */
        return intervals[idx].orig_end;
    }
    if (out_is_def) *out_is_def = 0;
    return 0x7fffffff;
}

/* ----------------------------------------------------------------
 * Deferred spill actions (STORE/LOAD inserted after Phase 4 rewrite)
 * ---------------------------------------------------------------- */
typedef struct {
    int kind;           /* 0 = STORE after serial, 1 = LOAD before serial */
    int serial;         /* target serial (for sort ordering) */
    IR3Inst *target;    /* target IR3 node (stable across Phase 4b insertions) */
    int phys_reg;       /* register to store from / load into */
    int spill_off;      /* bp-relative offset */
    int size;           /* always 4 */
    int line;           /* source line */
} SplitAction;

#define MAX_SPLIT_ACTIONS 2048
static SplitAction split_actions[MAX_SPLIT_ACTIONS];
static int n_split_actions;

/* serial_to_node: maps Phase 1 serial → IR3Inst* for stable targeting
 * across Phase 4b insertions. */
static IR3Inst **serial_to_node;

static void record_split_action(int kind, int serial, int phys, int off, int line) {
    if (n_split_actions >= MAX_SPLIT_ACTIONS) {
        fprintf(stderr, "linscan: too many split actions\n");
        return;
    }
    split_actions[n_split_actions].kind = kind;
    split_actions[n_split_actions].serial = serial;
    split_actions[n_split_actions].target = serial_to_node ? serial_to_node[serial] : NULL;
    split_actions[n_split_actions].phys_reg = phys;
    split_actions[n_split_actions].spill_off = off;
    split_actions[n_split_actions].size = 4;
    split_actions[n_split_actions].line = line;
    n_split_actions++;
}

/* Comparator for sorting split actions by serial, LOADs before STOREs at same serial.
 * LOADs (kind=1) insert BEFORE an instruction, STOREs (kind=0) insert AFTER. */
static int cmp_split_action(const void *a, const void *b) {
    const SplitAction *sa = (const SplitAction *)a;
    const SplitAction *sb = (const SplitAction *)b;
    if (sa->serial != sb->serial) return sa->serial - sb->serial;
    return sb->kind - sa->kind;  /* LOAD (1) before STORE (0) */
}

/* ----------------------------------------------------------------
 * Pending segments — reload segments created by splits that haven't
 * been processed yet (their start is in the future).
 * ---------------------------------------------------------------- */
typedef struct {
    int idx;           /* interval index */
    Segment *seg;      /* the reload segment */
} PendingEntry;

#define MAX_PENDING 512
static PendingEntry pending[MAX_PENDING];
static int n_pending;

static void pending_add(int idx, Segment *seg) {
    if (n_pending >= MAX_PENDING) {
        fprintf(stderr, "linscan: too many pending segments\n");
        return;
    }
    /* Insert sorted by seg->start */
    int k = n_pending++;
    while (k > 0 && pending[k-1].seg->start > seg->start) {
        pending[k] = pending[k-1];
        k--;
    }
    pending[k].idx = idx;
    pending[k].seg = seg;
}

/* ----------------------------------------------------------------
 * Spill state
 * ---------------------------------------------------------------- */
static int spill_next_off;
static int n_spills;

static int alloc_spill_slot(void) {
    spill_next_off -= 4;
    spill_next_off &= ~3;
    n_spills++;
    return spill_next_off;
}

/* ----------------------------------------------------------------
 * Segment allocation
 * ---------------------------------------------------------------- */
static Segment *new_segment(int start, int end) {
    Segment *s = calloc(1, sizeof(Segment));
    s->start = start;
    s->end = end;
    s->phys = -1;
    s->next = NULL;
    return s;
}

/* ----------------------------------------------------------------
 * Helpers to record definition / use of a vreg
 * ---------------------------------------------------------------- */
static void record_def(int vreg, int serial) {
    if (vreg <= IR3_VREG_ACCUM) return;
    int idx = vreg - IR3_VREG_BASE;
    if (idx >= MAX_INTERVALS) {
        fprintf(stderr, "linscan: vreg %d out of range\n", vreg);
        return;
    }
    if (vreg_map[idx] == -1) {
        if (n_intervals >= MAX_INTERVALS) {
            fprintf(stderr, "linscan: too many intervals\n");
            return;
        }
        int k = n_intervals++;
        intervals[k].vreg = vreg;
        intervals[k].orig_start = serial;
        intervals[k].orig_end = serial;
        intervals[k].segments = NULL;
        intervals[k].spill_off = -1;
        vreg_map[idx] = k;
    } else {
        int k = vreg_map[idx];
        if (serial < intervals[k].orig_start) intervals[k].orig_start = serial;
        if (serial > intervals[k].orig_end)   intervals[k].orig_end   = serial;
    }
}

static void record_use(int vreg, int serial) {
    if (vreg <= IR3_VREG_ACCUM) return;
    int idx = vreg - IR3_VREG_BASE;
    if (idx >= MAX_INTERVALS) return;
    if (vreg_map[idx] == -1) return;
    int k = vreg_map[idx];
    if (serial > intervals[k].orig_end) intervals[k].orig_end = serial;
}

/* Comparator for sorting intervals by orig_start. */
static int cmp_by_start(const void *a, const void *b) {
    const SplitInterval *ia = (const SplitInterval *)a;
    const SplitInterval *ib = (const SplitInterval *)b;
    return ia->orig_start - ib->orig_start;
}

/* Comparator for ints (for sorting use positions). */
static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

/* ----------------------------------------------------------------
 * IR3 node helpers
 * ---------------------------------------------------------------- */
static IR3Inst *new_ir3(IR3Op op) {
    IR3Inst *n = calloc(1, sizeof(IR3Inst));
    n->op  = op;
    n->rd  = IR3_VREG_NONE;
    n->rs1 = IR3_VREG_NONE;
    n->rs2 = IR3_VREG_NONE;
    return n;
}

static void insert_after(IR3Inst *after, IR3Inst *ins) {
    ins->next = after->next;
    after->next = ins;
}

static void insert_before(IR3Inst *prev, IR3Inst *target, IR3Inst *ins) {
    ins->next = target;
    if (prev) prev->next = ins;
}

/* ----------------------------------------------------------------
 * Find the segment covering a given serial for a vreg.
 * Returns the physical register, or -1 if in a gap (error).
 * ---------------------------------------------------------------- */
static int phys_at_serial(int vreg, int serial) {
    if (vreg == IR3_VREG_ACCUM) return 0;
    if (vreg < IR3_VREG_BASE) return vreg;
    int idx = vreg - IR3_VREG_BASE;
    if (idx < 0 || idx >= MAX_INTERVALS) return vreg;
    int k = vreg_map[idx];
    if (k < 0) {
        fprintf(stderr, "linscan: vreg %d unmapped at serial %d\n", vreg, serial);
        return vreg;
    }
    for (Segment *s = intervals[k].segments; s; s = s->next) {
        if (serial >= s->start && serial <= s->end)
            return s->phys;
    }
    fprintf(stderr, "linscan: vreg %d has no segment at serial %d\n", vreg, serial);
    return vreg;
}

/* ----------------------------------------------------------------
 * Process one interval or pending segment during Phase 3.
 * Returns the register assigned, or -1 if deferred (pathological).
 * ---------------------------------------------------------------- */
static int process_interval(int iv_idx, Segment *seg, int *serial_to_line, int n_insts) {
    int P = seg->start;

    /* Expire old active segments */
    for (int j = 0; j < n_active; ) {
        if (active[j].seg->end < P) {
            pool_free(active[j].seg->phys);
            active_remove(j);
        } else {
            j++;
        }
    }

    /* Try to allocate */
    int reg = pool_alloc();
    if (reg >= 0) {
        seg->phys = reg;
        active_add(iv_idx, seg);
        return reg;
    }

    /* Pool empty — find active entry with furthest next-ref */
    int victim_pos = -1;
    int victim_next_ref = -1;
    for (int j = 0; j < n_active; j++) {
        int nu = next_ref_after(active[j].idx, P, NULL);
        if (nu > victim_next_ref) {
            victim_next_ref = nu;
            victim_pos = j;
        }
    }

    /* Check if current interval's next ref is further than all active */
    int my_next_ref = next_ref_after(iv_idx, P, NULL);

    if (victim_pos >= 0 && victim_next_ref > my_next_ref) {
        /* Split the victim, give its register to us */
        int v_idx = active[victim_pos].idx;
        Segment *v_seg = active[victim_pos].seg;
        reg = v_seg->phys;

        /* Allocate spill slot for victim if needed */
        int v_first_spill = (intervals[v_idx].spill_off == -1);
        if (v_first_spill)
            intervals[v_idx].spill_off = alloc_spill_slot();

        /* Record STORE: save victim's register after serial P-1 */
        int store_serial = P - 1;
        if (store_serial < 0) store_serial = 0;
        int line = (store_serial < n_insts) ? serial_to_line[store_serial] : 0;
        record_split_action(0, store_serial, reg, intervals[v_idx].spill_off, line);

        /* If this is the first spill for this vreg, also record a STORE at
         * the definition point.  Since SSA values never change, this ensures
         * the spill slot is always populated regardless of control-flow path,
         * eliminating the need for edge stores at branch points. */
        if (v_first_spill) {
            int def_serial = intervals[v_idx].orig_start;
            int def_phys = intervals[v_idx].segments->phys;
            int def_line = (def_serial < n_insts) ? serial_to_line[def_serial] : 0;
            record_split_action(0, def_serial, def_phys, intervals[v_idx].spill_off, def_line);
        }

        /* End victim's current segment.  If the victim has a reference
         * at serial P (e.g. as a source operand of the instruction where
         * the new interval starts), extend through P so phys_at_serial
         * can find the register.  Hardware reads before writes, so both
         * the victim (source) and new interval (dest) can share the
         * register at P.  The reload search starts from P either way. */
        {
            int has_ref_at_P = 0;
            int base = ref_pos_start[v_idx];
            int cnt = ref_pos_count[v_idx];
            for (int j = 0; j < cnt; j++) {
                if (ref_positions[base + j] == P) { has_ref_at_P = 1; break; }
                if (ref_positions[base + j] > P) break;
            }
            v_seg->end = has_ref_at_P ? P : P - 1;
        }

        /* Find victim's next reference after P and whether it's a def */
        int v_is_def = 0;
        int v_next = next_ref_after(v_idx, P, &v_is_def);
        if (v_next < 0x7fffffff) {
            /* Create reload segment for victim at its next reference */
            Segment *reload_seg = new_segment(v_next, intervals[v_idx].orig_end);
            reload_seg->next = v_seg->next;
            v_seg->next = reload_seg;
            /* Tag the segment: if next ref is a def, no LOAD needed */
            if (v_is_def)
                reload_seg->phys = -2;  /* sentinel: don't insert LOAD */
            pending_add(v_idx, reload_seg);
        }

        /* Remove victim from active */
        active_remove(victim_pos);

        /* Assign register to current segment */
        seg->phys = reg;
        active_add(iv_idx, seg);

        return reg;
    } else {
        /* Current interval has furthest next-ref — spill it.
         * We need a register briefly for the def. Split the victim
         * with furthest next-ref to borrow its register for one instruction. */
        if (victim_pos < 0) {
            fprintf(stderr, "linscan: no active entries to evict\n");
            return -1;
        }

        int v_idx = active[victim_pos].idx;
        Segment *v_seg = active[victim_pos].seg;
        reg = v_seg->phys;

        /* Allocate spill slots */
        int v_first_spill = (intervals[v_idx].spill_off == -1);
        if (v_first_spill)
            intervals[v_idx].spill_off = alloc_spill_slot();
        if (intervals[iv_idx].spill_off == -1)
            intervals[iv_idx].spill_off = alloc_spill_slot();

        /* Save victim before P */
        int store_serial = P - 1;
        if (store_serial < 0) store_serial = 0;
        int line = (store_serial < n_insts) ? serial_to_line[store_serial] : 0;
        record_split_action(0, store_serial, reg, intervals[v_idx].spill_off, line);

        /* Def-time STORE for victim if first spill */
        if (v_first_spill) {
            int def_serial = intervals[v_idx].orig_start;
            int def_phys = intervals[v_idx].segments->phys;
            int def_line = (def_serial < n_insts) ? serial_to_line[def_serial] : 0;
            record_split_action(0, def_serial, def_phys, intervals[v_idx].spill_off, def_line);
        }

        /* End victim's segment (extend through P if victim has a ref at P) */
        {
            int has_ref_at_P = 0;
            int base = ref_pos_start[v_idx];
            int cnt = ref_pos_count[v_idx];
            for (int j = 0; j < cnt; j++) {
                if (ref_positions[base + j] == P) { has_ref_at_P = 1; break; }
                if (ref_positions[base + j] > P) break;
            }
            v_seg->end = has_ref_at_P ? P : P - 1;
        }
        int v_is_def = 0;
        int v_next = next_ref_after(v_idx, P, &v_is_def);
        if (v_next < 0x7fffffff) {
            Segment *v_reload = new_segment(v_next, intervals[v_idx].orig_end);
            v_reload->next = v_seg->next;
            v_seg->next = v_reload;
            if (v_is_def) v_reload->phys = -2;
            pending_add(v_idx, v_reload);
        }
        active_remove(victim_pos);

        /* Give register to current interval for its def, then immediately spill */
        seg->phys = reg;
        seg->end = P;  /* only one instruction */

        /* Store current interval's value right after its def */
        int def_line = (P < n_insts) ? serial_to_line[P] : 0;
        record_split_action(0, P, reg, intervals[iv_idx].spill_off, def_line);

        /* Create reload segment for current interval at its first ref */
        int my_is_def = 0;
        int my_first_ref = next_ref_after(iv_idx, P, &my_is_def);
        if (my_first_ref < 0x7fffffff) {
            Segment *my_reload = new_segment(my_first_ref, intervals[iv_idx].orig_end);
            my_reload->next = seg->next;
            seg->next = my_reload;
            if (my_is_def) my_reload->phys = -2;
            pending_add(iv_idx, my_reload);
        }

        active_add(iv_idx, seg);

        return reg;
    }
}

/* ----------------------------------------------------------------
 * alloc_function — allocate registers for one function
 * ---------------------------------------------------------------- */
static void alloc_function(IR3Inst *func_head, IR3Inst *func_end,
                           IR3Inst *enter_node)
{
    /* --- Phase 1: number instructions and compute live intervals --- */
    n_intervals = 0;
    n_spills = 0;
    n_split_actions = 0;
    n_ref_positions = 0;
    n_pending = 0;
    memset(vreg_map, -1, sizeof(vreg_map));

    /* Compute spill-slot floor from deepest bp-relative offset. */
    int enter_N = enter_node ? enter_node->imm : 0;
    int min_bp_off = -enter_N;
    int adj_depth  = -enter_N;

    for (IR3Inst *p = func_head; p != func_end; p = p->next) {
        if (p->op == IR3_LOAD && p->rs1 == IR3_VREG_BP) {
            if (p->imm < min_bp_off) min_bp_off = p->imm;
        }
        if (p->op == IR3_STORE && p->rd == IR3_VREG_BP) {
            if (p->imm < min_bp_off) min_bp_off = p->imm;
        }
        if (p->op == IR3_LEA) {
            if (p->imm < min_bp_off) min_bp_off = p->imm;
        }
        if (p->op == IR3_ADJ) {
            adj_depth += p->imm;
            if (adj_depth < min_bp_off) min_bp_off = adj_depth;
        }
    }

    spill_next_off = min_bp_off & ~3;
    if (spill_next_off >= min_bp_off)
        spill_next_off -= 4;

    /* Build label_id → serial mapping for back-edge detection. */
    #define MAX_LABELS 1024
    int label_serial[MAX_LABELS];
    memset(label_serial, -1, sizeof(label_serial));

    /* Count instructions for serial_to_line array. */
    int n_insts = 0;
    for (IR3Inst *p = func_head; p != func_end; p = p->next)
        n_insts++;

    int *serial_to_line = calloc(n_insts, sizeof(int));
    serial_to_node = calloc(n_insts, sizeof(IR3Inst *));

    int serial = 0;
    for (IR3Inst *p = func_head; p != func_end; p = p->next, serial++) {
        serial_to_line[serial] = p->line;
        serial_to_node[serial] = p;
        if (p->op == IR3_LABEL && p->imm >= 0 && p->imm < MAX_LABELS)
            label_serial[p->imm] = serial;

        /* Definition: rd is a def for most ops.
         * IR3_STORE's rd is a use (base address), but we still call
         * record_def to ensure the interval is created if this is the
         * first reference.  The start/end math is the same either way. */
        if (p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP)
            record_def(p->rd, serial);
        /* Uses */
        if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP)
            record_use(p->rs1, serial);
        if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP)
            record_use(p->rs2, serial);
    }

    if (n_intervals == 0) {
        free(serial_to_line);
        return;
    }

    /* Sort each interval's use positions. Since we appended them in order,
     * they should already be sorted, but sort to be safe. */
    for (int i = 0; i < n_intervals; i++) {
        if (ref_pos_count[i] > 1) {
            qsort(&ref_positions[ref_pos_start[i]], ref_pos_count[i],
                  sizeof(int), cmp_int);
        }
    }

    /* --- Phase 1b: extend intervals across loop back-edges --- */
    {
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

        for (int iter = 0; iter < 4 && n_back_edges > 0; iter++) {
            int changed = 0;
            for (int bi = 0; bi < n_back_edges; bi++) {
                int hdr = back_edges[bi].header;
                int jmp = back_edges[bi].jump;
                for (int i = 0; i < n_intervals; i++) {
                    if (intervals[i].orig_start <= hdr && intervals[i].orig_end >= hdr
                        && intervals[i].orig_end < jmp) {
                        intervals[i].orig_end = jmp;
                        changed = 1;
                    }
                }
            }
            if (!changed) break;
        }
    }

    /* --- Phase 2: create single-segment intervals, sort by start --- */
    for (int i = 0; i < n_intervals; i++) {
        intervals[i].segments = new_segment(intervals[i].orig_start,
                                            intervals[i].orig_end);
    }

    qsort(intervals, n_intervals, sizeof(SplitInterval), cmp_by_start);

    /* Rebuild vreg_map after sort.  Also fix ref_pos_start/count. */
    /* We need to sort use positions along with intervals.  Since qsort
     * moved intervals around, we need to rebuild the use-position index.
     * The simplest approach: save and rebuild. */
    {
        /* Save old use-pos data keyed by vreg */
        typedef struct { int start; int count; } UPInfo;
        UPInfo up_old[MAX_INTERVALS];
        /* Before sort, the use_pos arrays were indexed by old interval index.
         * After sort, intervals[i] has a new vreg.  We need to map via vreg. */
        /* Actually, ref_pos_start/count moved with the intervals during qsort
         * only if they're part of the struct.  They're separate arrays, so they
         * didn't move.  We need to fix this. */

        /* Build a temporary map: vreg → old index */
        /* We already have vreg_map from before sort, but it was indexed by
         * vreg - IR3_VREG_BASE → old interval index.  After sort, intervals
         * are in new positions.  Let's just rebuild use pos tracking. */

        /* Copy the old use-pos info indexed by vreg */
        int old_up_start[MAX_INTERVALS];
        int old_up_count[MAX_INTERVALS];
        memcpy(old_up_start, ref_pos_start, n_intervals * sizeof(int));
        memcpy(old_up_count, ref_pos_count, n_intervals * sizeof(int));

        /* vreg_map still has old mapping.  Build reverse: old_index → vreg */
        /* Actually simpler: for each new position i, intervals[i].vreg tells us
         * which vreg it is.  We can look up the old index via vreg_map. */
        memset(vreg_map, -1, sizeof(vreg_map));
        for (int i = 0; i < n_intervals; i++) {
            int vidx = intervals[i].vreg - IR3_VREG_BASE;
            if (vidx >= 0 && vidx < MAX_INTERVALS)
                vreg_map[vidx] = i;
        }

        /* Now rebuild ref_pos_start/count for new ordering.
         * We need to find the old index for each interval.  The old index
         * is no longer stored.  But we can search by vreg in the old vreg_map.
         * However vreg_map was just overwritten.  Let's take a different approach:
         * just re-scan the IR to rebuild use positions from scratch. */

        /* Rebuild reference positions with two passes: count, then fill.
         * Records ALL references (defs and uses) so split decisions cover
         * every instruction that touches the vreg. */
        for (int i = 0; i < n_intervals; i++)
            ref_pos_count[i] = 0;

        /* Pass 1: count references per interval */
        serial = 0;
        for (IR3Inst *p = func_head; p != func_end; p = p->next, serial++) {
            /* rd: def (or use for STORE) — either way, a reference */
            if (p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP
                && p->rd > IR3_VREG_ACCUM) {
                int k = vreg_map[p->rd - IR3_VREG_BASE];
                if (k >= 0) ref_pos_count[k]++;
            }
            if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP
                && p->rs1 > IR3_VREG_ACCUM) {
                int k = vreg_map[p->rs1 - IR3_VREG_BASE];
                if (k >= 0) ref_pos_count[k]++;
            }
            if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP
                && p->rs2 > IR3_VREG_ACCUM) {
                int k = vreg_map[p->rs2 - IR3_VREG_BASE];
                if (k >= 0) ref_pos_count[k]++;
            }
        }

        /* Assign start positions */
        n_ref_positions = 0;
        for (int i = 0; i < n_intervals; i++) {
            ref_pos_start[i] = n_ref_positions;
            n_ref_positions += ref_pos_count[i];
        }
        if (n_ref_positions > MAX_REF_POS)
            n_ref_positions = MAX_REF_POS;

        /* Pass 2: fill reference positions with def/use tags */
        int fill_cursor[MAX_INTERVALS];
        for (int i = 0; i < n_intervals; i++)
            fill_cursor[i] = ref_pos_start[i];

        serial = 0;
        for (IR3Inst *p = func_head; p != func_end; p = p->next, serial++) {
            /* rd: DEF for most ops; USE for IR3_STORE (base address) */
            if (p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP
                && p->rd > IR3_VREG_ACCUM) {
                int k = vreg_map[p->rd - IR3_VREG_BASE];
                if (k >= 0 && fill_cursor[k] < MAX_REF_POS) {
                    int pos = fill_cursor[k]++;
                    ref_positions[pos] = serial;
                    ref_is_def[pos] = (p->op != IR3_STORE) ? 1 : 0;
                }
            }
            if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP
                && p->rs1 > IR3_VREG_ACCUM) {
                int k = vreg_map[p->rs1 - IR3_VREG_BASE];
                if (k >= 0 && fill_cursor[k] < MAX_REF_POS) {
                    int pos = fill_cursor[k]++;
                    ref_positions[pos] = serial;
                    ref_is_def[pos] = 0;
                }
            }
            if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP
                && p->rs2 > IR3_VREG_ACCUM) {
                int k = vreg_map[p->rs2 - IR3_VREG_BASE];
                if (k >= 0 && fill_cursor[k] < MAX_REF_POS) {
                    int pos = fill_cursor[k]++;
                    ref_positions[pos] = serial;
                    ref_is_def[pos] = 0;
                }
            }
        }

        /* Deduplicate (same serial can appear multiple times if vreg is
         * used in multiple operand slots of the same instruction).
         * If any entry at a serial is a USE, keep it as USE (conservative). */
        for (int i = 0; i < n_intervals; i++) {
            int base = ref_pos_start[i];
            int cnt = fill_cursor[i] - base;
            if (cnt <= 1) { ref_pos_count[i] = cnt; continue; }
            int out = 1;
            for (int j = 1; j < cnt; j++) {
                if (ref_positions[base + j] != ref_positions[base + out - 1]) {
                    ref_positions[base + out] = ref_positions[base + j];
                    ref_is_def[base + out] = ref_is_def[base + j];
                    out++;
                } else {
                    /* Same serial: if either is a use, mark as use */
                    if (!ref_is_def[base + j])
                        ref_is_def[base + out - 1] = 0;
                }
            }
            ref_pos_count[i] = out;
        }
    }

    /* --- Phase 3: linear scan with splitting --- */
    pool_init();
    n_active = 0;
    n_pending = 0;

    int iv_cursor = 0;  /* next interval to process from sorted list */

    while (iv_cursor < n_intervals || n_pending > 0) {
        /* Pick the next item: either from the sorted interval list or
         * from the pending reload segments, whichever starts earlier. */
        int use_pending = 0;
        if (iv_cursor >= n_intervals) {
            use_pending = 1;
        } else if (n_pending > 0) {
            if (pending[0].seg->start <= intervals[iv_cursor].segments->start)
                use_pending = 1;
        }

        int idx;
        Segment *seg;

        int needs_load = 0;  /* whether this reload segment needs a LOAD */

        if (use_pending) {
            idx = pending[0].idx;
            seg = pending[0].seg;
            /* Check if this reload segment needs a LOAD instruction.
             * phys == -2 means the next reference is a DEF (no load needed).
             * phys == -1 means the next reference is a USE (load needed). */
            needs_load = (seg->phys != -2);
            seg->phys = -1;  /* reset sentinel before processing */
            /* Remove from pending */
            for (int i = 0; i < n_pending - 1; i++)
                pending[i] = pending[i+1];
            n_pending--;
        } else {
            idx = iv_cursor;
            seg = intervals[iv_cursor].segments;
            iv_cursor++;
        }

        int assigned = process_interval(idx, seg, serial_to_line, n_insts);

        /* If this was a reload segment that needs a LOAD, record the action */
        if (use_pending && needs_load && assigned >= 0) {
            int line = (seg->start < n_insts) ? serial_to_line[seg->start] : 0;
            record_split_action(1, seg->start, assigned,
                               intervals[idx].spill_off, line);
        }
    }

    /* --- Phase 4: rewrite IR3Inst registers --- */
    serial = 0;
    for (IR3Inst *p = func_head; p != func_end; p = p->next, serial++) {
        if (p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP)
            p->rd = phys_at_serial(p->rd, serial);
        if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP)
            p->rs1 = phys_at_serial(p->rs1, serial);
        if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP)
            p->rs2 = phys_at_serial(p->rs2, serial);
    }

    /* --- Phase 4b: control-flow edge resolution --- */
    /* When a split interval has different physical registers at a jump
     * source vs the target label, insert MOVs to reconcile.  For
     * unconditional jumps, MOVs are inserted before the jump.  For
     * conditional jumps, a resolution block is created:
     *   JZ _target  →  JNZ _skip; <MOVs>; J _target; _skip:
     * Multiple MOVs are topologically sorted (parallel move problem). */
    {
        /* Find max label ID for allocating resolution labels */
        int resolve_label = 0;
        for (IR3Inst *p = func_head; p != func_end; p = p->next)
            if (p->op == IR3_LABEL && p->imm >= resolve_label)
                resolve_label = p->imm + 1;

        serial = 0;
        IR3Inst *prev_node = NULL;
        for (IR3Inst *p = func_head; p != func_end; ) {
            if ((p->op == IR3_J || p->op == IR3_JZ || p->op == IR3_JNZ)
                && p->imm >= 0 && p->imm < MAX_LABELS
                && label_serial[p->imm] >= 0)
            {
                int target_serial = label_serial[p->imm];

                /* Collect resolution moves */
                struct { int dst; int src; } moves[8];
                int n_moves = 0;

                for (int i = 0; i < n_intervals; i++) {
                    int phys_src = -1, phys_dst = -1;
                    for (Segment *s = intervals[i].segments; s; s = s->next) {
                        if (s->start <= serial && serial <= s->end && s->phys >= 0)
                            phys_src = s->phys;
                        if (s->start <= target_serial && target_serial <= s->end && s->phys >= 0)
                            phys_dst = s->phys;
                    }
                    if (phys_src >= 0 && phys_dst >= 0 && phys_src != phys_dst) {
                        if (n_moves < 8) {
                            moves[n_moves].dst = phys_dst;
                            moves[n_moves].src = phys_src;
                            n_moves++;
                        }
                    }
                    /* No edge stores needed: def-time STOREs ensure spill slots
                     * are always populated from the definition point. */
                }

                if (n_moves > 0) {
                    /* For conditional branches, create a resolution block:
                     * Replace: JZ target  with: JNZ skip; <MOVs>; J target; skip:
                     * For unconditional: insert MOVs before the jump. */
                    int is_conditional = (p->op == IR3_JZ || p->op == IR3_JNZ);
                    IR3Inst *insert_point; /* MOVs are inserted before this node */
                    IR3Inst *insert_prev;  /* previous node for linking */

                    if (is_conditional) {
                        /* Create: <inverse branch> _skip; ... <MOVs> ... J _target; _skip: */
                        int skip_label = resolve_label++;
                        int orig_target = p->imm;

                        /* Change the conditional branch to the inverse, targeting skip */
                        IR3Op inverse = (p->op == IR3_JZ) ? IR3_JNZ : IR3_JZ;
                        p->op = inverse;
                        p->imm = skip_label;

                        /* Create: J original_target (after MOVs) */
                        IR3Inst *jmp = calloc(1, sizeof(IR3Inst));
                        jmp->op = IR3_J;
                        jmp->imm = orig_target;
                        jmp->rd = IR3_VREG_NONE;
                        jmp->rs1 = IR3_VREG_NONE;
                        jmp->rs2 = IR3_VREG_NONE;
                        jmp->line = p->line;

                        /* Create: _skip label */
                        IR3Inst *lbl = calloc(1, sizeof(IR3Inst));
                        lbl->op = IR3_LABEL;
                        lbl->imm = skip_label;
                        lbl->rd = IR3_VREG_NONE;
                        lbl->rs1 = IR3_VREG_NONE;
                        lbl->rs2 = IR3_VREG_NONE;
                        lbl->line = p->line;

                        /* Link: p (now inverse branch) → <MOVs> → jmp → lbl → p->next */
                        lbl->next = p->next;
                        jmp->next = lbl;
                        /* MOVs will be inserted between p and jmp */
                        insert_point = jmp;
                        insert_prev = p;
                    } else {
                        /* Unconditional: insert MOVs before p */
                        insert_point = p;
                        insert_prev = prev_node;
                    }

                    /* Topological sort: emit moves whose dst is not a remaining src */
                    int done[8] = {0};
                    int emitted_count = 0;
                    while (emitted_count < n_moves) {
                        int progress = 0;
                        for (int i = 0; i < n_moves; i++) {
                            if (done[i]) continue;
                            int blocked = 0;
                            for (int j = 0; j < n_moves; j++) {
                                if (!done[j] && j != i && moves[j].src == moves[i].dst) {
                                    blocked = 1;
                                    break;
                                }
                            }
                            if (!blocked) {
                                IR3Inst *mov = calloc(1, sizeof(IR3Inst));
                                mov->op = IR3_MOV;
                                mov->rd = moves[i].dst;
                                mov->rs1 = moves[i].src;
                                mov->rs2 = IR3_VREG_NONE;
                                mov->line = p->line;
                                mov->next = insert_point;
                                if (insert_prev) insert_prev->next = mov;
                                insert_prev = mov;
                                done[i] = 1;
                                emitted_count++;
                                progress = 1;
                            }
                        }
                        if (!progress) {
                            /* Cycle: break with XOR swap */
                            int i;
                            for (i = 0; i < n_moves; i++)
                                if (!done[i]) break;
                            int a = moves[i].dst, b = moves[i].src;
                            for (int k = 0; k < 3; k++) {
                                IR3Inst *x = calloc(1, sizeof(IR3Inst));
                                x->op = IR3_ALU;
                                x->alu_op = IR_XOR;
                                x->rd = (k == 1) ? b : a;
                                x->rs1 = a;
                                x->rs2 = b;
                                x->line = p->line;
                                x->next = insert_point;
                                if (insert_prev) insert_prev->next = x;
                                insert_prev = x;
                            }
                            done[i] = 1;
                            emitted_count++;
                            for (int j = 0; j < n_moves; j++) {
                                if (!done[j]) {
                                    if (moves[j].src == a) moves[j].src = b;
                                    else if (moves[j].src == b) moves[j].src = a;
                                }
                            }
                        }
                    }

                    if (is_conditional) {
                        /* For conditional: link the last MOV to jmp→lbl chain */
                        /* insert_prev is already pointing to insert_point (jmp) */
                        /* Advance prev_node past the entire resolution block */
                        /* Find the label node at the end */
                        IR3Inst *q = insert_point; /* jmp node */
                        prev_node = q->next; /* lbl node */
                        p = prev_node->next; /* original p->next */
                        serial++;
                        continue;
                    }
                }
            }
            prev_node = p;
            p = p->next;
            serial++;
        }
    }

    /* --- Phase 5a: expand ENTER and shift flush offsets --- */
    if (n_spills > 0 && enter_node) {
        int orig_N = enter_node->imm;
        int total_frame = -spill_next_off;
        total_frame = (total_frame + 3) & ~3;
        int expansion = total_frame - orig_N;

        if (expansion > 0) {
            enter_node->imm = total_frame;

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

    /* (Edge store sentinel cleanup removed — def-time STOREs replaced edge stores) */

    /* --- Apply split actions (insert STORE/LOAD nodes) --- */
    /* Uses target pointers (set during Phase 1) instead of serial counting,
     * so Phase 4b-inserted resolution MOVs don't affect placement. */
    if (n_split_actions > 0) {
        qsort(split_actions, n_split_actions, sizeof(SplitAction), cmp_split_action);

        int sa_idx = 0;
        IR3Inst *prev = NULL;

        for (IR3Inst *p = func_head; p != func_end && sa_idx < n_split_actions; ) {
            /* Insert LOADs (kind=1) BEFORE this instruction */
            while (sa_idx < n_split_actions
                   && split_actions[sa_idx].target == p
                   && split_actions[sa_idx].kind == 1) {
                IR3Inst *ld = new_ir3(IR3_LOAD);
                ld->rd   = split_actions[sa_idx].phys_reg;
                ld->rs1  = IR3_VREG_BP;
                ld->imm  = split_actions[sa_idx].spill_off;
                ld->size = 4;
                ld->line = split_actions[sa_idx].line;
                insert_before(prev, p, ld);
                if (!prev) func_head = ld;
                prev = ld;
                sa_idx++;
            }

            IR3Inst *next = p->next;

            /* Insert STOREs (kind=0) AFTER this instruction */
            while (sa_idx < n_split_actions
                   && split_actions[sa_idx].target == p
                   && split_actions[sa_idx].kind == 0) {
                IR3Inst *st = new_ir3(IR3_STORE);
                st->rd   = IR3_VREG_BP;
                st->rs1  = split_actions[sa_idx].phys_reg;
                st->imm  = split_actions[sa_idx].spill_off;
                st->size = 4;
                st->line = split_actions[sa_idx].line;
                insert_after(p, st);
                p = st;  /* advance past the store */
                sa_idx++;
            }

            prev = p;
            p = next;
        }
    }

    /* Free segments */
    for (int i = 0; i < n_intervals; i++) {
        Segment *s = intervals[i].segments;
        while (s) {
            Segment *next = s->next;
            free(s);
            s = next;
        }
        intervals[i].segments = NULL;
    }

    free(serial_to_line);
    free(serial_to_node);
    serial_to_node = NULL;
}

/* ----------------------------------------------------------------
 * linscan_regalloc — public entry point.
 * ---------------------------------------------------------------- */
void linscan_regalloc(IR3Inst *head)
{
    IR3Inst *func_start = NULL;
    IR3Inst *enter_node = NULL;

    for (IR3Inst *p = head; p; p = p->next) {
        if (p->op == IR3_SYMLABEL) {
            if (func_start)
                alloc_function(func_start, p, enter_node);
            func_start = p->next;
            enter_node = NULL;
            for (IR3Inst *q = p->next; q; q = q->next) {
                if (q->op == IR3_ENTER) { enter_node = q; break; }
                if (q->op == IR3_SYMLABEL) break;
            }
        }
    }
    if (func_start)
        alloc_function(func_start, NULL, enter_node);

    /* Final pass: rewrite ACCUM everywhere. */
    for (IR3Inst *p = head; p; p = p->next) {
        if (p->rd  == IR3_VREG_ACCUM) p->rd  = 0;
        if (p->rs1 == IR3_VREG_ACCUM) p->rs1 = 0;
        if (p->rs2 == IR3_VREG_ACCUM) p->rs2 = 0;
    }
}
