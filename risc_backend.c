/* risc_backend.c — IR3 → CPU4 assembly emitter.
 *
 * Walks the post-regalloc IR3Inst list (all registers physical 0-7)
 * and emits CPU4 assembly text to the output file (set by set_asm_out()).
 *
 * Instruction selection:
 *   IR3_LOAD  rs1==-2            → F2  lw/lb/ll  rx, imm7
 *   IR3_LOAD  rs1>=0             → F3b llw/llb/lll rx, ry, 0
 *   IR3_STORE rd==-2             → F2  sw/sb/sl  rx, imm7
 *   IR3_STORE rd>=0              → F3b slw/slb/sll rx, ry, 0
 *   IR3_ALU   (le/ge/les/ges/…)  → assembler pseudo-ops (operand swap, single insn)
 *   IR3_ALU1  itof/ftoi          → F0  itof / ftoi (r0 implicit)
 *   IR3_ALU1  sxb/sxw            → F1b sxb/sxw rd
 *   IR3_CONST sym==NULL          → immw rd, lo [+ immwh rd, hi]
 *   IR3_CONST sym!=NULL          → immw rd, sym
 *   IR3_ADJ                      → adjw imm  (always adjw; no 8-bit adj)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smallcc.h"
#include "ir3.h"

/* Forward declarations */
static void rb_emit_src_comment(int line);
static int f2_word_ok(int byte_off);
static int f2_byte_ok(int byte_off);
static int f2_long_ok(int byte_off);

/* Label-bitset dimensions (must be defined before the rb_fwd_label_off table) */
#define RB_LABEL_MAX 16384

/* ----------------------------------------------------------------
 * E4 lookahead helpers
 * ---------------------------------------------------------------- */
static IR3Inst *next_live_rb(IR3Inst *p)
{
    for (p = p->next; p; p = p->next) {
        if ((int)p->op < 0) continue;  /* rb_mark_dead */
        /* Identity moves (rd == rs1, including rd == NONE dead phi remnants)
         * are not emitted; skip them so the E4 sign-extend peephole can see
         * through the MOV ACCUM←vN that braun inserts before every SXW. */
        if (p->op == IR3_MOV && (p->rd == p->rs1 || p->rd == IR3_VREG_NONE))
            continue;
        break;
    }
    return p;
}

static void rb_mark_dead(IR3Inst *p) { p->op = (IR3Op)-1; }

/* ----------------------------------------------------------------
 * rb_instr_size — conservative upper-bound byte size of one IR3Inst.
 * Used by the forward-branch fusion pre-pass to estimate label offsets.
 * Returns 0 for dead nodes and structural pseudo-nodes that emit no bytes.
 * ---------------------------------------------------------------- */
static int rb_instr_size(IR3Inst *p)
{
    if (!p || (int)p->op < 0) return 0;
    switch (p->op) {
    case IR3_LABEL:
    case IR3_SYMLABEL:
    case IR3_COMMENT:
    case IR3_PHI:
        return 0;
    case IR3_RET:
    case IR3_CALLR:
    case IR3_PUTCHAR:
        return 1;  /* F0 */
    case IR3_ALU1:
        if (p->alu_op == IR_ITOF || p->alu_op == IR_FTOI) return 1; /* F0 */
        return 2;  /* F1b: sxb/sxw/zxb/zxw */
    case IR3_MOV:
        if (p->rd == p->rs1 || p->rd == IR3_VREG_NONE) return 0; /* elided */
        return 2;  /* F1a */
    case IR3_ALU:
        if (p->rs2 == IR3_VREG_NONE) {
            int C = p->imm;
            if (!flag_no_newinsns) {
                if ((p->alu_op == IR_AND || p->alu_op == IR_SHL)
                        && C >= -64 && C <= 63)
                    return 2; /* F2 in-place andi/shli */
                if (p->alu_op == IR_AND && p->rd != p->rs1)
                    return 3; /* F3b andi3 */
                if ((p->alu_op == IR_SHR || p->alu_op == IR_SHRS)
                        && C >= 0 && C <= 31)
                    return 3; /* F3b shri/shrsi */
            }
            return 5; /* immw + reg-reg op fallback */
        }
        return 2; /* F1a */
    case IR3_LOAD:
        if (p->rs1 == IR3_VREG_BP) {
            if ((p->size == 2 && f2_word_ok(p->imm)) ||
                (p->size == 1 && f2_byte_ok(p->imm)) ||
                (p->size == 4 && f2_long_ok(p->imm)))
                return 2; /* F2 */
            return 6; /* lea + lll/llw/llb fallback */
        }
        return 3; /* F3b */
    case IR3_STORE:
        if (p->rd == IR3_VREG_BP) {
            if ((p->size == 2 && f2_word_ok(p->imm)) ||
                (p->size == 1 && f2_byte_ok(p->imm)) ||
                (p->size == 4 && f2_long_ok(p->imm)))
                return 2; /* F2 */
            return 9; /* worst-case fallback (save/restore scratch reg) */
        }
        return 3; /* F3b */
    case IR3_CONST:
        if (p->sym) return 3; /* immw sym (F3c) */
        if ((unsigned)p->imm <= 0xffff) return 3; /* immw (F3c) */
        return 6; /* immw + immwh */
    case IR3_LEA:
        return 3; /* F3c */
    case IR3_J:
    case IR3_JZ:
    case IR3_JNZ:
        return 3; /* F3a */
    case IR3_CALL:
        return 3; /* F3a jl */
    case IR3_ENTER:
        return 3; /* F3a enter */
    case IR3_ADJ:
        return (p->imm != 0) ? 3 : 0; /* F3a adjw or elided */
    case IR3_WORD:
        return 2;
    case IR3_BYTE:
        return 1;
    case IR3_ALIGN:
        return 0; /* conservative: ignore alignment padding in estimates */
    default:
        return 3; /* conservative upper bound */
    }
}

/* Static table: estimated byte offset of each numeric label in the current
 * risc_backend_emit call.  Built by the forward-branch pre-pass at the top
 * of risc_backend_emit.  Indexed by label id; -1 = unknown / unseen. */
static int rb_fwd_label_off[RB_LABEL_MAX];

/* ----------------------------------------------------------------
 * Helpers: F2 bp-relative address range check
 * ---------------------------------------------------------------- */

/* For F2 lw/sw: imm7 encodes WORD index; address = bp + sext(imm7)*2.
 * Valid when byte_offset is even and imm7 = byte_offset/2 fits in [-64..63]. */
static int f2_word_ok(int byte_off)
{
    if (byte_off & 1) return 0;
    int imm7 = byte_off / 2;
    return imm7 >= -64 && imm7 <= 63;
}

/* For F2 lb/sb: imm7 encodes BYTE index; address = bp + sext(imm7).
 * Valid when byte_offset fits in [-64..63]. */
static int f2_byte_ok(int byte_off)
{
    return byte_off >= -64 && byte_off <= 63;
}

/* For F2 ll/sl: imm7 encodes LONG index; address = bp + sext(imm7)*4.
 * Valid when byte_offset is divisible by 4 and imm7 = byte_offset/4
 * fits in [-64..63]. */
static int f2_long_ok(int byte_off)
{
    if (byte_off & 3) return 0;
    int imm7 = byte_off / 4;
    return imm7 >= -64 && imm7 <= 63;
}

/* ----------------------------------------------------------------
 * Emit a bp-relative load (F2 if in range, else F3b via lea+llw)
 * sign_ext: if non-zero, use sign-extending variants (lwx/lbx/llwx/llbx).
 *           Ignored for size==4 (no sign-extending long load exists).
 * ---------------------------------------------------------------- */
static void emit_bp_load(int rd, int byte_off, int size, int sign_ext,
                         const char *sym)
{
    const char *ann = (flag_annotate && sym) ? sym : NULL;
    if (size == 2 && f2_word_ok(byte_off)) {
        fprintf(asm_out, "    %-7s r%d, %d", sign_ext ? "lwx" : "lw",
                rd, byte_off / 2);
        if (ann) fprintf(asm_out, "  ; %s", ann);
        fputc('\n', asm_out);
    } else if (size == 1 && f2_byte_ok(byte_off)) {
        fprintf(asm_out, "    %-7s r%d, %d", sign_ext ? "lbx" : "lb",
                rd, byte_off);
        if (ann) fprintf(asm_out, "  ; %s", ann);
        fputc('\n', asm_out);
    } else if (size == 4 && f2_long_ok(byte_off)) {
        fprintf(asm_out, "    ll      r%d, %d", rd, byte_off / 4);
        if (ann) fprintf(asm_out, "  ; %s", ann);
        fputc('\n', asm_out);
    } else {
        /* Fallback: lea rd, byte_off; then load through rd */
        fprintf(asm_out, "    lea     r%d, %d\n", rd, byte_off);
        if      (size == 2)
            fprintf(asm_out, "    %-7s r%d, r%d, 0", sign_ext ? "llwx" : "llw", rd, rd);
        else if (size == 1)
            fprintf(asm_out, "    %-7s r%d, r%d, 0", sign_ext ? "llbx" : "llb", rd, rd);
        else
            fprintf(asm_out, "    lll     r%d, r%d, 0", rd, rd);
        if (ann) fprintf(asm_out, "  ; %s", ann);
        fputc('\n', asm_out);
    }
}

/* ----------------------------------------------------------------
 * Emit a bp-relative store (F2 if in range, else F3b via lea+slw)
 * ---------------------------------------------------------------- */
static void emit_bp_store(int val_reg, int byte_off, int size,
                          const char *sym)
{
    const char *ann = (flag_annotate && sym) ? sym : NULL;
    if (size == 2 && f2_word_ok(byte_off)) {
        fprintf(asm_out, "    sw      r%d, %d", val_reg, byte_off / 2);
        if (ann) fprintf(asm_out, "  ; %s", ann);
        fputc('\n', asm_out);
    } else if (size == 1 && f2_byte_ok(byte_off)) {
        fprintf(asm_out, "    sb      r%d, %d", val_reg, byte_off);
        if (ann) fprintf(asm_out, "  ; %s", ann);
        fputc('\n', asm_out);
    } else if (size == 4 && f2_long_ok(byte_off)) {
        fprintf(asm_out, "    sl      r%d, %d", val_reg, byte_off / 4);
        if (ann) fprintf(asm_out, "  ; %s", ann);
        fputc('\n', asm_out);
    } else {
        /* Fallback: use r7 for address, save/restore r7 via r0 or stack.
         * If val_reg == 7: we can't use r7 for address. Use push/pop to save r7,
         *   then lea r7, byte_off, move val from stack to r0, store r0 via r7.
         * If val_reg != 7: save r7 to r0, lea r7, byte_off, store val_reg via r7,
         *   restore r7 from r0. */
        if (val_reg == 7) {
            /* val is in r7. Save r0, move val to r0, compute addr in r7, store.
             * r0 still holds val after the store (store does not modify r0),
             * so restore r7 from r0 before restoring r0 from the stack. */
            fprintf(asm_out, "    push\n");                  /* save r0 */
            fprintf(asm_out, "    or      r0, r7, r7\n");   /* r0 = r7 (val) */
            fprintf(asm_out, "    lea     r7, %d\n", byte_off); /* r7 = address */
            if      (size == 2) fprintf(asm_out, "    slw     r0, r7, 0");
            else if (size == 1) fprintf(asm_out, "    slb     r0, r7, 0");
            else                fprintf(asm_out, "    sll     r0, r7, 0");
            if (ann) fprintf(asm_out, "  ; %s", ann);
            fputc('\n', asm_out);
            fprintf(asm_out, "    or      r7, r0, r0\n");  /* restore r7 (r0 still has val) */
            fprintf(asm_out, "    pop\n");                  /* restore r0 */
        } else if (val_reg == 0) {
            /* val_reg is 0. Value is in r0. Use pushr/popr to save/restore r7:
             * 1. pushr r7 (save r7 to stack)
             * 2. lea r7, byte_off (r7 = address)
             * 3. slw r0, r7, 0 (store r0 via r7)
             * 4. popr r7 (restore r7)
             * r0 is preserved throughout. */
            fprintf(asm_out, "    pushr   r7\n");             /* save r7 */
            fprintf(asm_out, "    lea     r7, %d\n", byte_off); /* r7 = address */
            if      (size == 2) fprintf(asm_out, "    slw     r0, r7, 0");
            else if (size == 1) fprintf(asm_out, "    slb     r0, r7, 0");
            else                fprintf(asm_out, "    sll     r0, r7, 0");
            if (ann) fprintf(asm_out, "  ; %s", ann);
            fputc('\n', asm_out);
            fprintf(asm_out, "    popr    r7\n");             /* restore r7 */
        } else {
            /* val_reg is not 0 or 7. Value is in val_reg. Use pushr/popr to save/restore r7. */
            fprintf(asm_out, "    pushr   r7\n");             /* save r7 */
            fprintf(asm_out, "    lea     r7, %d\n", byte_off); /* r7 = address */
            if      (size == 2) fprintf(asm_out, "    slw     r%d, r7, 0", val_reg);
            else if (size == 1) fprintf(asm_out, "    slb     r%d, r7, 0", val_reg);
            else                fprintf(asm_out, "    sll     r%d, r7, 0", val_reg);
            if (ann) fprintf(asm_out, "  ; %s", ann);
            fputc('\n', asm_out);
            fprintf(asm_out, "    popr    r7\n");             /* restore r7 */
        }
    }
}

/* ----------------------------------------------------------------
 * Map IROp binary opcode to CPU4 mnemonic string (or NULL if special)
 * ---------------------------------------------------------------- */
static const char *alu_mnemonic(IROp op)
{
    switch (op) {
    case IR_ADD:  return "add";
    case IR_SUB:  return "sub";
    case IR_MUL:  return "mul";
    case IR_DIV:  return "div";
    case IR_MOD:  return "mod";
    case IR_SHL:  return "shl";
    case IR_SHR:  return "shr";
    case IR_EQ:   return "eq";
    case IR_NE:   return "ne";
    case IR_LT:   return "lt";
    case IR_GT:   return "gt";
    case IR_LE:   return "le";
    case IR_GE:   return "ge";
    case IR_AND:  return "and";
    case IR_OR:   return "or";
    case IR_XOR:  return "xor";
    case IR_LTS:  return "lts";
    case IR_GTS:  return "gts";
    case IR_LES:  return "les";
    case IR_GES:  return "ges";
    case IR_DIVS: return "divs";
    case IR_MODS: return "mods";
    case IR_SHRS: return "shrs";
    case IR_FADD: return "fadd";
    case IR_FSUB: return "fsub";
    case IR_FMUL: return "fmul";
    case IR_FDIV: return "fdiv";
    case IR_FLT:  return "flt";
    case IR_FGT:  return "fgt";
    case IR_FLE:  return "fle";
    case IR_FGE:  return "fge";
    default:      return NULL;
    }
}

/* ----------------------------------------------------------------
 * Annotation: emit source-line comment
 * ---------------------------------------------------------------- */
static void rb_emit_src_comment(int line)
{
    if (!flag_annotate || line < 1 || !ann_lines || line > ann_nlines) return;
    const char *s = ann_lines[line - 1];
    const char *e = s;
    while (*e && *e != '\n') e++;
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    if (s < e)
        fprintf(asm_out, "; %.*s\n", (int)(e - s), s);
}

/* ----------------------------------------------------------------
 * risc_backend_emit — main emission pass
 * ---------------------------------------------------------------- */
void risc_backend_emit(IR3Inst *head)
{
    if (!asm_out) asm_out = stdout;

    int prev_line = 0;
    /* Backward-branch tracker for compare+branch fusion.
     * Track exactly which numeric label IDs have been emitted in the current
     * function.  A branch is a backward branch iff its target is in this set.
     * Using a bitset rather than "highest ID seen" to correctly handle
     * loop-rotated code where labels are emitted out of numeric order.
     * (The old "highest ID" check fired incorrectly when loop rotation emitted
     * the check label before the body label, causing fused forward branches
     * whose large offsets silently overflow the F3b imm10 field.) */
    uint64_t rb_seen[(RB_LABEL_MAX + 63) / 64];
    memset(rb_seen, 0, sizeof(rb_seen));

    /* Byte-boundedness tracker: rb_byte_bnd[r] is true when physical register r
     * is known to hold a value with bits [31:8] == 0.  Used by E5 to skip ZXB
     * when the AND-0xff target is already bounded (e.g. andi rX,_,1 then xor).
     * Reset at every BB boundary (LABEL / SYMLABEL). */
    bool rb_byte_bnd[8];
    memset(rb_byte_bnd, 0, sizeof(rb_byte_bnd));
#define RB_MARK(id) do { if ((id)>=0&&(id)<RB_LABEL_MAX) rb_seen[(id)>>6] |= 1ULL<<((id)&63); } while(0)
#define RB_TEST(id) ((id)>=0 && (id)<RB_LABEL_MAX && ((rb_seen[(id)>>6]>>((id)&63))&1))

    /* Forward-branch pre-pass: scan the full list and record the estimated byte
     * offset of each IR3_LABEL.  Used by the compare+branch fusion below to
     * allow forward branches when the target is provably within the F3b imm10
     * ±511-byte range.  The estimate uses rb_instr_size() as an upper bound,
     * so if the check says "in range" the actual offset is guaranteed to fit. */
    memset(rb_fwd_label_off, -1, sizeof(rb_fwd_label_off));
    {
        int off = 0;
        for (IR3Inst *q = head; q; q = q->next) {
            if (q->op == IR3_LABEL && q->imm >= 0 && q->imm < RB_LABEL_MAX)
                rb_fwd_label_off[q->imm] = off;
            off += rb_instr_size(q);
        }
    }

    int emit_offset = 0;  /* running estimated byte offset; tracks rb_fwd_label_off */

    for (IR3Inst *p = head; p; p = p->next)
    {
        /* Skip dead nodes */
        if ((int)p->op < 0) continue;

        /* Source annotation */
        if (flag_annotate && p->line && p->line != prev_line &&
            p->op != IR3_LABEL && p->op != IR3_SYMLABEL &&
            p->op != IR3_COMMENT)
        {
            rb_emit_src_comment(p->line);
            prev_line = p->line;
        }

        switch (p->op)
        {
        /* ---- Labels ---- */
        case IR3_LABEL:
            fprintf(asm_out, "_l%d:\n", p->imm);
            RB_MARK(p->imm);
            memset(rb_byte_bnd, 0, sizeof(rb_byte_bnd));
            break;

        case IR3_SYMLABEL:
            fprintf(asm_out, "%s:\n", p->sym);
            memset(rb_seen, 0, sizeof(rb_seen));  /* new function: reset backward-branch tracker */
            memset(rb_byte_bnd, 0, sizeof(rb_byte_bnd));
            break;

        case IR3_COMMENT:
            fprintf(asm_out, ";%s\n", p->sym);
            break;

        /* ---- Immediate loads (integer const or symbolic address) ---- */
        case IR3_CONST:
            if (p->sym) {
                fprintf(asm_out, "    immw    r%d, %s\n", p->rd, p->sym);
            } else {
                unsigned u  = (unsigned)p->imm;
                /* E5: CONST 0xff/0xffff + AND rd, rd, rX → zxb/zxw rd (in-place).
                 * Absorb the immw into the following AND when rd==rs1 and rs2==this reg.
                 * Disabled by -no-newinsns. */
                if (!flag_no_newinsns && (u == 0xff || u == 0xffff)) {
                    IR3Inst *nxt = next_live_rb(p);
                    if (nxt && nxt->op == IR3_ALU && nxt->alu_op == IR_AND
                            && nxt->rd == nxt->rs1   /* in-place AND */
                            && nxt->rs2 == p->rd)    /* uses our const register */
                    {
                        /* If the AND target is already byte-bounded (e.g. the XOR
                         * of two andi-1 results), the ZXB is redundant: kill both. */
                        if (u == 0xff && nxt->rs1 >= 0 && nxt->rs1 < 8
                                && rb_byte_bnd[nxt->rs1]) {
                            rb_mark_dead(p);
                            rb_mark_dead(nxt);
                            break;
                        }
                        rb_mark_dead(p);             /* suppress immw */
                        nxt->op     = IR3_ALU1;
                        nxt->alu_op = (u == 0xff) ? IR_ZXB : IR_ZXW;
                        nxt->rs1    = nxt->rd;
                        nxt->rs2    = IR3_VREG_NONE;
                        break;
                    }
                }
                unsigned lo = u & 0xffff;
                unsigned hi = (u >> 16) & 0xffff;
                fprintf(asm_out, "    immw    r%d, 0x%04x\n", p->rd, lo);
                if (hi)
                    fprintf(asm_out, "    immwh   r%d, 0x%04x\n", p->rd, hi);
                if (p->rd >= 0 && p->rd < 8)
                    rb_byte_bnd[p->rd] = (u <= 0xff);
            }
            break;

        /* ---- LEA (address of local) ---- */
        case IR3_LEA:
            fprintf(asm_out, "    lea     r%d, %d\n", p->rd, p->imm);
            break;

        /* ---- Register move ---- */
        case IR3_MOV:
            if (p->rd == IR3_VREG_NONE) continue;  /* dead phi residual: skip */
            if (p->rd == p->rs1) break;             /* identity move; skip */
            /* Emit as: or rd, rs1, rs1 (CPU4 pseudo: mov rd, rs1) */
            fprintf(asm_out, "    or      r%d, r%d, r%d\n", p->rd, p->rs1, p->rs1);
            if (p->rd >= 0 && p->rd < 8)
                rb_byte_bnd[p->rd] = (p->rs1 >= 0 && p->rs1 < 8 && rb_byte_bnd[p->rs1]);
            break;

        /* ---- Loads ---- */
        case IR3_LOAD:
        {
            /* E4: LOAD+SXW/SXB → sign-extending LOAD.
             * If the next live node is an in-place sign-extend of the load
             * result, absorb it and use lwx/lbx / llwx/llbx instead. */
            IR3Inst *nxt = next_live_rb(p);
            int sign_ext = (nxt && nxt->op == IR3_ALU1
                            && nxt->rd  == p->rd && nxt->rs1 == p->rd
                            && ((nxt->alu_op == IR_SXW && p->size == 2) ||
                                (nxt->alu_op == IR_SXB && p->size == 1)));
            if (sign_ext) rb_mark_dead(nxt);

            if (p->rs1 == IR3_VREG_BP) {
                /* bp-relative (F2) */
                emit_bp_load(p->rd, p->imm, p->size, sign_ext, p->sym);
            } else {
                /* Register-relative (F3b): rd = mem[rs1 + imm * scale] */
                int base = p->rs1;
                int off  = p->imm;
                if (p->size == 2) {
                    int w = off / 2;
                    fprintf(asm_out, "    %-7s r%d, r%d, %d\n",
                            sign_ext ? "llwx" : "llw", p->rd, base, w);
                } else if (p->size == 1) {
                    fprintf(asm_out, "    %-7s r%d, r%d, %d\n",
                            sign_ext ? "llbx" : "llb", p->rd, base, off);
                } else {
                    int l = off / 4;
                    fprintf(asm_out, "    lll     r%d, r%d, %d\n", p->rd, base, l);
                }
            }
            if (p->rd >= 0 && p->rd < 8) rb_byte_bnd[p->rd] = false;
            break;
        }

        /* ---- Stores ---- */
        case IR3_STORE:
            if (p->rd == IR3_VREG_BP) {
                /* bp-relative store (F2 or fallback). */
                emit_bp_store(p->rs1, p->imm, p->size, p->sym);
            } else {
                /* Register-relative store (F3b):
                 * rd = base address register, rs1 = value register */
                int base = p->rd;
                int val  = p->rs1;
                int off  = p->imm;
                if (p->size == 2) {
                    int w = off / 2;
                    fprintf(asm_out, "    slw     r%d, r%d, %d\n", val, base, w);
                } else if (p->size == 1) {
                    fprintf(asm_out, "    slb     r%d, r%d, %d\n", val, base, off);
                } else {
                    int l = off / 4;
                    fprintf(asm_out, "    sll     r%d, r%d, %d\n", val, base, l);
                }
            }
            break;

        /* ---- 3-address ALU ---- */
        case IR3_ALU:
        {
            int rd  = p->rd;
            int rs1 = p->rs1;
            int rs2 = p->rs2;

            /* Immediate ADD: ir3_opt folded CONST+ADD (rs2=NONE sentinel, imm=C).
             * Emit addli/addi/inc/dec instead of immw+add.
             * Look-ahead: addli rD,rS,K + MOV rS←rD → in-place addi/inc/dec rS,K. */
            if (rs2 == IR3_VREG_NONE && p->alu_op == IR_ADD) {
                int C = p->imm;
                if (rd != rs1) {
                    /* Find the copy-back: scan forward skipping dead, identity
                     * moves, and IR3_ADJ (adjw doesn't affect registers). */
                    IR3Inst *nxt = NULL;
                    for (IR3Inst *q = p->next; q; q = q->next) {
                        if ((int)q->op < 0) continue;
                        if (q->op == IR3_MOV && (q->rd == q->rs1 ||
                                q->rd == IR3_VREG_NONE)) continue;
                        if (q->op == IR3_ADJ) continue; /* sp-only, skip */
                        if (q->op == IR3_MOV && q->rd == rs1 && q->rs1 == rd)
                            nxt = q;
                        break;
                    }
                    if (nxt) {
                        /* Safety: rd must not be live after the copy-back MOV.
                         * Scan forward; be conservative at labels and jumps. */
                        bool rd_live = false;
                        for (IR3Inst *q = nxt->next; q; q = q->next) {
                            if ((int)q->op < 0) continue;
                            /* Unconditional jump: done with this BB; rd not used */
                            if (q->op == IR3_J) break;
                            /* Labels, conditional branches, calls: conservative stop */
                            if (q->op == IR3_LABEL || q->op == IR3_SYMLABEL ||
                                    q->op == IR3_JZ || q->op == IR3_JNZ ||
                                    q->op == IR3_CALL || q->op == IR3_CALLR) {
                                rd_live = true; break;
                            }
                            /* STORE: rd field is a base-address read, not a def */
                            if (q->op == IR3_STORE && q->rd == rd)
                                { rd_live = true; break; }
                            if (q->rs1 == rd || q->rs2 == rd) { rd_live = true; break; }
                            if (q->rd == rd) break; /* rd redefined: dead above */
                        }
                        if (rd_live) goto skip_fold;
                        /* Fold: kill the copy-back MOV, emit in-place op on rs1 */
                        rb_mark_dead(nxt);
                        if (C == 1)             fprintf(asm_out, "    inc     r%d\n", rs1);
                        else if (C == -1)       fprintf(asm_out, "    dec     r%d\n", rs1);
                        else if (C >= -64 && C <= 63)
                                                fprintf(asm_out, "    addi    r%d, %d\n", rs1, C);
                        else                    fprintf(asm_out, "    addli   r%d, r%d, %d\n", rs1, rs1, C);
                        if (rs1 >= 0 && rs1 < 8) rb_byte_bnd[rs1] = false;
                        break;
                    }
                }
                skip_fold:;
                if (rd == rs1 && C == 1)
                    fprintf(asm_out, "    inc     r%d\n", rd);
                else if (rd == rs1 && C == -1)
                    fprintf(asm_out, "    dec     r%d\n", rd);
                else if (rd == rs1 && C >= -64 && C <= 63)
                    fprintf(asm_out, "    addi    r%d, %d\n", rd, C);
                else
                    fprintf(asm_out, "    addli   r%d, r%d, %d\n", rd, rs1, C);
                if (rd >= 0 && rd < 8) rb_byte_bnd[rd] = false;
                break;
            }

            /* Immediate AND: andi (F2 in-place when rd==rs1, F3b 0xdf when rd!=rs1).
             * Fallback to immw+and when -no-newinsns is set or imm out of range. */
            if (rs2 == IR3_VREG_NONE && p->alu_op == IR_AND) {
                int C = p->imm;
                if (!flag_no_newinsns && C >= -64 && C <= 63) {
                    if (rd == rs1)
                        fprintf(asm_out, "    andi    r%d, %d\n", rd, C);
                    else
                        fprintf(asm_out, "    andi    r%d, r%d, %d\n", rd, rs1, C);
                } else {
                    fprintf(asm_out, "    immw    r%d, 0x%04x\n", rd, (unsigned)C & 0xffff);
                    fprintf(asm_out, "    and     r%d, r%d, r%d\n", rd, rs1, rd);
                }
                if (rd >= 0 && rd < 8) rb_byte_bnd[rd] = ((uint32_t)(unsigned)C <= 0xff);
                break;
            }

            /* Immediate SHL: shli (F2 in-place when rd==rs1, F3b 0xdf when rd!=rs1).
             * Fallback to immw+shl when -no-newinsns is set or imm out of range. */
            if (rs2 == IR3_VREG_NONE && p->alu_op == IR_SHL) {
                int C = p->imm;
                if (!flag_no_newinsns && C >= 0 && C <= 31) {
                    if (rd == rs1)
                        fprintf(asm_out, "    shli    r%d, %d\n", rd, C);
                    else
                        fprintf(asm_out, "    shli    r%d, r%d, %d\n", rd, rs1, C);
                } else {
                    fprintf(asm_out, "    immw    r%d, 0x%04x\n", rd, (unsigned)C & 0xffff);
                    fprintf(asm_out, "    shl     r%d, r%d, r%d\n", rd, rs1, rd);
                }
                if (rd >= 0 && rd < 8) rb_byte_bnd[rd] = false;
                break;
            }

            /* Immediate SHR (logical): shri (F3b 0xdf sub-op 2, always 3-operand).
             * Fallback to immw+shr when -no-newinsns is set or imm out of range. */
            if (rs2 == IR3_VREG_NONE && p->alu_op == IR_SHR) {
                int C = p->imm;
                if (!flag_no_newinsns && C >= 0 && C <= 31)
                    fprintf(asm_out, "    shri    r%d, r%d, %d\n", rd, rs1, C);
                else {
                    fprintf(asm_out, "    immw    r%d, 0x%04x\n", rd, (unsigned)C & 0xffff);
                    fprintf(asm_out, "    shr     r%d, r%d, r%d\n", rd, rs1, rd);
                }
                if (rd >= 0 && rd < 8) rb_byte_bnd[rd] = false;
                break;
            }

            /* Immediate SHRS (arithmetic): shrsi (F3b 0xdf sub-op 3, always 3-operand).
             * Fallback to immw+shrs when -no-newinsns is set or imm out of range. */
            if (rs2 == IR3_VREG_NONE && p->alu_op == IR_SHRS) {
                int C = p->imm;
                if (!flag_no_newinsns && C >= 0 && C <= 31)
                    fprintf(asm_out, "    shrsi   r%d, r%d, %d\n", rd, rs1, C);
                else {
                    fprintf(asm_out, "    immw    r%d, 0x%04x\n", rd, (unsigned)C & 0xffff);
                    fprintf(asm_out, "    shrs    r%d, r%d, r%d\n", rd, rs1, rd);
                }
                if (rd >= 0 && rd < 8) rb_byte_bnd[rd] = false;
                break;
            }

            /* Compare+branch fusion: if this comparison writes r0 (ACCUM) and
             * the next live instruction is JNZ or JZ, emit a single fused branch
             * (beq/bne/blt/ble/blts/bles) instead of the two-instruction sequence.
             * After apply_colors, ACCUM (IR3_VREG_ACCUM=100) has been rewritten to
             * physical register 0, so check rd==0 here. */
            if (rd == 0) {
                IR3Inst *nxt = next_live_rb(p);
                /* Fuse when the branch target is:
                 * (a) a backward branch (label already emitted — RB_TEST), or
                 * (b) a forward branch whose estimated offset fits the F3b
                 *     imm10 signed ±511-byte range (pre-pass guaranteed to be
                 *     an upper bound, so "fits" → definitely fits at assemble time). */
                bool fuse_ok = false;
                if (nxt && (nxt->op == IR3_JNZ || nxt->op == IR3_JZ)) {
                    if (RB_TEST(nxt->imm)) {
                        fuse_ok = true; /* backward branch — always short */
                    } else if (nxt->imm >= 0 && nxt->imm < RB_LABEL_MAX
                               && rb_fwd_label_off[nxt->imm] >= 0) {
                        /* Forward branch: check estimated offset fits imm10.
                         * emit_offset is the address of the compare; the fused
                         * branch will be 3 bytes (F3b), so pc-after = emit_offset+3. */
                        int rel = rb_fwd_label_off[nxt->imm] - (emit_offset + 3);
                        fuse_ok = (rel >= -512 && rel <= 511);
                    }
                }
                if (fuse_ok) {
                    bool is_jz = (nxt->op == IR3_JZ);
                    const char *fmn = NULL;
                    bool fswap = false;
                    /* Map (alu_op, jz/jnz) to a branch mnemonic and operand order.
                     * swap=true means emit "fmn rs2, rs1" (reverse operands). */
                    if (!is_jz) {
                        /* JNZ: jump when comparison is true */
                        switch (p->alu_op) {
                        case IR_EQ:  fmn="beq";  break;
                        case IR_NE:  fmn="bne";  break;
                        case IR_LT:  fmn="blt";  break;
                        case IR_LE:  fmn="ble";  break;
                        case IR_GT:  fmn="blt";  fswap=true; break; /* bgt a,b => blt b,a */
                        case IR_GE:  fmn="ble";  fswap=true; break; /* bge a,b => ble b,a */
                        case IR_LTS: fmn="blts"; break;
                        case IR_LES: fmn="bles"; break;
                        case IR_GTS: fmn="blts"; fswap=true; break;
                        case IR_GES: fmn="bles"; fswap=true; break;
                        default: break;
                        }
                    } else {
                        /* JZ: jump when comparison is false (negated condition) */
                        switch (p->alu_op) {
                        case IR_EQ:  fmn="bne";  break;
                        case IR_NE:  fmn="beq";  break;
                        case IR_LT:  fmn="ble";  fswap=true; break; /* not lt => ge => ble b,a */
                        case IR_LE:  fmn="blt";  fswap=true; break; /* not le => gt => blt b,a */
                        case IR_GT:  fmn="ble";  break;              /* not gt => le => ble a,b */
                        case IR_GE:  fmn="blt";  break;              /* not ge => lt => blt a,b */
                        case IR_LTS: fmn="bles"; fswap=true; break;
                        case IR_LES: fmn="blts"; fswap=true; break;
                        case IR_GTS: fmn="bles"; break;
                        case IR_GES: fmn="blts"; break;
                        default: break;
                        }
                    }
                    if (fmn) {
                        int fa = fswap ? rs2 : rs1;
                        int fb = fswap ? rs1 : rs2;
                        fprintf(asm_out, "    %-7s r%d, r%d, _l%d\n",
                                fmn, fa, fb, nxt->imm);
                        rb_mark_dead(nxt);
                        break;
                    }
                }
            }

            const char *mn = alu_mnemonic(p->alu_op);
            if (mn) {
                /* Direct instruction: rd = rs1 op rs2
                 * le/ge and variants are assembler pseudo-ops (operand swap). */
                fprintf(asm_out, "    %-7s r%d, r%d, r%d\n", mn, rd, rs1, rs2);
            } else {
                fprintf(stderr, "risc_backend: unhandled ALU op %d\n", (int)p->alu_op);
            }
            /* XOR of two byte-bounded values is itself byte-bounded; all else clear. */
            if (rd >= 0 && rd < 8) {
                rb_byte_bnd[rd] = (p->alu_op == IR_XOR
                                   && rs1 >= 0 && rs1 < 8 && rs2 >= 0 && rs2 < 8
                                   && rb_byte_bnd[rs1] && rb_byte_bnd[rs2]);
            }
            break;
        }

        /* ---- Single-register ops ---- */
        case IR3_ALU1:
            switch (p->alu_op) {
            case IR_SXB:
                fprintf(asm_out, "    sxb     r%d\n", p->rd);
                if (p->rd >= 0 && p->rd < 8) rb_byte_bnd[p->rd] = false;
                break;
            case IR_SXW:
                fprintf(asm_out, "    sxw     r%d\n", p->rd);
                if (p->rd >= 0 && p->rd < 8) rb_byte_bnd[p->rd] = false;
                break;
            case IR_ZXB:
                fprintf(asm_out, "    zxb     r%d\n", p->rd);
                if (p->rd >= 0 && p->rd < 8) rb_byte_bnd[p->rd] = true;
                break;
            case IR_ZXW:
                fprintf(asm_out, "    zxw     r%d\n", p->rd);
                if (p->rd >= 0 && p->rd < 8) rb_byte_bnd[p->rd] = false;
                break;
            case IR_ITOF:
                fprintf(asm_out, "    itof\n");
                rb_byte_bnd[0] = false;  /* r0 implicit */
                break;
            case IR_FTOI:
                fprintf(asm_out, "    ftoi\n");
                rb_byte_bnd[0] = false;  /* r0 implicit */
                break;
            default:
                fprintf(stderr, "risc_backend: unhandled ALU1 op %d\n", (int)p->alu_op);
                break;
            }
            break;

        /* ---- Frame management ---- */
        case IR3_ENTER:
            fprintf(asm_out, "    enter   %d\n", p->imm);
            break;

        case IR3_ADJ:
            if (p->imm != 0)
                fprintf(asm_out, "    adjw    %d\n", p->imm);
            break;

        case IR3_RET:
            fprintf(asm_out, "    ret\n");
            break;

        /* ---- Calls ---- */
        case IR3_CALL:
            fprintf(asm_out, "    jl      %s\n", p->sym);
            break;

        case IR3_CALLR:
            fprintf(asm_out, "    jlr\n");
            break;

        /* ---- Branches ---- */
        case IR3_J:
            fprintf(asm_out, "    j       _l%d\n", p->imm);
            break;

        case IR3_JZ:
            fprintf(asm_out, "    jz      _l%d\n", p->imm);
            break;

        case IR3_JNZ:
            fprintf(asm_out, "    jnz     _l%d\n", p->imm);
            break;

        /* ---- Data section ---- */
        case IR3_WORD:
            if (p->sym)
                fprintf(asm_out, "    word    %s\n", p->sym);
            else
                fprintf(asm_out, "    word    0x%04x\n", p->imm & 0xffff);
            break;

        case IR3_BYTE:
            fprintf(asm_out, "    byte    0x%02x\n", p->imm & 0xff);
            break;

        case IR3_ALIGN:
            fprintf(asm_out, "    align\n");
            break;

        /* ---- Misc ---- */
        case IR3_PUTCHAR:
            fprintf(asm_out, "    putchar\n");
            break;

        case IR3_PHI:
            /* Phi nodes must be eliminated before reaching here */
            if (p->rd != IR3_VREG_NONE)
                fprintf(stderr, "risc_backend: unexpected live IR3_PHI (rd=%d)\n", p->rd);
            break;

        default:
            /* Unknown or dead op — skip */
            break;
        }

        emit_offset += rb_instr_size(p);
    }
}
