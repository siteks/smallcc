/* ir3.c — IR3 infrastructure: vreg counter, CFG builder, free_ir3. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ir3.h"

/* ----------------------------------------------------------------
 * Fresh virtual register counter.
 * IR3_VREG_ACCUM (100) is reserved for the accumulator.
 * Fresh scratch vregs are allocated from 101 upward.
 * ---------------------------------------------------------------- */
static int next_vreg = IR3_VREG_BASE + 1;  /* 101; 100 = ACCUM, reserved */

int ir3_new_vreg(void)
{
    return next_vreg++;
}

void ir3_reset(void)
{
    next_vreg = IR3_VREG_BASE + 1;
}

/* ----------------------------------------------------------------
 * build_cfg — build a basic-block CFG from a stack-IR list.
 *
 * ir_head must be an IR_SYMLABEL node for a function.
 * Scans from ir_head->next until the next IR_SYMLABEL or NULL.
 *
 * BB layout:
 *   BB 0: ir_first = ir_head (the SYMLABEL node itself), ir_last = last
 *         instruction before the first IR_BB_START (or last instruction
 *         in function if no BB_START exists).
 *   BB k (k>0): ir_first = the kth IR_BB_START; ir_last = last instruction
 *         in that block.
 *
 * translate_bb iterates from ir_first->next to ir_last->next (exclusive),
 * so for BB 0 it starts at the instruction after the SYMLABEL (= ENTER),
 * and for subsequent BBs it starts at the instruction after the IR_BB_START.
 *
 * Returns NULL and *n_blocks_out=0 if no real BBs exist.
 * ---------------------------------------------------------------- */

static int find_bb_by_label(BB *blocks, int n, int label)
{
    for (int i = 0; i < n; i++)
        if (blocks[i].label_id == label) return i;
    return -1;
}

BB *build_cfg(IRInst *ir_head, int *n_blocks_out)
{
    if (!ir_head || !ir_head->next) {
        *n_blocks_out = 0;
        return NULL;
    }

    /* Pass 1: count total BBs.
     * BB 0 always exists (it covers instructions from ir_head->next until first BB_START).
     * Each IR_BB_START starts a new BB. */
    int n_bb = 1;  /* BB 0 always exists */
    for (IRInst *p = ir_head->next; p && p->op != IR_SYMLABEL; p = p->next)
        if (p->op == IR_BB_START) n_bb++;

    BB *blocks = calloc(n_bb, sizeof(BB));
    if (!blocks) { fprintf(stderr, "build_cfg: OOM\n"); exit(1); }

    /* Pass 2: fill BB array.
     * BB 0's ir_first = ir_head (SYMLABEL); subsequent BBs' ir_first = BB_START. */
    blocks[0].id       = 0;
    blocks[0].label_id = -1;
    blocks[0].sealed   = false;
    blocks[0].ir_first = ir_head;   /* translate_bb iterates from ir_first->next */
    blocks[0].ir_last  = ir_head;   /* will be updated */

    int bb_idx = 0;
    for (IRInst *p = ir_head->next; p && p->op != IR_SYMLABEL; p = p->next) {
        if (p->op == IR_BB_START) {
            bb_idx++;
            blocks[bb_idx].id       = bb_idx;
            blocks[bb_idx].label_id = -1;
            blocks[bb_idx].sealed   = false;
            blocks[bb_idx].ir_first = p;  /* translate_bb starts from p->next */
            blocks[bb_idx].ir_last  = p;  /* will be updated */
            continue;
        }

        /* Record label_id for the block (first IR_LABEL seen) */
        if (p->op == IR_LABEL && blocks[bb_idx].label_id < 0)
            blocks[bb_idx].label_id = p->operand;

        /* Update ir_last for every non-NOP instruction */
        if (p->op != IR_NOP)
            blocks[bb_idx].ir_last = p;
    }

    /* Pass 3: build succ/pred edges based on ir_last of each BB. */
    for (int i = 0; i < n_bb; i++) {
        IRInst *last = blocks[i].ir_last;
#define ADD_EDGE(from, to) \
    do { \
        if (blocks[from].n_succs >= BB_MAX_SUCCS) { \
            fprintf(stderr, "build_cfg: block %d exceeds BB_MAX_SUCCS %d\n", from, BB_MAX_SUCCS); exit(1); } \
        if (blocks[to].n_preds >= BB_MAX_PREDS) { \
            fprintf(stderr, "build_cfg: block %d exceeds BB_MAX_PREDS %d\n", to, BB_MAX_PREDS); exit(1); } \
        blocks[from].succs[blocks[from].n_succs++] = (to); \
        blocks[to].preds[blocks[to].n_preds++] = (from); \
    } while (0)

        if (!last || last->op == IR_BB_START) {
            /* Empty block or only had a BB_START: fall-through to next */
            int fall = i + 1;
            if (fall < n_bb) ADD_EDGE(i, fall);
            continue;
        }

        if (last->op == IR_J) {
            int tgt = find_bb_by_label(blocks, n_bb, last->operand);
            if (tgt >= 0) ADD_EDGE(i, tgt);
        } else if (last->op == IR_JZ || last->op == IR_JNZ) {
            int tgt  = find_bb_by_label(blocks, n_bb, last->operand);
            int fall = i + 1;
            if (fall < n_bb) ADD_EDGE(i, fall);
            if (tgt >= 0 && tgt != fall) ADD_EDGE(i, tgt);
        } else if (last->op == IR_RET) {
            /* No successors */
        } else if (last->op == IR_BB_START) {
            /* Shouldn't happen (already handled above) */
        } else {
            /* Fall-through to next BB */
            int fall = i + 1;
            if (fall < n_bb) ADD_EDGE(i, fall);
        }
    }

    *n_blocks_out = n_bb;
    return blocks;
}

void free_cfg(BB *blocks, int n_blocks)
{
    (void)n_blocks;
    free(blocks);
}

/* ----------------------------------------------------------------
 * ir3_dump — print the IR3 list in a human-readable SSA notation.
 * Called after ir3_optimize, before irc_regalloc.
 * ---------------------------------------------------------------- */

static const char *dump_reg(int v, char *buf, size_t cap)
{
    if (v == IR3_VREG_NONE)  { buf[0] = '-'; buf[1] = '\0'; return buf; }
    if (v == IR3_VREG_BP)    return "bp";
    if (v == IR3_VREG_ACCUM) return "acc";
    if (v >= 0 && v <= 7)    { snprintf(buf, cap, "r%d", v); return buf; }
    snprintf(buf, cap, "v%d", v);
    return buf;
}

static const char *dump_size(int sz)
{
    if (sz == 1) return "b";
    if (sz == 4) return "l";
    return "w";
}

static const char *dump_alu(IROp op)
{
    switch (op) {
    case IR_ADD:  return "add";  case IR_SUB:  return "sub";
    case IR_MUL:  return "mul";  case IR_DIV:  return "div";
    case IR_MOD:  return "mod";  case IR_SHL:  return "shl";
    case IR_SHR:  return "shr";  case IR_EQ:   return "eq";
    case IR_NE:   return "ne";   case IR_LT:   return "lt";
    case IR_GT:   return "gt";   case IR_LE:   return "le";
    case IR_GE:   return "ge";   case IR_AND:  return "and";
    case IR_OR:   return "or";   case IR_XOR:  return "xor";
    case IR_LTS:  return "lts";  case IR_GTS:  return "gts";
    case IR_LES:  return "les";  case IR_GES:  return "ges";
    case IR_DIVS: return "divs"; case IR_MODS: return "mods";
    case IR_SHRS: return "shrs"; case IR_FADD: return "fadd";
    case IR_FSUB: return "fsub"; case IR_FMUL: return "fmul";
    case IR_FDIV: return "fdiv"; case IR_FLT:  return "flt";
    case IR_FGT:  return "fgt";  case IR_FLE:  return "fle";
    case IR_FGE:  return "fge";  case IR_SXB:  return "sxb";
    case IR_SXW:  return "sxw";  case IR_ITOF: return "itof";
    case IR_FTOI: return "ftoi";
    default:      return "?";
    }
}

void ir3_dump(IR3Inst *head, FILE *f)
{
    char a[16], b[16], c[16];
    int prev_line = 0;
    for (IR3Inst *p = head; p; p = p->next) {
        /* Skip dead value-producing nodes (DCE kills phi/mov with rd=NONE) */
        switch (p->op) {
        case IR3_MOV: case IR3_ALU: case IR3_ALU1: case IR3_CONST:
        case IR3_LOAD: case IR3_LEA: case IR3_PHI:
            if (p->rd == IR3_VREG_NONE) continue;
            break;
        default: break;
        }
        /* Emit source annotation when the line number changes */
        if (p->line && p->line != prev_line &&
            ann_lines && p->line >= 1 && p->line <= ann_nlines) {
            const char *s = ann_lines[p->line - 1];
            const char *e = s;
            while (*e && *e != '\n') e++;
            while (s < e && (*s == ' ' || *s == '\t')) s++;
            if (s < e)
                fprintf(f, "  ; %.*s\n", (int)(e - s), s);
            prev_line = p->line;
        }
        switch (p->op) {
        case IR3_SYMLABEL:
            prev_line = 0;  /* reset at each function boundary */
            fprintf(f, "\n%s:\n", p->sym ? p->sym : "?");
            break;
        case IR3_LABEL:
            fprintf(f, "_l%d:\n", p->imm);
            break;
        case IR3_ENTER:
            fprintf(f, "  enter %d\n", p->imm);
            break;
        case IR3_ADJ:
            if (p->imm) fprintf(f, "  adj %d\n", p->imm);
            break;
        case IR3_RET:
            fprintf(f, "  ret\n");
            break;
        case IR3_J:
            fprintf(f, "  j _l%d\n", p->imm);
            break;
        case IR3_JZ:
            fprintf(f, "  jz _l%d\n", p->imm);
            break;
        case IR3_JNZ:
            fprintf(f, "  jnz _l%d\n", p->imm);
            break;
        case IR3_CONST:
            if (p->sym)
                fprintf(f, "  %s = &%s\n", dump_reg(p->rd, a, sizeof a), p->sym);
            else
                fprintf(f, "  %s = %d\n",  dump_reg(p->rd, a, sizeof a), p->imm);
            break;
        case IR3_MOV:
            fprintf(f, "  %s = %s\n",
                    dump_reg(p->rd,  a, sizeof a),
                    dump_reg(p->rs1, b, sizeof b));
            break;
        case IR3_LEA:
            fprintf(f, "  %s = &bp[%d]\n", dump_reg(p->rd, a, sizeof a), p->imm);
            break;
        case IR3_LOAD:
            if (p->rs1 == IR3_VREG_BP)
                fprintf(f, "  %s = bp[%d]%s\n",
                        dump_reg(p->rd, a, sizeof a), p->imm, dump_size(p->size));
            else
                fprintf(f, "  %s = [%s+%d]%s\n",
                        dump_reg(p->rd, a, sizeof a),
                        dump_reg(p->rs1, b, sizeof b), p->imm, dump_size(p->size));
            break;
        case IR3_STORE:
            if (p->rd == IR3_VREG_BP)
                fprintf(f, "  bp[%d]%s = %s\n",
                        p->imm, dump_size(p->size), dump_reg(p->rs1, b, sizeof b));
            else
                fprintf(f, "  [%s+%d]%s = %s\n",
                        dump_reg(p->rd, a, sizeof a), p->imm, dump_size(p->size),
                        dump_reg(p->rs1, b, sizeof b));
            break;
        case IR3_ALU:
            fprintf(f, "  %s = %s %s %s\n",
                    dump_reg(p->rd,  a, sizeof a),
                    dump_reg(p->rs1, b, sizeof b),
                    dump_alu(p->alu_op),
                    dump_reg(p->rs2, c, sizeof c));
            break;
        case IR3_ALU1:
            fprintf(f, "  %s = %s(%s)\n",
                    dump_reg(p->rd,  a, sizeof a),
                    dump_alu(p->alu_op),
                    dump_reg(p->rs1, b, sizeof b));
            break;
        case IR3_CALL:
            fprintf(f, "  acc = call %s\n", p->sym ? p->sym : "?");
            break;
        case IR3_CALLR:
            fprintf(f, "  acc = callr\n");
            break;
        case IR3_PHI: {
            fprintf(f, "  %s = phi(", dump_reg(p->rd, a, sizeof a));
            for (int i = 0; i < p->n_phi_ops; i++) {
                if (i) fprintf(f, ", ");
                fprintf(f, "%s", dump_reg(p->phi_ops[i], b, sizeof b));
            }
            fprintf(f, ")\n");
            break;
        }
        case IR3_WORD:
            fprintf(f, "  .word %s%d\n", p->sym ? p->sym : "", p->sym ? 0 : p->imm);
            break;
        case IR3_BYTE:
            fprintf(f, "  .byte %d\n", p->imm);
            break;
        case IR3_ALIGN:
            fprintf(f, "  .align\n");
            break;
        case IR3_COMMENT:
            fprintf(f, "  ; %s\n", p->sym ? p->sym : "");
            break;
        case IR3_PUTCHAR:
            fprintf(f, "  putchar\n");
            break;
        }
    }
}

/* ----------------------------------------------------------------
 * free_ir3 — release all nodes in an IR3Inst list.
 * ---------------------------------------------------------------- */
void free_ir3(IR3Inst *head)
{
    while (head) {
        IR3Inst *next = head->next;
        free(head);
        head = next;
    }
}
