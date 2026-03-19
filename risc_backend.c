/* risc_backend.c — SSA → CPU4 assembly emitter.
 *
 * Walks the SSAInst list produced by lift_to_ssa() and emits CPU4 assembly
 * text to the output file (set by set_asm_out()).
 *
 * Instruction selection:
 *   SSA_LOAD  rs1==-2            → F2  lw/lb/ll  rx, imm7
 *   SSA_LOAD  rs1>=0             → F3b llw/llb/lll rx, ry, 0
 *   SSA_STORE rd==-2             → F2  sw/sb/sl  rx, imm7
 *   SSA_STORE rd>=0              → F3b slw/slb/sll rx, ry, 0
 *   SSA_ALU   (le/ge/les/ges/…)  → 3-instruction expansion (no direct encoding)
 *   SSA_ALU1  itof/ftoi          → F0  itof / ftoi (r0 implicit)
 *   SSA_ALU1  sxb/sxw            → F1b sxb/sxw rd
 *   SSA_MOVI  32-bit const       → immw rd, lo + immwh rd, hi
 *   SSA_ADJ                      → adjw imm  (always adjw; no 8-bit adj)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smallcc.h"
#include "ssa.h"

/* Forward declaration */
static void rb_emit_src_comment(int line);

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
 * ---------------------------------------------------------------- */
static void emit_bp_load(int rd, int byte_off, int size)
{
    if (size == 2 && f2_word_ok(byte_off)) {
        fprintf(asm_out, "    lw      r%d, %d\n", rd, byte_off / 2);
    } else if (size == 1 && f2_byte_ok(byte_off)) {
        fprintf(asm_out, "    lb      r%d, %d\n", rd, byte_off);
    } else if (size == 4 && f2_long_ok(byte_off)) {
        fprintf(asm_out, "    ll      r%d, %d\n", rd, byte_off / 4);
    } else {
        /* Fallback: lea rd, byte_off; then load through rd */
        fprintf(asm_out, "    lea     r%d, %d\n", rd, byte_off);
        if      (size == 2) fprintf(asm_out, "    llw     r%d, r%d, 0\n", rd, rd);
        else if (size == 1) fprintf(asm_out, "    llb     r%d, r%d, 0\n", rd, rd);
        else                fprintf(asm_out, "    lll     r%d, r%d, 0\n", rd, rd);
    }
}

/* ----------------------------------------------------------------
 * Emit a bp-relative store (F2 if in range, else F3b via lea+slw)
 * ---------------------------------------------------------------- */
static void emit_bp_store(int val_reg, int byte_off, int size, int scratch)
{
    if (size == 2 && f2_word_ok(byte_off)) {
        fprintf(asm_out, "    sw      r%d, %d\n", val_reg, byte_off / 2);
    } else if (size == 1 && f2_byte_ok(byte_off)) {
        fprintf(asm_out, "    sb      r%d, %d\n", val_reg, byte_off);
    } else if (size == 4 && f2_long_ok(byte_off)) {
        fprintf(asm_out, "    sl      r%d, %d\n", val_reg, byte_off / 4);
    } else {
        /* Fallback: lea scratch, byte_off; store via scratch */
        fprintf(asm_out, "    lea     r%d, %d\n", scratch, byte_off);
        if      (size == 2) fprintf(asm_out, "    slw     r%d, r%d, 0\n", val_reg, scratch);
        else if (size == 1) fprintf(asm_out, "    slb     r%d, r%d, 0\n", val_reg, scratch);
        else                fprintf(asm_out, "    sll     r%d, r%d, 0\n", val_reg, scratch);
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
    case IR_AND:  return "and";
    case IR_OR:   return "or";
    case IR_XOR:  return "xor";
    case IR_LTS:  return "lts";
    case IR_GTS:  return "gts";
    case IR_DIVS: return "divs";
    case IR_MODS: return "mods";
    case IR_SHRS: return "shrs";
    case IR_FADD: return "fadd";
    case IR_FSUB: return "fsub";
    case IR_FMUL: return "fmul";
    case IR_FDIV: return "fdiv";
    case IR_FLT:  return "flt";
    case IR_FGT:  return "fgt";
    default:      return NULL;   /* le/ge/les/ges/fle/fge need expansion */
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
void risc_backend_emit(SSAInst *head)
{
    if (!asm_out) asm_out = stdout;

    int prev_line = 0;

    for (SSAInst *p = head; p; p = p->next)
    {
        /* Skip dead nodes (marked by ssa_peephole) */
        if ((int)p->op < 0) continue;

        /* Source annotation */
        if (flag_annotate && p->line && p->line != prev_line &&
            p->op != SSA_LABEL && p->op != SSA_SYMLABEL &&
            p->op != SSA_COMMENT)
        {
            rb_emit_src_comment(p->line);
            prev_line = p->line;
        }

        switch (p->op)
        {
        /* ---- Labels ---- */
        case SSA_LABEL:
            fprintf(asm_out, "_l%d:\n", p->imm);
            break;

        case SSA_SYMLABEL:
            fprintf(asm_out, "%s:\n", p->sym);
            break;

        case SSA_COMMENT:
            fprintf(asm_out, ";%s\n", p->sym);
            break;

        /* ---- Immediate loads ---- */
        case SSA_MOVI:
        {
            unsigned u  = (unsigned)p->imm;
            unsigned lo = u & 0xffff;
            unsigned hi = (u >> 16) & 0xffff;
            fprintf(asm_out, "    immw    r%d, 0x%04x\n", p->rd, lo);
            if (hi)
                fprintf(asm_out, "    immwh   r%d, 0x%04x\n", p->rd, hi);
            break;
        }

        case SSA_MOVSYM:
            if (p->sym)
                fprintf(asm_out, "    immw    r%d, %s\n", p->rd, p->sym);
            else {
                /* Numeric immediate stored in imm field (symbolic was NULL) */
                unsigned u = (unsigned)p->imm;
                fprintf(asm_out, "    immw    r%d, 0x%04x\n", p->rd, u & 0xffff);
                if (u >> 16)
                    fprintf(asm_out, "    immwh   r%d, 0x%04x\n", p->rd, (u >> 16) & 0xffff);
            }
            break;

        /* ---- LEA (address of local) ---- */
        case SSA_LEA:
            fprintf(asm_out, "    lea     r%d, %d\n", p->rd, p->imm);
            break;

        /* ---- Register move ---- */
        case SSA_MOV:
            if (p->rd == p->rs1) break;  /* identity move; skip */
            /* Emit as: or rd, rs1, rs1 (CPU4 pseudo: mov rd, rs1) */
            fprintf(asm_out, "    or      r%d, r%d, r%d\n", p->rd, p->rs1, p->rs1);
            break;

        /* ---- Loads ---- */
        case SSA_LOAD:
            if (p->rs1 == -2) {
                /* bp-relative (F2) */
                emit_bp_load(p->rd, p->imm, p->size);
            } else {
                /* Register-relative (F3b): rd = mem[rs1 + imm * scale] */
                int base = p->rs1;
                int off  = p->imm;  /* byte offset → scale for F3b */
                if (p->size == 2) {
                    int w = off / 2;
                    fprintf(asm_out, "    llw     r%d, r%d, %d\n", p->rd, base, w);
                } else if (p->size == 1) {
                    fprintf(asm_out, "    llb     r%d, r%d, %d\n", p->rd, base, off);
                } else {
                    int l = off / 4;
                    fprintf(asm_out, "    lll     r%d, r%d, %d\n", p->rd, base, l);
                }
            }
            break;

        /* ---- Stores ---- */
        case SSA_STORE:
            if (p->rd == -2) {
                /* bp-relative store (F2); use rd+1 as scratch if needed */
                int scratch = (p->rs1 == 0) ? 1 : 0;
                emit_bp_store(p->rs1, p->imm, p->size, scratch);
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
        case SSA_ALU:
        {
            int rd  = p->rd;
            int rs1 = p->rs1;  /* left operand (from virtual stack) */
            int rs2 = p->rs2;  /* right operand (r0) */
            const char *mn = alu_mnemonic(p->alu_op);

            if (mn) {
                /* Direct instruction: rd = rs1 op rs2 */
                fprintf(asm_out, "    %-7s r%d, r%d, r%d\n", mn, rd, rs1, rs2);
            } else {
                /* Needs 3-instruction expansion (le/ge/les/ges/fle/fge).
                 * rs1 is now free (virtual stack was popped before lift_to_ssa
                 * generates this node), so we reuse it as a 0-constant reg.
                 *
                 *   le  a, b  = NOT(a > b):  gt rd,rs1,rs2; immw rs1,0; eq rd,rd,rs1
                 *   ge  a, b  = NOT(a < b):  lt rd,rs1,rs2; immw rs1,0; eq rd,rd,rs1
                 *   les a, b  = NOT(a gts b):gts rd,rs1,rs2; immw rs1,0; eq rd,rd,rs1
                 *   ges a, b  = NOT(a lts b):lts rd,rs1,rs2; immw rs1,0; eq rd,rd,rs1
                 *   fle a, b  = NOT(a fgt b):fgt rd,rs1,rs2; immw rs1,0; eq rd,rd,rs1
                 *   fge a, b  = NOT(a flt b):flt rd,rs1,rs2; immw rs1,0; eq rd,rd,rs1
                 */
                const char *inv;
                switch (p->alu_op) {
                case IR_LE:  inv = "gt";  break;
                case IR_GE:  inv = "lt";  break;
                case IR_LES: inv = "gts"; break;
                case IR_GES: inv = "lts"; break;
                case IR_FLE: inv = "fgt"; break;
                case IR_FGE: inv = "flt"; break;
                default:
                    fprintf(stderr, "risc_backend: unhandled ALU op %d\n", (int)p->alu_op);
                    inv = "add";
                    break;
                }
                /* Use rs1 as scratch for the 0 constant (rs1 is freed by pop) */
                fprintf(asm_out, "    %-7s r%d, r%d, r%d\n", inv, rd, rs1, rs2);
                fprintf(asm_out, "    immw    r%d, 0\n", rs1);
                fprintf(asm_out, "    eq      r%d, r%d, r%d\n", rd, rd, rs1);
            }
            break;
        }

        /* ---- Single-register ops ---- */
        case SSA_ALU1:
            switch (p->alu_op) {
            case IR_SXB:  fprintf(asm_out, "    sxb     r%d\n", p->rd); break;
            case IR_SXW:  fprintf(asm_out, "    sxw     r%d\n", p->rd); break;
            case IR_ITOF: fprintf(asm_out, "    itof\n");                break;
            case IR_FTOI: fprintf(asm_out, "    ftoi\n");                break;
            default:
                fprintf(stderr, "risc_backend: unhandled ALU1 op %d\n", (int)p->alu_op);
                break;
            }
            break;

        /* ---- Frame management ---- */
        case SSA_ENTER:
            fprintf(asm_out, "    enter   %d\n", p->imm);
            break;

        case SSA_ADJ:
            if (p->imm != 0)
                fprintf(asm_out, "    adjw    %d\n", p->imm);
            break;

        case SSA_RET:
            fprintf(asm_out, "    ret\n");
            break;

        /* ---- Calls ---- */
        case SSA_CALL:
            fprintf(asm_out, "    jl      %s\n", p->sym);
            break;

        case SSA_CALLR:
            fprintf(asm_out, "    jlr\n");
            break;

        /* ---- Branches ---- */
        case SSA_J:
            fprintf(asm_out, "    j       _l%d\n", p->imm);
            break;

        case SSA_JZ:
            fprintf(asm_out, "    jz      _l%d\n", p->imm);
            break;

        case SSA_JNZ:
            fprintf(asm_out, "    jnz     _l%d\n", p->imm);
            break;

        case SSA_BRANCH:
        {
            /* Fused compare-branch (B2): emit a single F3b branch instruction.
             * alu_op encodes the branch condition: IR_EQ→beq, IR_NE→bne,
             * IR_LT→blt, IR_GT→bgt, IR_LTS→blts, IR_GTS→bgts. */
            const char *mn;
            switch (p->alu_op) {
            case IR_EQ:  mn = "beq";  break;
            case IR_NE:  mn = "bne";  break;
            case IR_LT:  mn = "blt";  break;
            case IR_GT:  mn = "bgt";  break;
            case IR_LTS: mn = "blts"; break;
            case IR_GTS: mn = "bgts"; break;
            default:
                fprintf(stderr, "risc_backend: unhandled BRANCH op %d\n",
                        (int)p->alu_op);
                mn = "beq";
                break;
            }
            fprintf(asm_out, "    %-7s r%d, r%d, _l%d\n",
                    mn, p->rs1, p->rs2, p->imm);
            break;
        }

        case SSA_ALU_IMM:
        {
            /* Add-immediate (D4): rd = rs1 + K.
             * Emit the most compact form available:
             *   inc rd        (F1b, 2 bytes) when K=+1 and rd==rs1
             *   dec rd        (F1b, 2 bytes) when K=-1 and rd==rs1
             *   addi rd, K   (F2,  2 bytes) when |K|≤63 and rd==rs1
             *   addli rd,rs1,K (F3b, 3 bytes) otherwise (|K|≤511)
             * If K is outside addli range, fall back to immw + add. */
            int rd  = p->rd;
            int rs1 = p->rs1;
            int K   = p->imm;
            if (K == 1 && rd == rs1) {
                fprintf(asm_out, "    inc     r%d\n", rd);
            } else if (K == -1 && rd == rs1) {
                fprintf(asm_out, "    dec     r%d\n", rd);
            } else if (rd == rs1 && K >= -64 && K <= 63) {
                fprintf(asm_out, "    addi    r%d, %d\n", rd, K);
            } else if (K >= -512 && K <= 511) {
                fprintf(asm_out, "    addli   r%d, r%d, %d\n", rd, rs1, K);
            } else {
                /* Fallback: load K into rd, then add rs1 */
                unsigned u = (unsigned)K;
                fprintf(asm_out, "    immw    r%d, 0x%04x\n", rd, u & 0xffff);
                if (u >> 16)
                    fprintf(asm_out, "    immwh   r%d, 0x%04x\n", rd,
                            (u >> 16) & 0xffff);
                fprintf(asm_out, "    add     r%d, r%d, r%d\n", rd, rs1, rd);
            }
            break;
        }

        /* ---- Data section ---- */
        case SSA_WORD:
            if (p->sym)
                fprintf(asm_out, "    word    %s\n", p->sym);
            else
                fprintf(asm_out, "    word    0x%04x\n", p->imm & 0xffff);
            break;

        case SSA_BYTE:
            fprintf(asm_out, "    byte    0x%02x\n", p->imm & 0xff);
            break;

        case SSA_ALIGN:
            fprintf(asm_out, "    align\n");
            break;

        /* ---- Misc ---- */
        case SSA_PUTCHAR:
            fprintf(asm_out, "    putchar\n");
            break;

        default:
            /* Unknown or dead op — skip */
            break;
        }
    }
}
