#include <stdlib.h>
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

void inst_insert_after(Inst *prev, Inst *ins) {
    Block *b   = prev->block;
    ins->block = b;
    ins->prev  = prev;
    ins->next  = prev->next;
    if (prev->next) prev->next->prev = ins;
    else            b->tail = ins;
    prev->next = ins;
}

int vtype_size(ValType vt) {
    switch (vt) {
    case VT_I8:  case VT_U8:  return 1;
    case VT_I16: case VT_U16: return 2;
    case VT_I32: case VT_U32: case VT_F32: return 4;
    case VT_PTR: return 4;     // ILP32: pointers are 4 bytes
    default: return 4;
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

// Remove one occurrence of pred from b's predecessor list (swap-with-last).
void block_remove_pred(Block *b, Block *pred) {
    for (int k = 0; k < b->npreds; k++) {
        if (b->preds[k] == pred) {
            b->preds[k] = b->preds[--b->npreds];
            break;
        }
    }
}

// Remove one occurrence of succ from b's successor list (swap-with-last).
void block_remove_succ(Block *b, Block *succ) {
    for (int k = 0; k < b->nsuccs; k++) {
        if (b->succs[k] == succ) {
            b->succs[k] = b->succs[--b->nsuccs];
            break;
        }
    }
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
        "itof","frecip","frsqrt","ftoi","sext8","sext16","zext","trunc",
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
    case IK_ADDR:
        fprintf(out, " bp%+d", inst->imm);
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
        if (inst->fname)                             fprintf(out, "%s", inst->fname);
        else if (inst->nops >= 1 && inst->ops[0])    print_val(inst->ops[0], out);
        else                                         fprintf(out, "bp");
        if (inst->imm) fprintf(out, "+%d", inst->imm);
        fprintf(out, "]:%d", inst->size);
        if (inst->is_volatile) fprintf(out, " volatile");
        break;
    case IK_STORE:
        fprintf(out, " [");
        if (inst->nops >= 2) { print_val(inst->ops[0], out); if (inst->imm) fprintf(out, "+%d", inst->imm); }
        else if (inst->fname) fprintf(out, "%s+%d", inst->fname, inst->imm);
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


// ============================================================
// type_to_valtype
// ============================================================

ValType type_to_valtype(Type *t) {
    if (!t) return VT_VOID;
    switch (t->base) {
    case TB_VOID:                       return VT_VOID;
    case TB_CHAR:                       return VT_I8;
    case TB_UCHAR:                      return VT_U8;
    case TB_SHORT:                      return VT_I16;
    case TB_USHORT:                     return VT_U16;
    case TB_INT:                        return VT_I32;   // ILP32: int is 4 bytes
    case TB_UINT:                       return VT_U32;   // ILP32: unsigned int is 4 bytes
    case TB_LONG:                       return VT_I32;
    case TB_ULONG:                      return VT_U32;
    case TB_FLOAT: case TB_DOUBLE:      return VT_F32;
    case TB_POINTER: case TB_FUNCTION:  return VT_PTR;
    case TB_ARRAY:                      return VT_PTR;   // array decays to pointer
    case TB_STRUCT:                     return VT_I32;   // struct size varies; pass-around is pointer-sized
    case TB_ENUM:                       return VT_I32;   // enum follows int
    default:                            return VT_VOID;
    }
}

// ---- source locations (ssa.h) ------------------------------------------
typedef struct { const char *file; int line, parent; } SrcLocEnt;
static SrcLocEnt *g_locs;
static int g_nlocs = 1, g_loc_cap;          // id 0 = unknown
static int *g_loc_hash;
static int g_loc_hcap;

static unsigned loc_hash(const char *file, int line, int parent) {
    unsigned h = 2166136261u;
    for (const char *c = file ? file : ""; *c; c++) { h ^= (unsigned char)*c; h *= 16777619u; }
    h ^= (unsigned)line * 2654435761u; h ^= (unsigned)parent * 40503u;
    return h;
}

int src_loc(const char *file, int line, int parent) {
    if (line <= 0) return 0;
    if (!file) file = "?";
    if (2 * g_nlocs >= g_loc_hcap) {                      // grow and rehash
        int nc = g_loc_hcap ? 2 * g_loc_hcap : 1024;
        int *nh = calloc(nc, sizeof(int));
        for (int i = 1; i < g_nlocs; i++) {
            unsigned k = loc_hash(g_locs[i].file, g_locs[i].line, g_locs[i].parent) & (nc - 1);
            while (nh[k]) k = (k + 1) & (nc - 1);
            nh[k] = i;
        }
        free(g_loc_hash); g_loc_hash = nh; g_loc_hcap = nc;
    }
    unsigned k = loc_hash(file, line, parent) & (g_loc_hcap - 1);
    for (; g_loc_hash[k]; k = (k + 1) & (g_loc_hcap - 1)) {
        SrcLocEnt *e = &g_locs[g_loc_hash[k]];
        if (e->line == line && e->parent == parent && !strcmp(e->file, file)) return g_loc_hash[k];
    }
    if (g_nlocs >= g_loc_cap) {
        g_loc_cap = g_loc_cap ? 2 * g_loc_cap : 1024;
        g_locs = realloc(g_locs, g_loc_cap * sizeof *g_locs);
    }
    g_locs[g_nlocs] = (SrcLocEnt){ file, line, parent };
    g_loc_hash[k] = g_nlocs;
    return g_nlocs++;
}

const char *src_loc_file(int id)  { return id > 0 && id < g_nlocs ? g_locs[id].file : NULL; }
int         src_loc_line(int id)  { return id > 0 && id < g_nlocs ? g_locs[id].line : 0; }
int         src_loc_parent(int id) { return id > 0 && id < g_nlocs ? g_locs[id].parent : 0; }
