#include <stdio.h>
#include <string.h>
#include "ssa.h"
#include "smallcc.h"

// ============================================================
// Constructors
// ============================================================

Function *new_function(const char *name) {
    Function *f = arena_alloc(sizeof(Function));
    f->name     = arena_strdup(name);
    return f;
}

Block *new_block(Function *f) {
    Block *b = arena_alloc(sizeof(Block));
    b->id     = f->next_blk_id++;
    // Grow blocks array
    if (f->nblocks >= f->blk_cap) {
        int nc = f->blk_cap ? f->blk_cap * 2 : 8;
        Block **nb = arena_alloc(nc * sizeof(Block *));
        memcpy(nb, f->blocks, f->nblocks * sizeof(Block *));
        f->blocks  = nb;
        f->blk_cap = nc;
    }
    f->blocks[f->nblocks++] = b;
    return b;
}

Value *new_value(Function *f, ValKind kind, ValType vt) {
    Value *v = arena_alloc(sizeof(Value));
    v->kind      = kind;
    v->id        = f->next_val_id++;
    v->vtype     = vt;
    v->phys_reg  = -1;
    v->spill_slot = -1;
    // Grow values array
    if (f->nvalues >= f->val_cap) {
        int nc = f->val_cap ? f->val_cap * 2 : 16;
        Value **nv = arena_alloc(nc * sizeof(Value *));
        memcpy(nv, f->values, f->nvalues * sizeof(Value *));
        f->values  = nv;
        f->val_cap = nc;
    }
    f->values[f->nvalues++] = v;
    return v;
}

Value *new_const(Function *f, int ival, ValType vt) {
    Value *v = new_value(f, VAL_CONST, vt);
    v->iconst = ival;
    return v;
}

Inst *new_inst(Function *f, Block *b, InstKind kind, Value *dst) {
    (void)f;
    Inst *inst   = arena_alloc(sizeof(Inst));
    inst->kind   = kind;
    inst->dst    = dst;
    inst->block  = b;
    if (dst) dst->def = inst;
    return inst;
}

void inst_add_op(Inst *inst, Value *v) {
    // Dynamic array of operands
    int n   = inst->nops;
    Value **ops = arena_alloc((n + 1) * sizeof(Value *));
    memcpy(ops, inst->ops, n * sizeof(Value *));
    ops[n] = v;
    inst->ops  = ops;
    inst->nops = n + 1;
    if (v) v->use_count++;
}

void inst_insert_before(Inst *next, Inst *ins) {
    Block *b   = next->block;
    ins->block = b;
    ins->next  = next;
    ins->prev  = next->prev;
    if (next->prev) next->prev->next = ins;
    else            b->head = ins;
    next->prev = ins;
}

int vtype_size(ValType vt) {
    switch (vt) {
    case VT_I8:  case VT_U8:  return 1;
    case VT_I16: case VT_U16: return 2;
    case VT_I32: case VT_U32: case VT_F32: return 4;
    case VT_PTR: return 2;
    default: return 2;
    }
}

void inst_append(Block *b, Inst *inst) {
    inst->block = b;
    inst->prev  = b->tail;
    inst->next  = NULL;
    if (b->tail) b->tail->next = inst;
    else         b->head = inst;
    b->tail = inst;
}

void block_add_succ(Block *from, Block *to) {
    Block **ns = arena_alloc((from->nsuccs + 1) * sizeof(Block *));
    memcpy(ns, from->succs, from->nsuccs * sizeof(Block *));
    from->succs = ns;
    from->succs[from->nsuccs++] = to;
}

void block_add_pred(Block *to, Block *from) {
    Block **np = arena_alloc((to->npreds + 1) * sizeof(Block *));
    memcpy(np, to->preds, to->npreds * sizeof(Block *));
    to->preds = np;
    to->preds[to->npreds++] = from;
}

// ============================================================
// IR Printer
// ============================================================

static const char *instname(InstKind k) {
    static const char *names[] = {
        "const","copy","phi","param",
        "add","sub","mul","div","udiv","mod","umod",
        "shl","shr","ushr","and","or","xor","neg","not",
        "lt","ult","le","ule","eq","ne",
        "fadd","fsub","fmul","fdiv","flt","fle","feq","fne",
        "itof","ftoi","sext8","sext16","zext","trunc",
        "load","store","addr","gaddr","memcpy",
        "call","icall","putchar",
        "br","jmp","ret","switch",
    };
    if (k >= 0 && k < (int)(sizeof(names)/sizeof(names[0])))
        return names[k];
    return "???";
}

static const char *vtname(ValType vt) {
    switch (vt) {
    case VT_VOID: return "void";
    case VT_I8:   return "i8";
    case VT_I16:  return "i16";
    case VT_I32:  return "i32";
    case VT_U8:   return "u8";
    case VT_U16:  return "u16";
    case VT_U32:  return "u32";
    case VT_PTR:  return "ptr";
    case VT_F32:  return "f32";
    default:      return "?";
    }
    return "?";
}

static void print_val(Value *v, FILE *out) {
    if (!v) { fprintf(out, "_"); return; }
    v = val_resolve(v);
    if (v->kind == VAL_CONST) { fprintf(out, "%d", v->iconst); return; }
    if (v->kind == VAL_UNDEF) { fprintf(out, "undef"); return; }
    if (v->phys_reg >= 0)
        fprintf(out, "r%d[v%d]", v->phys_reg, v->id);
    else
        fprintf(out, "v%d", v->id);
}

void print_inst(Inst *inst, FILE *out) {
    if (!inst) return;
    fprintf(out, "  ");
    if (inst->dst) {
        print_val(inst->dst, out);
        fprintf(out, ":%s = ", vtname(inst->dst->vtype));
    }
    fprintf(out, "%s", instname(inst->kind));

    switch (inst->kind) {
    case IK_CONST:
        fprintf(out, " %d", inst->imm);
        break;
    case IK_CALL:
        fprintf(out, " %s(", inst->fname ? inst->fname : "?");
        for (int i = 0; i < inst->nops; i++) {
            if (i) fprintf(out, ", ");
            print_val(inst->ops[i], out);
        }
        fprintf(out, ")");
        break;
    case IK_BR:
        fprintf(out, " ");
        print_val(inst->ops[0], out);
        fprintf(out, " ? B%d : B%d",
                inst->target  ? inst->target->id  : -1,
                inst->target2 ? inst->target2->id : -1);
        break;
    case IK_JMP:
        fprintf(out, " B%d", inst->target ? inst->target->id : -1);
        break;
    case IK_SWITCH:
        fprintf(out, " ");
        if (inst->nops >= 1) print_val(inst->ops[0], out);
        fprintf(out, " [");
        for (int i = 0; i < inst->switch_ncase; i++) {
            if (i) fprintf(out, ", ");
            fprintf(out, "%d:B%d", inst->switch_vals[i],
                    inst->switch_targets[i] ? inst->switch_targets[i]->id : -1);
        }
        fprintf(out, " default:B%d]",
                inst->switch_default ? inst->switch_default->id : -1);
        break;
    case IK_LOAD:
        fprintf(out, "[");
        if (inst->nops >= 1) print_val(inst->ops[0], out);
        else                 fprintf(out, "bp");
        if (inst->imm) fprintf(out, "+%d", inst->imm);
        fprintf(out, "]:%d", inst->size);
        break;
    case IK_STORE:
        fprintf(out, " [");
        if (inst->nops >= 2) { print_val(inst->ops[0], out); if (inst->imm) fprintf(out, "+%d", inst->imm); }
        else                  fprintf(out, "bp+%d", inst->imm);
        fprintf(out, "]:%d = ", inst->size);
        if (inst->nops >= 2) print_val(inst->ops[1], out);
        else if (inst->nops >= 1) print_val(inst->ops[0], out);
        break;
    default:
        for (int i = 0; i < inst->nops; i++) {
            fprintf(out, " ");
            print_val(inst->ops[i], out);
        }
        if (inst->fname) fprintf(out, " \"%s\"", inst->fname);
        break;
    }
    fprintf(out, "\n");
}

void print_function(Function *f, FILE *out) {
    fprintf(out, "func %s:\n", f->name);
    for (int i = 0; i < f->nblocks; i++) {
        Block *b = f->blocks[i];
        if (b->label)
            fprintf(out, "B%d(%s):\n", b->id, b->label);
        else
            fprintf(out, "B%d:\n", b->id);
        if (b->npreds) {
            fprintf(out, "  preds:");
            for (int j = 0; j < b->npreds; j++)
                fprintf(out, " B%d", b->preds[j]->id);
            fprintf(out, "\n");
        }
        for (Inst *inst = b->head; inst; inst = inst->next)
            print_inst(inst, out);
    }
    fprintf(out, "\n");
}
