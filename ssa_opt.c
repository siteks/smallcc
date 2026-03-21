/* ssa_opt.c — SSA-level optimizations (operate on virtual registers,
 * before physical register allocation).
 *
 * Passes (in order):
 *   0. Identity-move elimination       (original)
 *   1. B1: SSA_ADJ merging
 *   2. B3/C1: LEA→STORE forwarding     (F3b register-relative → F2 bp-relative)
 *   3. C2: LOAD+[SXW/SXB]+MOV fusion  (skip optional sign-extend between load and MOV)
 *   4. B2: compare-branch fusion       (SSA_ALU + SSA_JZ/JNZ → SSA_BRANCH)
 *   5. D4: add-immediate               (SSA_MOVI + SSA_ALU ADD → SSA_ALU_IMM)
 *   6. E1: LEA+LOAD → bp-relative LOAD (SSA_LEA + SSA_LOAD rs1=vA → rs1=-2)
 *   7. E2: ADDLI+LOAD → offset LOAD   (SSA_ALU_IMM ADD + SSA_LOAD → non-zero imm LOAD)
 *   8. E3: ALU+MOV → retarget ALU     (redirect ALU destination, kill identity MOV)
 *   9. E5: Dead MOV DCE               (kill SSA_MOV whose destination is never used)
 *  10. F1: MOVI/MOVSYM+MOV → retarget (immw r0,N;or r1,r0,r0 → immw r1,N)
 *  11. F2: Redundant SXW/SXB DCE      (sxw(sxw(x))==sxw(x); kill second)
 */
#include <stdlib.h>
#include <string.h>
#include "ssa.h"

/* ----------------------------------------------------------------
 * Virtual register use-count table.
 * Records how many times each virtual register is used as a source
 * operand (rs1, rs2) or as an address in SSA_STORE (rd field).
 * ---------------------------------------------------------------- */
#define MAX_VREGS 512

static int use_count[MAX_VREGS];

static void count_uses(SSAInst *head)
{
    memset(use_count, 0, sizeof(use_count));
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0) continue;
#define INC_USE(r) do { \
    int _r = (r); \
    if (_r >= VREG_START && _r - VREG_START < MAX_VREGS) \
        use_count[_r - VREG_START]++; \
} while (0)
        INC_USE(p->rs1);
        INC_USE(p->rs2);
        /* For SSA_STORE, rd is the address register — a use, not a def */
        if (p->op == SSA_STORE) INC_USE(p->rd);
#undef INC_USE
    }
}

static int single_use(int vreg)
{
    if (vreg < VREG_START || vreg - VREG_START >= MAX_VREGS) return 0;
    return use_count[vreg - VREG_START] == 1;
}

static void mark_dead(SSAInst *p) { p->op = (SSAOp)-1; }

/* Return next live node after p (skipping dead ones). */
static SSAInst *next_live(SSAInst *p)
{
    for (p = p->next; p && (int)p->op < 0; p = p->next) {}
    return p;
}

/* F2 bp-relative range checks (mirrors risc_backend.c). */
static int f2_ok(int off, int size)
{
    if (size == 2) return !(off & 1) && off / 2 >= -64 && off / 2 <= 63;
    if (size == 1) return off >= -64 && off <= 63;
    if (size == 4) return !(off & 3) && off / 4 >= -64 && off / 4 <= 63;
    return 0;
}

/* Scan forward from p (exclusive) for the first live SSA_STORE with rd==vA.
 * Stops at control-flow boundaries or after 128 nodes.
 * Returns NULL if not found. */
static SSAInst *find_store_for(SSAInst *p, int vA)
{
    int limit = 128;
    for (SSAInst *q = p->next; q && limit-- > 0; q = q->next) {
        if ((int)q->op < 0) continue;
        /* Stop at control flow or function entry */
        if (q->op == SSA_J || q->op == SSA_JZ || q->op == SSA_JNZ ||
            q->op == SSA_BRANCH || q->op == SSA_RET || q->op == SSA_SYMLABEL)
            return NULL;
        if (q->op == SSA_STORE && q->rd == vA)
            return q;
    }
    return NULL;
}

/* Return 1 if vreg appears as rs1 or rs2 (source) in any live node between
 * p (exclusive) and target (exclusive), stopping early if vreg is redefined
 * as rd (so a use after a redefinition is of a different value and is safe).
 *
 * Used by B3 for two purposes:
 *   (a) vL check: does the LEA's output (v8) escape the LEA→MOV boundary?
 *       A compound-assignment loads through vL before the STORE.
 *       If a LOAD redefines vL (rd=vL) without using it, subsequent uses
 *       of vL are of the LOAD's value, not the LEA's — so we stop early.
 *   (b) vA check: is the MOV's output (v9) used for anything other than
 *       the STORE itself?  If yes (e.g. struct member adds use v9 as a
 *       base address), killing the MOV would leave those ops with no value. */
static int used_before_redef(SSAInst *p, SSAInst *target, int vreg)
{
    for (SSAInst *q = p->next; q && q != target; q = q->next) {
        if ((int)q->op < 0) continue;
        /* Used as source: vreg's value is still needed */
        if (q->rs1 == vreg || q->rs2 == vreg) return 1;
        /* Used as SSA_STORE address (rd is a source for stores) */
        if (q->op == SSA_STORE && q->rd == vreg) return 1;
        /* Redefined without being used first: subsequent uses are of a new def */
        if (q->rd == vreg) return 0;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * ssa_peephole — optimisation passes (in-place, on virtual regs)
 * ---------------------------------------------------------------- */
void ssa_peephole(SSAInst *head)
{
    /* ----- Pass 0: identity-move elimination ----- */
    for (SSAInst *p = head; p; p = p->next) {
        if (p->op == SSA_MOV && p->rd == p->rs1 && p->rd >= 0)
            mark_dead(p);
    }

    /* ----- Pass 1: B1 — merge adjacent SSA_ADJ ----- */
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_ADJ) continue;
        SSAInst *q = next_live(p);
        if (!q || q->op != SSA_ADJ) continue;
        p->imm += q->imm;
        mark_dead(q);
        if (p->imm == 0) mark_dead(p);
    }

    /* ----- Pass 2: B3/C1 — LEA→STORE forwarding ----- *
     *
     * Pattern (from a local-variable assignment `x = rhs`):
     *   SSA_LEA rd=vL, imm=N           (address of local at bp+N)
     *   SSA_MOV rd=vA, rs1=vL          (push address to virtual stack)
     *   <rhs computation into v8>
     *   SSA_STORE rd=vA, rs1=vZ, imm=0 (register-relative store)
     *
     * If vL and vA are each used exactly once, and N is in F2 range
     * for the store size, rewrite the STORE to bp-relative (F2) and
     * kill the LEA and MOV.
     */
    count_uses(head);
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_LEA) continue;
        int vL = p->rd;    /* virtual reg holding the address */
        int N  = p->imm;   /* bp-relative byte offset */
        if (vL < VREG_START) continue;

        /* Expect next live to be SSA_MOV vA = vL (the IR_PUSH equivalent).
         * We use structural adjacency here rather than single_use(vL) because
         * virtual regs are reused by depth (not true SSA): the accumulator v8
         * is redefined by the RHS computation after the push, so its use_count
         * is > 1 even though the LEA's result is captured in only this one MOV. */
        SSAInst *mov = next_live(p);
        if (!mov || mov->op != SSA_MOV || mov->rs1 != vL) continue;
        int vA = mov->rd;
        if (vA < VREG_START) continue;
        /* No single_use(vA) check: the virtual stack model guarantees that vA is
         * only live until the matching STORE, so find_store_for's bounded scan
         * (which stops at control-flow boundaries) finds the right STORE. */

        /* Find the STORE that uses vA as its address register */
        SSAInst *store = find_store_for(mov, vA);
        if (!store) continue;
        if (store->imm != 0) continue;         /* only handles base+0 addressing */
        if (!f2_ok(N, store->size)) continue;
        /* Guard 1: vL (LEA's output) must not be used between MOV and STORE.
         * Stops early if vL is redefined first (safe: later uses are of new def).
         * E.g. compound `a += 2`: loads through vL (rs1=vL) before the STORE. */
        if (used_before_redef(mov, store, vL)) continue;
        /* Guard 2: vA (MOV's output) must not be used except in the STORE itself.
         * E.g. struct member assignment computes `&s + offset` using vA as base. */
        if (used_before_redef(mov, store, vA)) continue;

        /* Transform: convert to bp-relative (F2) store; kill LEA and MOV */
        store->sym = p->sym;  /* carry variable name for -ann annotation */
        store->rd  = -2;
        store->imm = N;
        mark_dead(p);
        mark_dead(mov);
    }

    /* ----- Pass 3: C2 — LOAD+[SXW/SXB]+MOV fusion ----- *
     *
     * Pattern (base):
     *   SSA_LOAD rd=vA, ...
     *   SSA_MOV  rd=vB, rs1=vA   (immediately adjacent)
     *
     * Extended pattern (sign-extend in between):
     *   SSA_LOAD  rd=vA, ...
     *   SSA_ALU1  rd=vA, rs1=vA, alu_op=IR_SXW or IR_SXB   (optional)
     *   SSA_MOV   rd=vB, rs1=vA
     *
     * Redirect the LOAD to produce vB directly. When a SXW/SXB is present,
     * retarget it to operate on vB so that E4 (in risc_backend) can later
     * fuse LOAD(vB)+SXW(vB) → sign-extending load instruction (lwx/lbx).
     * Kill the MOV.
     */
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_LOAD) continue;
        int vA = p->rd;
        if (vA < VREG_START) continue;
        SSAInst *q = next_live(p);
        if (!q) continue;
        /* Optional SXW/SXB sitting between LOAD and MOV */
        SSAInst *sxnode = NULL;
        SSAInst *mov = q;
        if (q->op == SSA_ALU1 && q->rd == vA && q->rs1 == vA &&
            (q->alu_op == IR_SXW || q->alu_op == IR_SXB)) {
            sxnode = q;
            mov = next_live(q);
        }
        if (!mov || mov->op != SSA_MOV || mov->rs1 != vA) continue;
        int vD = mov->rd;
        /* Guard: vA must not be used after the MOV (before its next redefinition). */
        if (used_before_redef(mov, NULL, vA)) continue;
        /* Redirect LOAD (and SXW/SXB if present) to target vD; kill MOV */
        p->rd = vD;
        if (sxnode) { sxnode->rd = vD; sxnode->rs1 = vD; }
        mark_dead(mov);
    }

    /* ----- Pass 4: B2 — compare-branch fusion ----- *
     *
     * Pattern:
     *   SSA_ALU alu_op=CMP rd=vC, rs1=rA, rs2=rB
     *   SSA_JZ  imm=L    (branch when compare is FALSE)
     *   or
     *   SSA_JNZ imm=L    (branch when compare is TRUE)
     *
     * Fuse into:
     *   SSA_BRANCH alu_op=BRANCH_OP, rs1=rA, rs2=rB, imm=L
     *
     * Supported fusions:
     *   CMP=EQ,  JNZ → beq  (branch if equal)
     *   CMP=EQ,  JZ  → bne  (branch if not equal)
     *   CMP=NE,  JNZ → bne
     *   CMP=NE,  JZ  → beq
     *   CMP=LT,  JNZ → blt  (unsigned)
     *   CMP=GT,  JNZ → bgt  (unsigned)
     *   CMP=LTS, JNZ → blts (signed)
     *   CMP=GTS, JNZ → bgts (signed)
     *   (LT/GT/LTS/GTS + JZ cannot be expressed as a single F3b branch)
     */
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_ALU) continue;
        IROp cmp = p->alu_op;
        int can_jnz = (cmp == IR_EQ || cmp == IR_NE ||
                       cmp == IR_LT || cmp == IR_GT ||
                       cmp == IR_LTS || cmp == IR_GTS);
        int can_jz  = (cmp == IR_EQ || cmp == IR_NE);
        if (!can_jnz && !can_jz) continue;

        /* Structural adjacency: the compare result (accumulator) must be
         * consumed immediately by the branch. */
        SSAInst *q = next_live(p);
        if (!q || (q->op != SSA_JZ && q->op != SSA_JNZ)) continue;

        int is_jnz = (q->op == SSA_JNZ);
        if ( is_jnz && !can_jnz) continue;
        if (!is_jnz && !can_jz)  continue;

        /* Determine the branch opcode (invert EQ↔NE for JZ) */
        IROp branch_op = cmp;
        if (!is_jnz) {
            branch_op = (cmp == IR_EQ) ? IR_NE : IR_EQ;
        }

        /* Reuse this node as SSA_BRANCH; kill the JZ/JNZ */
        p->op     = SSA_BRANCH;
        p->alu_op = branch_op;
        /* rs1 and rs2 remain: the two compare operands */
        p->imm    = q->imm;  /* label id */
        p->rd     = -1;      /* no destination */
        mark_dead(q);
    }

    /* ----- Pass 5: D4 — add-immediate ----- *
     *
     * Pattern:
     *   SSA_MOVI rd=vK, imm=K
     *   SSA_ALU alu_op=IR_ADD rd=vR, rs1=vA, rs2=vK  (or rs1=vK, rs2=vA)
     *
     * If vK is single-use and K fits in imm10 (-512..511), rewrite to:
     *   SSA_ALU_IMM rd=vR, rs1=vA, imm=K
     * and kill the MOVI.
     *
     * risc_backend emits inc/dec (K=±1, rd==rs1) or addi (|K|≤63, rd==rs1)
     * or addli (rd≠rs1 or |K|>63) as appropriate.
     */
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_MOVI) continue;
        int vK = p->rd;
        if (vK < VREG_START) continue;
        int K = p->imm;
        if (K < -512 || K > 511) continue;

        /* Structural adjacency: the MOVI result must be immediately consumed
         * by the ADD (accumulator reuse model means this is the only use). */
        SSAInst *q = next_live(p);
        if (!q || q->op != SSA_ALU || q->alu_op != IR_ADD) continue;

        /* Identify which operand is the constant */
        int rs_other;
        if (q->rs2 == vK)      rs_other = q->rs1;
        else if (q->rs1 == vK) rs_other = q->rs2;
        else continue;

        /* Transform to SSA_ALU_IMM */
        q->op     = SSA_ALU_IMM;
        q->alu_op = IR_ADD;
        q->rs1    = rs_other;
        q->rs2    = -1;
        q->imm    = K;
        mark_dead(p);
    }

    /* ----- Pass 6: E1 — LEA+LOAD → bp-relative LOAD ----- *
     *
     * Pattern:
     *   SSA_LEA  rd=vA, imm=N
     *   SSA_LOAD rd=vB, rs1=vA, imm=0, size=S    (next live node)
     *
     * Rewrite the LOAD to bp-relative (rs1=-2, imm=N) so the backend
     * selects the compact F2 encoding.  We do NOT use single_use(vA):
     * virtual registers are reused by depth so global use_count is always
     * > 1 for the accumulator.  Instead we check two windows:
     *   (a) vA not used between LEA and LOAD (used_before_redef(p,q,vA))
     *   (b) if LOAD doesn't redefine vA (q->rd != vA), vA must not be
     *       used after LOAD before its next def (used_before_redef(q,NULL,vA))
     */
    count_uses(head);
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_LEA) continue;
        int vA = p->rd;
        int N  = p->imm;
        if (vA < VREG_START) continue;

        SSAInst *q = next_live(p);
        if (!q || q->op != SSA_LOAD || q->rs1 != vA || q->imm != 0) continue;
        if (used_before_redef(p, q, vA)) continue;
        if (q->rd != vA && used_before_redef(q, NULL, vA)) continue;

        q->rs1 = -2;
        q->imm = N;
        mark_dead(p);
    }

    /* ----- Pass 7: E2 — ADDLI+LOAD → offset LOAD ----- *
     *
     * Pattern:
     *   SSA_ALU_IMM rd=vA, rs1=vB, imm=N    (alu_op=IR_ADD)
     *   SSA_LOAD    rd=vC, rs1=vA, imm=0    (next live node)
     *
     * Fold N into the LOAD offset so the backend can emit "llw rd,rb,N/2"
     * (F3b) rather than addli + llw at 0.  vB must be a virtual register
     * (not the bp-relative sentinel -2).
     *
     * We do NOT check single_use(vA) here: the LOAD redefines vA (rd=vA),
     * so any uses of vA after the LOAD are of the LOAD's output, not the
     * ADDLI's.  used_before_redef guards the ADDLI→LOAD window.
     */
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_ALU_IMM) continue;
        if (p->alu_op != IR_ADD) continue;
        int vA = p->rd;
        int vB = p->rs1;
        int N  = p->imm;
        if (vA < VREG_START) continue;
        if (vB < VREG_START) continue;  /* vB must be a real virtual register */

        SSAInst *q = next_live(p);
        if (!q || q->op != SSA_LOAD || q->rs1 != vA || q->imm != 0) continue;
        if (used_before_redef(p, q, vA)) continue;

        q->rs1 = vB;
        q->imm = N;
        mark_dead(p);
    }

    /* ----- Pass 8: E3 — ALU+MOV → retarget ALU destination ----- *
     *
     * Pattern:
     *   SSA_ALU rd=vA, rs1=vB, rs2=vC
     *   SSA_MOV rd=vD, rs1=vA              (next live node)
     *
     * Redirect the ALU to write directly into vD and kill the MOV.
     * CPU4 F1a reads all sources before writing the destination so
     * rd==rs1 or rd==rs2 is always safe.
     *
     * Example: mul r0,r3,r0; or r3,r0,r0  →  mul r3,r3,r0
     *
     * We do NOT use single_use(vA): the accumulator virtual register is
     * reused throughout the function, so its global use_count is always
     * large.  Instead we mirror the C2 check: vA must not appear as a
     * source after the MOV before vA is redefined.
     */
    count_uses(head);
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_ALU) continue;
        int vA = p->rd;
        if (vA < VREG_START) continue;

        SSAInst *q = next_live(p);
        if (!q || q->op != SSA_MOV || q->rs1 != vA) continue;
        int vD = q->rd;
        if (used_before_redef(p, q, vA)) continue;
        if (used_before_redef(q, NULL, vA)) continue;

        /* E3 is incorrect when vA==VREG_START (r0 as ALU destination);
         * disable those cases until root cause is identified. */
        if (vA == VREG_START) continue;

        p->rd = vD;
        mark_dead(q);
    }

    /* ----- Pass 9: E5 — Dead MOV DCE ----- *
     *
     * After E3 (and other passes), some SSA_MOV nodes may have a
     * destination virtual register that is never used. Kill them.
     */
    count_uses(head);
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_MOV) continue;
        int vA = p->rd;
        if (vA < VREG_START) continue;
        if (use_count[vA - VREG_START] == 0)
            mark_dead(p);
    }

    /* ----- Pass 10: F1 — MOVI/MOVSYM + MOV → retarget immediate load ----- *
     *
     * Pattern:
     *   SSA_MOVI   rd=vA, imm=K          (or SSA_MOVSYM rd=vA, sym=S)
     *   SSA_MOV    rd=vD, rs1=vA
     *
     * Retarget the MOVI/MOVSYM to write directly into vD and kill the MOV.
     *
     * Example: immw r0,2; or r1,r0,r0  →  immw r1,2
     */
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0) continue;
        if (p->op != SSA_MOVI && p->op != SSA_MOVSYM) continue;
        int vA = p->rd;
        if (vA < VREG_START) continue;
        SSAInst *q = next_live(p);
        if (!q || q->op != SSA_MOV || q->rs1 != vA) continue;
        int vD = q->rd;
        if (used_before_redef(q, NULL, vA)) continue;
        p->rd = vD;
        mark_dead(q);
    }

    /* ----- Pass 11: F2 — redundant consecutive SXW/SXB DCE ----- *
     *
     * sxw16(sxw16(x)) == sxw16(x): a second identical in-place sign-extend
     * is a no-op.  Kill it.  This cleans up double-SXW sequences that arise
     * from both insert_coercions and explicit (type) casts, and prevents a
     * spurious "sxw r0" from appearing after an E4-generated "llwx r0,rx,N".
     *
     * Example: sxw r0; sxw r0  →  sxw r0
     */
    for (SSAInst *p = head; p; p = p->next) {
        if ((int)p->op < 0 || p->op != SSA_ALU1) continue;
        if (p->alu_op != IR_SXW && p->alu_op != IR_SXB) continue;
        int vA = p->rd;   /* == p->rs1 (in-place) */
        SSAInst *q = next_live(p);
        if (!q || q->op != SSA_ALU1) continue;
        if (q->rd != vA || q->rs1 != vA || q->alu_op != p->alu_op) continue;
        mark_dead(q);
    }

}
