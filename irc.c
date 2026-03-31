/* irc.c — Iterated Register Coalescing (Appel & George 1996) for IR3.
 *
 * Replaces linscan.c + call_spill.c.  Runs per function between
 * IR3_SYMLABEL nodes.  Rewrites rd/rs1/rs2 in-place to physical r0–r7.
 *
 * Virtual register encoding on input:
 *   IR3_VREG_ACCUM (100) → pre-colored node 0 (physical r0)
 *   fresh vreg v (≥ 101) → node IRC_PHYS + (v - 101)
 *   IR3_VREG_NONE (-1) and IR3_VREG_BP (-2) → pass through unchanged
 *
 * K = 7: r1–r7 are allocatable; r0 is reserved for ACCUM (pre-colored).
 * r0–r3 are caller-saved; r4–r7 are callee-saved.
 * CALL/CALLR: interfere live vregs with r0–r3 only (caller-saved); r4–r7
 * survive calls because callees save/restore them (insert_callee_saves).
 * No separate call_spill pass needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ir3.h"

extern int new_label(void);

/* ================================================================
 * Constants
 * ================================================================ */
#define IRC_K            7           /* allocatable: r1-r7                  */
#define IRC_PHYS         8           /* total physical regs r0-r7            */
#define IRC_MAX_VREGS    4096        /* max fresh vregs per function+spills  */
#define IRC_MAX_NODES    (IRC_PHYS + IRC_MAX_VREGS)   /* 4104               */
#define IRC_WORDS        ((IRC_MAX_NODES + 63) / 64)  /* 17                 */
#define IRC_MAX_MOVES    2048
#define IRC_MAX_BB       512         /* was 256; fatal exit if exceeded       */
#define IRC_MAX_ITERS    6           /* spill-rewrite restart limit           */
#define BB_INST_MAX      4096        /* max instructions per BB for rev scan  */

/* ================================================================
 * Node index encoding:
 *   physical r0..r7    →  0..7
 *   ACCUM (vreg 100)   →  0   (pre-colored r0)
 *   fresh vreg v≥101   →  IRC_PHYS + (v - 101)  =  v - 93
 * ================================================================ */
static int vreg_to_node(int v)
{
    if (v == IR3_VREG_NONE || v == IR3_VREG_BP) return -1;
    if (v == IR3_VREG_ACCUM) return 0;
    if (v >= 0 && v < IRC_PHYS) return v;          /* physical (post-rewrite) */
    if (v > IR3_VREG_ACCUM) {
        int n = IRC_PHYS + (v - (IR3_VREG_BASE + 1));
        if (n >= IRC_MAX_NODES) {
            fprintf(stderr, "irc: fatal: vreg %d maps to node %d, exceeds IRC_MAX_NODES=%d\n",
                    v, n, IRC_MAX_NODES);
            exit(1);
        }
        return n;
    }
    return -1;
}

static inline bool is_precolored(int n) { return n >= 0 && n < IRC_PHYS; }
static inline bool is_virtual(int n)    { return n >= IRC_PHYS && n < IRC_MAX_NODES; }

/* ================================================================
 * Bitset (IRC_WORDS × 64 bits = 1088 bits, covering all 1032 nodes)
 * ================================================================ */
typedef uint64_t Bitset[IRC_WORDS];

static void bs_set(Bitset b, int i)
    { if (i>=0 && i<IRC_MAX_NODES) b[i>>6] |=  (1ULL<<(i&63)); }
static void bs_clr(Bitset b, int i)
    { if (i>=0 && i<IRC_MAX_NODES) b[i>>6] &= ~(1ULL<<(i&63)); }
static bool bs_test(const Bitset b, int i)
    { return i>=0 && i<IRC_MAX_NODES && ((b[i>>6]>>(i&63))&1); }
static void bs_zero(Bitset b)    { memset(b, 0, IRC_WORDS*8); }
static void bs_copy(Bitset d, const Bitset s) { memcpy(d, s, IRC_WORDS*8); }
static bool bs_empty(const Bitset b) {
    for (int i=0;i<IRC_WORDS;i++) if (b[i]) return false; return true; }
static void bs_or(Bitset d, const Bitset s) {
    for (int i=0;i<IRC_WORDS;i++) d[i]|=s[i]; }
static bool bs_equal(const Bitset a, const Bitset b) {
    for (int i=0;i<IRC_WORDS;i++) if (a[i]!=b[i]) return false; return true; }

/* Iterate: for(int n=bs_next(b,-1); n>=0; n=bs_next(b,n)) */
static int bs_next(const Bitset b, int after)
{
    int start = after + 1;
    for (int w = start>>6; w < IRC_WORDS; w++) {
        uint64_t word = b[w];
        int bit = (w == start>>6) ? (start & 63) : 0;
        word >>= bit;
        if (word) return (w<<6) | (bit + __builtin_ctzll(word));
    }
    return -1;
}

/* ================================================================
 * Per-function state (static globals; reset in irc_reset())
 * ================================================================ */

/* Interference graph */
static Bitset imat[IRC_MAX_NODES];      /* symmetric bitset matrix           */
static int    degree[IRC_MAX_NODES];    /* interference degree               */

/* Colors and aliases */
static int color_of[IRC_MAX_NODES];     /* -1 = uncolored                    */
static int alias_of[IRC_MAX_NODES];     /* alias_of[n]=n if not coalesced    */
static Bitset coalesced_set;            /* nodes that have been coalesced    */

/* Move instructions */
#define MOVE_WORKLIST    0
#define MOVE_ACTIVE      1
#define MOVE_COALESCED   2
#define MOVE_CONSTRAINED 3
#define MOVE_FROZEN      4

typedef struct { int u, v, state; } IRCMove;
static IRCMove  g_moves[IRC_MAX_MOVES];
static int      n_moves;

/* Per-node move lists: scratch-allocated linked list per function.
 * node_move_head[n] = head of the singly-linked list of moves involving node n. */
typedef struct MoveNode { int mi; struct MoveNode *next; } MoveNode;
static MoveNode **node_move_head;  /* [IRC_MAX_NODES], scratch-allocated in irc_reset_fn */

/* Worklists (bitsets for membership) */
static Bitset simplify_wl;
static Bitset freeze_wl;
static Bitset spill_wl;
static Bitset select_set;
static int    select_stack[IRC_MAX_NODES];
static int    select_sp;

/* Actual spills (after AssignColors) */
static Bitset actual_spill;
static int    spill_slot[IRC_MAX_NODES];  /* bp-relative offset, 0 = no spill */

/* Frame state for spill slot allocation */
static int g_enter_N;       /* original ENTER imm (frame size)           */
static int g_spill_next;    /* next slot offset (grows negative)         */

/* Highest node index seen in current function */
static int g_max_node;
/* Nodes that actually appeared as operands in the current function's IR */
static Bitset seen_nodes;

/* CFG / liveness */
typedef struct {
    IR3Inst *first;
    IR3Inst *last;
    int succs[2];
    int n_succs;
    int preds[BB_MAX_PREDS];  /* was preds[16]; BB_MAX_PREDS from ir3.h */
    int n_preds;
    int label_id;
    Bitset live_in;
    Bitset live_out;
} IRCBB;

static IRCBB g_bbs[IRC_MAX_BB];
static int   n_bbs;

/* Static scratch arrays (avoid stack blowup) */
static Bitset g_gen[IRC_MAX_BB];
static Bitset g_kill[IRC_MAX_BB];
static IR3Inst *g_rev[BB_INST_MAX];  /* reverse instruction buffer */

/* ================================================================
 * State reset for one function invocation
 * ================================================================ */
static void irc_reset_fn(void)
{
    memset(imat,          0, sizeof(imat));
    memset(degree,        0, sizeof(degree));
    memset(color_of,     -1, sizeof(color_of));
    for (int i = 0; i < IRC_MAX_NODES; i++) alias_of[i] = i;
    bs_zero(coalesced_set);
    n_moves = 0;
    node_move_head = scratch_alloc(IRC_MAX_NODES * sizeof(MoveNode *));
    /* zeroed by scratch_alloc, so all heads are NULL */
    bs_zero(simplify_wl);
    bs_zero(freeze_wl);
    bs_zero(spill_wl);
    bs_zero(select_set);
    select_sp = 0;
    bs_zero(actual_spill);
    // memset(spill_slot, 0, sizeof(spill_slot));
    bs_zero(seen_nodes);
    n_bbs = 0;
    g_max_node = IRC_PHYS - 1;
    /* Pre-colored nodes: color = self, infinite degree */
    for (int i = 0; i < IRC_PHYS; i++) {
        color_of[i] = i;
        degree[i]   = 0x7fffffff;   /* never pushed to select stack */
    }
}

/* ================================================================
 * Interference graph operations
 * ================================================================ */
static int get_alias(int n)
{
    while (bs_test(coalesced_set, n))
        n = alias_of[n];
    return n;
}

static void add_irc_edge(int u, int v)
{
    if (u < 0 || v < 0 || u == v)          return;
    if (u >= IRC_MAX_NODES || v >= IRC_MAX_NODES) return;
    if (bs_test(imat[u], v)) return;        /* already present */
    bs_set(imat[u], v);
    bs_set(imat[v], u);
    if (!is_precolored(u)) degree[u]++;
    if (!is_precolored(v)) degree[v]++;
}

/* Add interference from node u against all nodes set in 'live' */
static void add_edge_live(int u, const Bitset live)
{
    for (int t = bs_next(live, -1); t >= 0; t = bs_next(live, t))
        add_irc_edge(u, t);
}

/* Track highest node index seen, and mark as appeared in IR */
static void note_node(int n)
{
    if (n < 0 || n >= IRC_MAX_NODES) return;
    if (n > g_max_node) g_max_node = n;
    bs_set(seen_nodes, n);
}

/* ================================================================
 * Move helpers
 * ================================================================ */
static bool is_move_related(int n)
{
    for (MoveNode *mn = node_move_head[n]; mn; mn = mn->next) {
        int s = g_moves[mn->mi].state;
        if (s == MOVE_WORKLIST || s == MOVE_ACTIVE) return true;
    }
    return false;
}

static void record_move(int u, int v)
{
    if (n_moves >= IRC_MAX_MOVES) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "irc: warning: IRC_MAX_MOVES=%d exceeded; coalescing opportunities dropped\n",
                    IRC_MAX_MOVES);
            warned = true;
        }
        return;
    }
    /* Dedup */
    for (int i = 0; i < n_moves; i++)
        if ((g_moves[i].u == u && g_moves[i].v == v) ||
            (g_moves[i].u == v && g_moves[i].v == u)) return;
    int mi = n_moves++;
    g_moves[mi].u = u; g_moves[mi].v = v; g_moves[mi].state = MOVE_WORKLIST;
    /* Prepend to linked list for u and v (O(1), no limit) */
    MoveNode *mnu = scratch_alloc(sizeof(MoveNode));
    mnu->mi = mi; mnu->next = node_move_head[u]; node_move_head[u] = mnu;
    MoveNode *mnv = scratch_alloc(sizeof(MoveNode));
    mnv->mi = mi; mnv->next = node_move_head[v]; node_move_head[v] = mnv;
}

/* ================================================================
 * CFG reconstruction from IR3 list (for one function)
 * ================================================================ */
static int label_to_bb[2048];  /* label id → bb index (-1 if none) */

static void build_cfg_irc(IR3Inst *func_head, IR3Inst *func_end)
{
    n_bbs = 0;
    memset(label_to_bb, -1, sizeof(label_to_bb));

    /* Pass 1: identify BB leaders and assign labels */
    bool new_bb = true;
    for (IR3Inst *p = func_head; p && p != func_end; p = p->next) {
        bool is_label = (p->op == IR3_LABEL);
        if (new_bb || is_label) {
            if (n_bbs > 0 && is_label && g_bbs[n_bbs-1].last == NULL) {
                /* Previous BB is empty except for its leader; set last = prev */
                /* Actually this won't happen; continue */
            }
            if (n_bbs >= IRC_MAX_BB) {
                fprintf(stderr, "irc: fatal: function has >%d basic blocks (IRC_MAX_BB)\n",
                        IRC_MAX_BB);
                exit(1);
            }
            g_bbs[n_bbs].first    = p;
            g_bbs[n_bbs].last     = p;
            g_bbs[n_bbs].n_succs  = 0;
            g_bbs[n_bbs].n_preds  = 0;
            g_bbs[n_bbs].label_id = is_label ? p->imm : -1;
            bs_zero(g_bbs[n_bbs].live_in);
            bs_zero(g_bbs[n_bbs].live_out);
            if (is_label && p->imm >= 0) {
                if (p->imm >= 2048) {
                    fprintf(stderr, "irc: fatal: label id %d exceeds label_to_bb size 2048\n",
                            p->imm);
                    exit(1);
                }
                label_to_bb[p->imm] = n_bbs;
            }
            n_bbs++;
            new_bb = false;
        } else {
            g_bbs[n_bbs-1].last = p;
            if (p->op == IR3_LABEL && p->imm >= 0) {
                if (p->imm >= 2048) {
                    fprintf(stderr, "irc: fatal: label id %d exceeds label_to_bb size 2048\n",
                            p->imm);
                    exit(1);
                }
                label_to_bb[p->imm] = n_bbs - 1;
            }
        }
        if (p->op == IR3_J || p->op == IR3_JZ || p->op == IR3_JNZ || p->op == IR3_RET)
            new_bb = true;
    }

    /* Pass 2: build edges */
    for (int i = 0; i < n_bbs; i++) {
        IR3Inst *last = g_bbs[i].last;
        if (!last) continue;
        int op = last->op;
        if (op == IR3_RET) {
            /* no successors */
        } else if (op == IR3_J) {
            int tgt = (last->imm >= 0 && last->imm < 2048) ? label_to_bb[last->imm] : -1;
            if (tgt >= 0) {
                g_bbs[i].succs[g_bbs[i].n_succs++] = tgt;
                if (g_bbs[tgt].n_preds >= 16) {
                    fprintf(stderr, "irc: fatal: BB %d has >16 predecessors\n", tgt);
                    exit(1);
                }
                g_bbs[tgt].preds[g_bbs[tgt].n_preds++] = i;
            }
        } else if (op == IR3_JZ || op == IR3_JNZ) {
            /* fall-through */
            if (i+1 < n_bbs) {
                g_bbs[i].succs[g_bbs[i].n_succs++] = i+1;
                if (g_bbs[i+1].n_preds >= 16) {
                    fprintf(stderr, "irc: fatal: BB %d has >16 predecessors\n", i+1);
                    exit(1);
                }
                g_bbs[i+1].preds[g_bbs[i+1].n_preds++] = i;
            }
            /* branch target */
            int tgt = (last->imm >= 0 && last->imm < 2048) ? label_to_bb[last->imm] : -1;
            if (tgt >= 0 && tgt != i+1) {
                g_bbs[i].succs[g_bbs[i].n_succs++] = tgt;
                if (g_bbs[tgt].n_preds >= 16) {
                    fprintf(stderr, "irc: fatal: BB %d has >16 predecessors\n", tgt);
                    exit(1);
                }
                g_bbs[tgt].preds[g_bbs[tgt].n_preds++] = i;
            }
        } else {
            /* fall-through */
            if (i+1 < n_bbs) {
                g_bbs[i].succs[g_bbs[i].n_succs++] = i+1;
                if (g_bbs[i+1].n_preds >= 16) {
                    fprintf(stderr, "irc: fatal: BB %d has >16 predecessors\n", i+1);
                    exit(1);
                }
                g_bbs[i+1].preds[g_bbs[i+1].n_preds++] = i;
            }
        }
    }
}

/* ================================================================
 * Liveness analysis (backward dataflow over the CFG)
 * ================================================================ */

/* Add a use to gen/kill (upward exposed if not already killed) */
static void note_use(Bitset gen, const Bitset kill, int v)
{
    int n = vreg_to_node(v);
    if (n < 0) return;
    note_node(n);
    if (!bs_test(kill, n)) bs_set(gen, n);
}

/* Add a def to kill */
static void note_def(Bitset kill, int v)
{
    int n = vreg_to_node(v);
    if (n < 0) return;
    note_node(n);
    bs_set(kill, n);
}

static void compute_liveness(IR3Inst *func_head, IR3Inst *func_end)
{
    /* Compute per-BB gen and kill sets */
    for (int i = 0; i < n_bbs; i++) {
        bs_zero(g_gen[i]);
        bs_zero(g_kill[i]);
        for (IR3Inst *p = g_bbs[i].first; p && p != func_end; p = p->next) {
            /* Uses */
            if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP)
                note_use(g_gen[i], g_kill[i], p->rs1);
            if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP)
                note_use(g_gen[i], g_kill[i], p->rs2);
            /* STORE's rd is a use (base address), not a def */
            if (p->op == IR3_STORE && p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP)
                note_use(g_gen[i], g_kill[i], p->rd);
            /* Defs */
            if (p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP && p->op != IR3_STORE)
                note_def(g_kill[i], p->rd);
            /* Implicit use of r0 by CALLR (function pointer), RET (return value),
             * PUTCHAR (char in r0) — not captured by rs1/rs2 fields */
            if (p->op == IR3_CALLR || p->op == IR3_RET || p->op == IR3_PUTCHAR)
                note_use(g_gen[i], g_kill[i], IR3_VREG_ACCUM);
            /* CALL/CALLR kills r0-r3 (caller-saved); r4-r7 are callee-saved
             * and survive calls — do NOT add them to g_kill. */
            if (p->op == IR3_CALL || p->op == IR3_CALLR)
                for (int r = 0; r < 4; r++)
                    bs_set(g_kill[i], r);
            if (p == g_bbs[i].last) break;
        }
    }

    /* Iterate backward until stable */
    for (int iter = 0; iter < 100; iter++) {
        bool changed = false;
        for (int i = n_bbs - 1; i >= 0; i--) {
            /* live_out[i] = union of live_in[succs] */
            Bitset new_out;
            bs_zero(new_out);
            for (int s = 0; s < g_bbs[i].n_succs; s++)
                bs_or(new_out, g_bbs[g_bbs[i].succs[s]].live_in);
            /* live_in[i] = gen[i] | (live_out[i] - kill[i]) */
            Bitset new_in;
            bs_copy(new_in, new_out);
            for (int w = 0; w < IRC_WORDS; w++) new_in[w] &= ~g_kill[i][w];
            bs_or(new_in, g_gen[i]);
            if (!bs_equal(new_in, g_bbs[i].live_in) ||
                !bs_equal(new_out, g_bbs[i].live_out)) {
                bs_copy(g_bbs[i].live_in,  new_in);
                bs_copy(g_bbs[i].live_out, new_out);
                changed = true;
            }
        }
        if (!changed) break;
    }
}

/* ================================================================
 * Build phase: construct interference graph + move list
 * ================================================================ */
static void build_interference(IR3Inst *func_head, IR3Inst *func_end)
{
    for (int bi = 0; bi < n_bbs; bi++) {
        /* Collect instructions in forward order */
        int n_insts = 0;
        for (IR3Inst *p = g_bbs[bi].first; p && p != func_end; p = p->next) {
            if (n_insts >= BB_INST_MAX) {
                fprintf(stderr, "irc: fatal: BB %d has >%d instructions (BB_INST_MAX)\n",
                        bi, BB_INST_MAX);
                exit(1);
            }
            g_rev[n_insts++] = p;
            if (p == g_bbs[bi].last) break;
        }

        /* Process in reverse order, maintaining live set */
        Bitset live;
        bs_copy(live, g_bbs[bi].live_out);

        for (int k = n_insts - 1; k >= 0; k--) {
            IR3Inst *p = g_rev[k];

            /* Backward analysis: defs/clobbers before uses.
             *
             * Step 1 — CALL/CALLR clobbers: r0-r3 are caller-saved (overwritten).
             * r4-r7 are callee-saved: the callee preserves them, so live vregs
             * allocated to r4-r7 survive the call without interference.
             * Interfere each caller-saved reg with everything live after the call,
             * then remove caller-saved regs from live (r4-r7 remain live). */
            if (p->op == IR3_CALL || p->op == IR3_CALLR) {
                for (int r = 0; r < 4; r++) {
                    for (int t = bs_next(live, -1); t >= 0; t = bs_next(live, t))
                        add_irc_edge(r, t);
                }
                for (int r = 0; r < 4; r++) bs_clr(live, r);
            }

            /* Step 2 — implicit uses of r0: CALLR reads r0 as the function pointer;
             * RET reads r0 as the return value; PUTCHAR reads r0 as the character.
             * Add r0 to live so that its producer sees it as live at this point.
             * This must happen after the clobber step so that for CALLR the function
             * pointer use is correctly live above (not swept away by the bs_clr). */
            if (p->op == IR3_CALLR || p->op == IR3_RET || p->op == IR3_PUTCHAR)
                bs_set(live, 0);  /* node 0 = r0 = ACCUM */

            /* For MOV d ← s: record as coalescing candidate, don't add (d,s) edge */
            if (p->op == IR3_MOV &&
                p->rd  != IR3_VREG_NONE && p->rd  != IR3_VREG_BP &&
                p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP) {
                int nd = vreg_to_node(p->rd);
                int ns = vreg_to_node(p->rs1);
                if (nd >= 0 && ns >= 0) {
                    note_node(nd); note_node(ns);
                    /* Temporarily remove src from live so d doesn't interfere with s */
                    bool s_was_live = bs_test(live, ns);
                    bs_clr(live, ns);
                    /* Add d to live, add interference from d to all currently live */
                    bs_set(live, nd);
                    add_edge_live(nd, live);
                    bs_clr(live, nd);
                    /* Restore src if it was live */
                    if (s_was_live) bs_set(live, ns);
                    /* Add s to live (it's a use) */
                    bs_set(live, ns);
                    /* Record move for coalescing */
                    record_move(nd, ns);
                    continue;  /* fully handled */
                }
            }

            /* Normal instruction: handle def */
            int nd = IR3_VREG_NONE;
            if (p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP && p->op != IR3_STORE)
                nd = vreg_to_node(p->rd);

            if (nd >= 0) {
                note_node(nd);
                bs_set(live, nd);          /* def is "live" at this point */
                add_edge_live(nd, live);   /* interferes with everything live */
                bs_clr(live, nd);          /* remove def from live */
            }

            /* Uses: add to live */
            if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP) {
                int n = vreg_to_node(p->rs1);
                if (n >= 0) { note_node(n); bs_set(live, n); }
            }
            if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP) {
                int n = vreg_to_node(p->rs2);
                if (n >= 0) { note_node(n); bs_set(live, n); }
            }
            /* STORE's rd is a use */
            if (p->op == IR3_STORE && p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP) {
                int n = vreg_to_node(p->rd);
                if (n >= 0) { note_node(n); bs_set(live, n); }
            }
        }
    }
}

/* ================================================================
 * Worklist initialization
 * ================================================================ */
static bool g_any_virtual;   /* true if any virtual nodes were seen */

static void make_worklist(void)
{
    g_any_virtual = false;
    for (int n = IRC_PHYS; n <= g_max_node; n++) {
        if (color_of[n] != -1) continue;          /* already colored */
        if (!bs_test(seen_nodes, n)) continue;     /* never appeared in IR */
        g_any_virtual = true;
        if (degree[n] >= IRC_K)
            bs_set(spill_wl, n);
        else if (is_move_related(n))
            bs_set(freeze_wl, n);
        else
            bs_set(simplify_wl, n);
    }
    /* Moves start as WORKLIST if at least one end is low-degree or pre-colored */
    for (int i = 0; i < n_moves; i++) {
        int u = g_moves[i].u, v = g_moves[i].v;
        if (degree[u] < IRC_K || is_precolored(u) ||
            degree[v] < IRC_K || is_precolored(v))
            g_moves[i].state = MOVE_WORKLIST;
        else
            g_moves[i].state = MOVE_ACTIVE;
    }
}

static bool has_worklist_moves(void)
{
    for (int i = 0; i < n_moves; i++)
        if (g_moves[i].state == MOVE_WORKLIST) return true;
    return false;
}

/* ================================================================
 * Simplify: remove low-degree, non-move-related node
 * ================================================================ */
static void decrement_degree(int m)
{
    if (m < 0 || m >= IRC_MAX_NODES || is_precolored(m)) return;
    if (degree[m] == 0) return;
    int old = degree[m]--;
    if (old == IRC_K) {
        /* m transitions from high-degree to low-degree: enable its moves */
        if (is_move_related(m)) {
            bs_clr(spill_wl, m);
            bs_set(freeze_wl, m);
            /* Enable any moves */
            for (MoveNode *mn = node_move_head[m]; mn; mn = mn->next) {
                if (g_moves[mn->mi].state == MOVE_ACTIVE)
                    g_moves[mn->mi].state = MOVE_WORKLIST;
            }
        } else {
            bs_clr(spill_wl, m);
            bs_set(simplify_wl, m);
        }
    }
}

static void simplify(void)
{
    int n = bs_next(simplify_wl, -1);
    if (n < 0) return;
    bs_clr(simplify_wl, n);
    if (select_sp >= IRC_MAX_NODES) {
        fprintf(stderr, "irc: fatal: select_stack overflow (IRC_MAX_NODES=%d)\n", IRC_MAX_NODES);
        exit(1);
    }
    select_stack[select_sp++] = n;
    bs_set(select_set, n);
    /* Decrement degree of all neighbors */
    for (int m = bs_next(imat[n], -1); m >= 0; m = bs_next(imat[n], m)) {
        if (bs_test(select_set, m) || bs_test(coalesced_set, m)) continue;
        decrement_degree(m);
    }
}

/* ================================================================
 * Coalesce: George and Briggs criteria
 * ================================================================ */

static void add_work_list(int u)
{
    if (is_precolored(u)) return;
    if (degree[u] >= IRC_K) return;
    if (is_move_related(u)) return;
    bs_clr(freeze_wl, u);
    bs_set(simplify_wl, u);
}

/* George: can coalesce v into pre-colored u?
 * All high-degree (≥K) neighbors of v must already interfere with u. */
static bool george_ok(int u, int v)
{
    for (int t = bs_next(imat[v], -1); t >= 0; t = bs_next(imat[v], t)) {
        if (bs_test(select_set, t) || bs_test(coalesced_set, t)) continue;
        if (degree[t] < IRC_K) continue;
        if (!bs_test(imat[u], t)) return false;
    }
    return true;
}

/* Briggs: can coalesce two virtual nodes u, v?
 * Count of high-degree (≥K) nodes in adj(u) ∪ adj(v) must be < K. */
static int combined_high_degree(int u, int v)
{
    Bitset combined;
    bs_copy(combined, imat[u]);
    bs_or(combined, imat[v]);
    bs_clr(combined, u);
    bs_clr(combined, v);
    int count = 0;
    for (int t = bs_next(combined, -1); t >= 0; t = bs_next(combined, t)) {
        if (bs_test(select_set, t) || bs_test(coalesced_set, t)) continue;
        if (degree[t] >= IRC_K) count++;
    }
    return count;
}

static void combine(int u, int v)
{
    /* Merge v into u */
    bs_clr(freeze_wl, v);
    bs_clr(spill_wl,  v);
    bs_set(coalesced_set, v);
    alias_of[v] = u;
    /* Merge v's move list into u: append any move not already in u's list */
    for (MoveNode *mn = node_move_head[v]; mn; mn = mn->next) {
        bool dup = false;
        for (MoveNode *mu = node_move_head[u]; mu; mu = mu->next)
            if (mu->mi == mn->mi) { dup = true; break; }
        if (!dup) {
            MoveNode *new_mn = scratch_alloc(sizeof(MoveNode));
            new_mn->mi = mn->mi; new_mn->next = node_move_head[u];
            node_move_head[u] = new_mn;
        }
    }
    /* Add v's interference edges to u; decrement neighbor degrees */
    for (int t = bs_next(imat[v], -1); t >= 0; t = bs_next(imat[v], t)) {
        if (bs_test(select_set, t) || bs_test(coalesced_set, t)) continue;
        add_irc_edge(u, t);
        decrement_degree(t);
    }
    /* If u jumped above K, move to spill_wl */
    if (degree[u] >= IRC_K && bs_test(freeze_wl, u)) {
        bs_clr(freeze_wl, u);
        bs_set(spill_wl, u);
    }
}

static void coalesce(void)
{
    /* Find a WORKLIST move */
    int mi = -1;
    for (int i = 0; i < n_moves; i++)
        if (g_moves[i].state == MOVE_WORKLIST) { mi = i; break; }
    if (mi < 0) return;

    int u = get_alias(g_moves[mi].u);
    int v = get_alias(g_moves[mi].v);
    if (is_precolored(v)) { int tmp = u; u = v; v = tmp; }  /* u = pre-colored if any */

    if (u == v) {
        /* Same node: trivially coalesced */
        g_moves[mi].state = MOVE_COALESCED;
        add_work_list(u);
    } else if (is_precolored(v) || bs_test(imat[u], v)) {
        /* Both pre-colored, or already interfere: constrained */
        g_moves[mi].state = MOVE_CONSTRAINED;
        add_work_list(u);
        add_work_list(v);
    } else if ((is_precolored(u) && george_ok(u, v)) ||
               (!is_precolored(u) && combined_high_degree(u, v) < IRC_K)) {
        g_moves[mi].state = MOVE_COALESCED;
        combine(u, v);
        add_work_list(u);
    } else {
        /* Not safe to coalesce yet; mark active */
        g_moves[mi].state = MOVE_ACTIVE;
    }
}

/* ================================================================
 * Freeze: give up coalescing a move-related node
 * ================================================================ */
static void freeze_moves(int u)
{
    for (MoveNode *mn = node_move_head[u]; mn; mn = mn->next) {
        int mi = mn->mi;
        if (g_moves[mi].state != MOVE_WORKLIST && g_moves[mi].state != MOVE_ACTIVE)
            continue;
        g_moves[mi].state = MOVE_FROZEN;
        /* Check if the other end of the move should now be simplified */
        int v = (get_alias(g_moves[mi].u) == u)
                ? get_alias(g_moves[mi].v)
                : get_alias(g_moves[mi].u);
        if (!is_precolored(v) && !is_move_related(v) && degree[v] < IRC_K) {
            bs_clr(freeze_wl, v);
            bs_set(simplify_wl, v);
        }
    }
}

static void freeze(void)
{
    int n = bs_next(freeze_wl, -1);
    if (n < 0) return;
    bs_clr(freeze_wl, n);
    bs_set(simplify_wl, n);
    freeze_moves(n);
}

/* ================================================================
 * SelectSpill: pick the highest-degree node
 * ================================================================ */
static void select_spill(void)
{
    int best = -1, best_deg = -1;
    for (int n = bs_next(spill_wl, -1); n >= 0; n = bs_next(spill_wl, n)) {
        if (degree[n] > best_deg) { best_deg = degree[n]; best = n; }
    }
    if (best < 0) return;
    bs_clr(spill_wl, best);
    bs_set(simplify_wl, best);
    freeze_moves(best);
}

/* ================================================================
 * AssignColors: pop select stack, assign colors
 * Returns true if there were actual spills.
 * ================================================================ */
static bool assign_colors(void)
{
    bool had_spill = false;
    while (select_sp > 0) {
        int n = select_stack[--select_sp];
        bs_clr(select_set, n);

        /* Build set of colors used by (colored) neighbors */
        /* ok_colors: bit i set = color i is available.  Colors 1..K = r1..r7. */
        uint64_t ok = ((1ULL << (IRC_K+1)) - 1) & ~1ULL;  /* bits 1..7 */
        for (int t = bs_next(imat[n], -1); t >= 0; t = bs_next(imat[n], t)) {
            int w = get_alias(t);
            int c = color_of[w];
            if (c >= 1 && c <= IRC_K) ok &= ~(1ULL << c);
        }

        if (!ok) {
            bs_set(actual_spill, n);
            had_spill = true;
        } else {
            color_of[n] = __builtin_ctzll(ok);  /* lowest available color */
        }
    }
    /* Coalesced nodes inherit the color of their alias */
    for (int n = bs_next(coalesced_set, -1); n >= 0; n = bs_next(coalesced_set, n))
        color_of[n] = color_of[get_alias(n)];
    return had_spill;
}

/* ================================================================
 * Frame expansion and spill slot allocation
 * ================================================================ */
static void alloc_spill_slots(void)
{
    for (int n = bs_next(actual_spill, -1); n >= 0; n = bs_next(actual_spill, n)) {
        if (!is_virtual(n)) continue;

        /* Try to reuse an existing spill slot from a non-interfering node. */
        int reuse = 0;
        for (int m = IRC_PHYS; m < IRC_MAX_NODES && !reuse; m++) {
            if (!spill_slot[m] || m == n) continue;
            if (bs_test(imat[n], m)) continue;        /* n and m interfere */
            /* Verify no other node sharing m's slot also conflicts with n. */
            int cand = spill_slot[m];
            bool conflict = false;
            for (int k = IRC_PHYS; k < IRC_MAX_NODES; k++) {
                if (spill_slot[k] == cand && k != n && bs_test(imat[n], k))
                    { conflict = true; break; }
            }
            if (!conflict) reuse = cand;
        }

        if (reuse) {
            spill_slot[n] = reuse;
        } else {
            g_spill_next -= 4;
            g_spill_next &= ~3;
            spill_slot[n] = g_spill_next;
        }
    }
}

static void expand_frame(IR3Inst *func_head, IR3Inst *func_end, IR3Inst *enter_node)
{
    if (!enter_node) return;
    int new_N = -g_spill_next;
    new_N = (new_N + 3) & ~3;
    int orig_N = g_enter_N;
    int expansion = new_N - orig_N;
    if (expansion <= 0) return;

    enter_node->imm = new_N;

    /* Shift existing bp-relative offsets below the original ENTER boundary */
    for (IR3Inst *p = func_head; p && p != func_end; p = p->next) {
        if (p->op == IR3_LOAD  && p->rs1 == IR3_VREG_BP && p->imm < -orig_N)
            p->imm -= expansion;
        if (p->op == IR3_STORE && p->rd  == IR3_VREG_BP && p->imm < -orig_N)
            p->imm -= expansion;
        if (p->op == IR3_LEA   && p->imm < -orig_N)
            p->imm -= expansion;
    }
}

/* ================================================================
 * Spill rewriting: replace spilled vregs with fresh tmps + LOAD/STORE
 * ================================================================ */
static IR3Inst *new_ir3node(IR3Op op, int line)
{
    IR3Inst *n = scratch_alloc(sizeof(IR3Inst));
    n->op  = op;
    n->rd  = IR3_VREG_NONE;
    n->rs1 = IR3_VREG_NONE;
    n->rs2 = IR3_VREG_NONE;
    n->line = line;
    return n;
}

static void rewrite_spills(IR3Inst **func_head_ptr, IR3Inst *func_end)
{
    IR3Inst *prev = NULL;
    for (IR3Inst *p = *func_head_ptr; p && p != func_end; ) {
        /* Skip non-instructions */
        if (p->op == IR3_SYMLABEL || p->op == IR3_LABEL || p->op == IR3_ENTER ||
            p->op == IR3_WORD || p->op == IR3_BYTE || p->op == IR3_ALIGN ||
            p->op == IR3_COMMENT || p->op == IR3_PUTCHAR) {
            prev = p; p = p->next; continue;
        }

        /* Helper: check if vreg v is spilled */
#define SPILLED(v) \
    ((v) > IR3_VREG_ACCUM && vreg_to_node(v) >= 0 && bs_test(actual_spill, vreg_to_node(v)))

        /* --- Insert LOADs before p for spilled uses --- */

        /* rs1 use */
        if (p->rs1 != IR3_VREG_NONE && p->rs1 != IR3_VREG_BP && SPILLED(p->rs1)) {
            int sn  = vreg_to_node(p->rs1);
            int tmp = ir3_new_vreg();
            IR3Inst *ld = new_ir3node(IR3_LOAD, p->line);
            ld->rd = tmp; ld->rs1 = IR3_VREG_BP;
            ld->imm = spill_slot[sn]; ld->size = 4;
            ld->next = p;
            if (prev) prev->next = ld; else *func_head_ptr = ld;
            prev = ld;
            p->rs1 = tmp;
        }

        /* rs2 use */
        if (p->rs2 != IR3_VREG_NONE && p->rs2 != IR3_VREG_BP && SPILLED(p->rs2)) {
            int sn  = vreg_to_node(p->rs2);
            int tmp = ir3_new_vreg();
            IR3Inst *ld = new_ir3node(IR3_LOAD, p->line);
            ld->rd = tmp; ld->rs1 = IR3_VREG_BP;
            ld->imm = spill_slot[sn]; ld->size = 4;
            ld->next = p;
            if (prev) prev->next = ld; else *func_head_ptr = ld;
            prev = ld;
            p->rs2 = tmp;
        }

        /* STORE base (rd used as address source) */
        if (p->op == IR3_STORE &&
            p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP && SPILLED(p->rd)) {
            int sn  = vreg_to_node(p->rd);
            int tmp = ir3_new_vreg();
            IR3Inst *ld = new_ir3node(IR3_LOAD, p->line);
            ld->rd = tmp; ld->rs1 = IR3_VREG_BP;
            ld->imm = spill_slot[sn]; ld->size = 4;
            ld->next = p;
            if (prev) prev->next = ld; else *func_head_ptr = ld;
            prev = ld;
            p->rd = tmp;
        }

        /* --- Replace def with fresh tmp and insert STORE after p --- */
        if (p->op != IR3_STORE &&
            p->rd != IR3_VREG_NONE && p->rd != IR3_VREG_BP && SPILLED(p->rd)) {
            int sn  = vreg_to_node(p->rd);
            int tmp = ir3_new_vreg();
            p->rd = tmp;
            IR3Inst *st = new_ir3node(IR3_STORE, p->line);
            st->rd  = IR3_VREG_BP;
            st->rs1 = tmp;
            st->imm  = spill_slot[sn];
            st->size = 4;
            st->next = p->next;
            p->next  = st;
            /* advance past the store (it contains no spilled vregs) */
            prev = st;
            p = st->next;
            continue;
        }
#undef SPILLED

        prev = p;
        p = p->next;
    }
}

/* ================================================================
 * Apply colors: rewrite vregs in-place; eliminate same-color MOVs
 * ================================================================ */
static int apply_color(int v)
{
    if (v == IR3_VREG_NONE || v == IR3_VREG_BP) return v;
    int n = vreg_to_node(v);
    if (n < 0) return v;
    int c = color_of[get_alias(n)];
    return (c >= 0) ? c : v;  /* fallback: leave as-is if uncolored (error) */
}

static void apply_colors(IR3Inst *func_head, IR3Inst *func_end)
{
    for (IR3Inst *p = func_head; p && p != func_end; p = p->next) {
        p->rd  = apply_color(p->rd);
        p->rs1 = apply_color(p->rs1);
        p->rs2 = apply_color(p->rs2);
        /* Eliminate MOV rd, rs1 when they got the same color */
        if (p->op == IR3_MOV && p->rd >= 0 && p->rd == p->rs1)
            p->rd = IR3_VREG_NONE;
    }
}

/* ================================================================
 * Main per-function allocator
 * ================================================================ */
static void alloc_function(IR3Inst **func_head_ptr, IR3Inst *func_end,
                           IR3Inst *enter_node)
{
    /* Spill slots go immediately below the current enter frame.
     * Do NOT scan for the deepest bp-relative access: call-arg saves inside
     * adjw scopes can reach very deep offsets, and placing the spill floor
     * below them would make the enter expansion huge and overflow the stack.
     * Instead, start at -orig_N and grow downward; expand_frame shifts all
     * existing bp-relative offsets (including adjw-space call-arg saves) by
     * the expansion, which correctly adjusts them for the larger enter frame. */
    int orig_N = enter_node ? enter_node->imm : 0;
    int spill_floor = -orig_N;

    memset(spill_slot, 0, sizeof(spill_slot));
    for (int iter = 0; iter < IRC_MAX_ITERS; iter++) {
        irc_reset_fn();
        g_enter_N    = enter_node ? enter_node->imm : 0;
        g_spill_next = spill_floor;

        /* Build CFG */
        build_cfg_irc(*func_head_ptr, func_end);
        if (n_bbs == 0) break;

        /* Liveness analysis */
        compute_liveness(*func_head_ptr, func_end);

        /* Build interference graph */
        build_interference(*func_head_ptr, func_end);

        /* Initialize worklists */
        make_worklist();
        if (!g_any_virtual) break;  /* no virtual nodes — nothing to allocate */

        /* Main IRC loop: Simplify → Coalesce → Freeze → SelectSpill */
        while (true) {
            if      (!bs_empty(simplify_wl)) simplify();
            else if (has_worklist_moves())    coalesce();
            else if (!bs_empty(freeze_wl))   freeze();
            else if (!bs_empty(spill_wl))    select_spill();
            else break;
        }

        /* Assign colors */
        bool need_spill = assign_colors();
        if (!need_spill) break;

        /* Actual spills: allocate slots, expand frame, rewrite IR */
        alloc_spill_slots();
        expand_frame(*func_head_ptr, func_end, enter_node);
        rewrite_spills(func_head_ptr, func_end);
        /* Update spill_floor for next iteration */
        spill_floor = g_spill_next;

        if (iter + 1 == IRC_MAX_ITERS) {
            fprintf(stderr, "irc: fatal: spill rewrite did not converge after %d iterations "
                    "(IRC_MAX_ITERS); vregs remain uncolored\n", IRC_MAX_ITERS);
            exit(1);
        }
    }

    /* Apply final colors */
    apply_colors(*func_head_ptr, func_end);
}

/* ================================================================
 * insert_callee_saves — post-regalloc callee-save/restore insertion.
 *
 * Called after alloc_function() has applied colors.  Scans the function
 * body for uses of r4-r7; for each used callee-saved register, allocates
 * a 4-byte frame slot below the current ENTER frame, inserts a STORE
 * immediately after ENTER (save), and inserts a LOAD immediately before
 * each RET (restore).  Expands ENTER imm to cover the new slots.
 *
 * Frame layout after insertion (grows downward from bp):
 *   [0 .. -(orig locals)]          original locals (from codegen)
 *   [-(orig locals) .. -(spill N)] spill slots (from irc iterations)
 *   [-(enter_node->imm + 4) ...]   callee-save slots (r4, r5, r6, r7)
 *
 * Any bp-relative offsets already below the original frame boundary
 * (call-arg stores, outer-temp spills in the adjw region) are shifted
 * down by the expansion amount so they don't alias the new save slots.
 * ================================================================ */
static void insert_callee_saves(IR3Inst *func_head, IR3Inst *func_end,
                                IR3Inst *enter_node)
{
    if (!enter_node) return;

    /* Collect which of r4-r7 are actually used in this function. */
    int used = 0;
    for (IR3Inst *q = func_head; q && q != func_end; q = q->next) {
        if (q->rd  >= 4 && q->rd  <= 7) used |= (1 << q->rd);
        if (q->rs1 >= 4 && q->rs1 <= 7) used |= (1 << q->rs1);
        if (q->rs2 >= 4 && q->rs2 <= 7) used |= (1 << q->rs2);
    }
    if (!used) return;

    /* Allocate one 4-byte slot per used callee-saved register, placed
     * immediately below the current final frame (enter_node->imm). */
    int save_off[8] = {0};
    int next = -(enter_node->imm);   /* starts at bottom of existing frame */
    for (int r = 4; r <= 7; r++) {
        if (!(used & (1 << r))) continue;
        next -= 4;
        save_off[r] = next;          /* bp-relative offset for r's slot */
    }

    /* Expand ENTER to cover callee-save slots.  'next' is already a
     * multiple of 4 because enter_node->imm is 4-aligned (expand_frame
     * enforces this) and we decrement by 4 each step. */
    int orig_N   = enter_node->imm;
    int new_N    = -next;
    int expansion = new_N - orig_N;
    enter_node->imm = new_N;

    /* Shift all existing bp-relative offsets that fall below the original
     * frame boundary (imm < -orig_N).  These are call-arg stores and
     * outer-temp spills placed in the adjw region below sp by
     * flush_for_call_n.  When enter grows by 'expansion', those slots
     * must move down by the same amount to avoid overlapping the new
     * callee-save slots just inserted at the bottom of the frame. */
    if (expansion > 0) {
        for (IR3Inst *q = func_head; q && q != func_end; q = q->next) {
            if (q->op == IR3_LOAD  && q->rs1 == IR3_VREG_BP
                    && q->imm < -orig_N)
                q->imm -= expansion;
            if (q->op == IR3_STORE && q->rd  == IR3_VREG_BP
                    && q->imm < -orig_N)
                q->imm -= expansion;
            if (q->op == IR3_LEA   && q->imm < -orig_N)
                q->imm -= expansion;
        }
    }

    /* Insert STORE instructions immediately after ENTER (save on entry). */
    IR3Inst *ins = enter_node;
    for (int r = 4; r <= 7; r++) {
        if (!save_off[r]) continue;
        IR3Inst *s = scratch_alloc(sizeof(IR3Inst));
        memset(s, 0, sizeof(*s));
        s->op   = IR3_STORE;
        s->rd   = IR3_VREG_BP;
        s->rs1  = r;
        s->imm  = save_off[r];
        s->size = 4;
        s->line = enter_node->line;
        s->next = ins->next;
        ins->next = s;
        ins = s;
    }

    /* Insert LOAD instructions immediately before each RET (restore on exit).
     * Walk with a trailing 'prev' pointer so we can splice before the RET. */
    IR3Inst *prev = enter_node;
    for (IR3Inst *q = enter_node->next; q && q != func_end; q = q->next) {
        if (q->op == IR3_RET) {
            IR3Inst *after = prev;
            for (int r = 4; r <= 7; r++) {
                if (!save_off[r]) continue;
                IR3Inst *ld = scratch_alloc(sizeof(IR3Inst));
                memset(ld, 0, sizeof(*ld));
                ld->op   = IR3_LOAD;
                ld->rd   = r;
                ld->rs1  = IR3_VREG_BP;
                ld->imm  = save_off[r];
                ld->size = 4;
                ld->line = q->line;
                ld->next = after->next;  /* = q (the RET) */
                after->next = ld;
                after = ld;
            }
            prev = q;       /* prev tracks the RET (q is unchanged) */
        } else {
            prev = q;
        }
    }
}

/* ================================================================
 * Public entry point: irc_regalloc(head)
 * Processes all functions between IR3_SYMLABEL nodes.
 * ================================================================ */
void irc_regalloc(IR3Inst *head)
{
    /* Scan for function boundaries.  Each IR3_SYMLABEL starts a new function.
     * The function body runs from the node after SYMLABEL up to (exclusive) the
     * next SYMLABEL (or end of list).  The ENTER node is found by a short scan. */

    IR3Inst *prev_sym = NULL;

    for (IR3Inst *p = head; p; p = p->next) {
        if (p->op != IR3_SYMLABEL) continue;

        if (prev_sym) {
            /* Process the function that started at prev_sym */
            IR3Inst *func_head = prev_sym->next;
            IR3Inst *func_end  = p;               /* exclusive end */
            IR3Inst *enter_node = NULL;
            for (IR3Inst *q = func_head; q && q != func_end; q = q->next) {
                if (q->op == IR3_ENTER) { enter_node = q; break; }
                if (q->op == IR3_SYMLABEL) break;
            }
            if (func_head && func_head != func_end && enter_node) {
                /* func_head_ptr must be stable even if we insert before first node.
                 * The SYMLABEL always precedes the function body, so
                 * insertions go after SYMLABEL which is stable. */
                IR3Inst *fh = func_head;
                alloc_function(&fh, func_end, enter_node);
                insert_callee_saves(fh, func_end, enter_node);
                /* Relink SYMLABEL → possibly new func_head */
                prev_sym->next = fh;
            }
        }
        prev_sym = p;
    }

    /* Last function */
    if (prev_sym) {
        IR3Inst *func_head = prev_sym->next;
        IR3Inst *func_end  = NULL;
        IR3Inst *enter_node = NULL;
        for (IR3Inst *q = func_head; q; q = q->next) {
            if (q->op == IR3_ENTER) { enter_node = q; break; }
            if (q->op == IR3_SYMLABEL) break;
        }
        if (func_head && enter_node) {
            IR3Inst *fh = func_head;
            alloc_function(&fh, func_end, enter_node);
            insert_callee_saves(fh, func_end, enter_node);
            prev_sym->next = fh;
        }
    }

    /* Final pass: rewrite ACCUM (vreg 100 → physical 0) everywhere,
     * including in data-section nodes and any remaining unprocessed vregs. */
    for (IR3Inst *p = head; p; p = p->next) {
        if (p->rd  == IR3_VREG_ACCUM) p->rd  = 0;
        if (p->rs1 == IR3_VREG_ACCUM) p->rs1 = 0;
        if (p->rs2 == IR3_VREG_ACCUM) p->rs2 = 0;
    }
}
