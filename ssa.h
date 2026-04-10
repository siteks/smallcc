#ifndef SSA_H
#define SSA_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "sx.h"   // ValType, CallDesc
#include "smallcc.h"  // Symbol

// ============================================================
// SSA IR — Regime 2 data structures
// ============================================================

// Instruction kinds (prefix IK_ to avoid collision with stack-IR IR_ names)
typedef enum {
    // Values
    IK_CONST,   // dst = iconst
    IK_COPY,    // dst = ops[0]
    IK_PHI,     // dst = phi(ops...)
    IK_PARAM,   // dst = param[param_idx]  (entry block only)
    // Integer ALU
    IK_ADD, IK_SUB, IK_MUL,
    IK_DIV, IK_UDIV,
    IK_MOD, IK_UMOD,
    IK_SHL, IK_SHR, IK_USHR,
    IK_AND, IK_OR, IK_XOR,
    IK_NEG, IK_NOT,
    // Integer compare (0 or 1)
    IK_LT, IK_ULT,
    IK_LE, IK_ULE,
    IK_EQ, IK_NE,
    // Float ALU
    IK_FADD, IK_FSUB, IK_FMUL, IK_FDIV,
    // Float compare
    IK_FLT, IK_FLE, IK_FEQ, IK_FNE,
    // Conversions
    IK_ITOF,
    IK_FTOI,
    IK_SEXT8,
    IK_SEXT16,
    IK_ZEXT,
    IK_TRUNC,
    // Memory
    IK_LOAD,    // dst = mem[ops[0] + imm], size bytes
    IK_STORE,   // mem[ops[0] + imm] = ops[1], size bytes
    IK_ADDR,    // dst = &local_slot[imm]  (frame-relative)
    IK_GADDR,   // dst = &global "fname"
    IK_MEMCPY,  // memcpy(ops[0], ops[1], imm bytes)
    // Calls
    IK_CALL,    // dst = call fname(ops...)
    IK_ICALL,   // dst = *ops[0](ops[1]...)
    IK_PUTCHAR, // putchar(ops[0])
    // Control flow (terminators)
    IK_BR,      // if ops[0] goto target else target2
    IK_JMP,     // goto target
    IK_RET,     // return ops[0]  (or void: nops==0)
} InstKind;

typedef enum { VAL_CONST, VAL_INST, VAL_UNDEF } ValKind;

typedef struct Value Value;
typedef struct Inst  Inst;
typedef struct Block Block;
typedef struct Function Function;

struct Value {
    ValKind  kind;
    int      id;            // unique within Function
    int      iconst;        // VAL_CONST: the constant value
    Inst    *def;           // VAL_INST: defining instruction
    Value   *alias;         // trivial-phi forwarding
    ValType  vtype;
    int      phys_reg;      // -1 until IRC assigns
    int      spill_slot;    // -1 unless spilled
    int      use_count;
};

struct Inst {
    InstKind  kind;
    Value    *dst;          // NULL if no result
    Value   **ops;          // operands (dynamic array)
    int       nops;
    int       imm;          // load/store offset; IK_CONST value
    int       size;         // load/store/memcpy bytes
    int       param_idx;    // IK_PARAM
    Block    *target;       // IK_BR true / IK_JMP
    Block    *target2;      // IK_BR false
    char     *fname;        // IK_CALL / IK_GADDR
    char     *label;        // IK_JMP to named label (goto)
    CallDesc *calldesc;
    Inst     *prev, *next;
    Block    *block;
    int       is_dead;
    int       line;         // source line (0 = unknown); used by -ann emission
};

// Braun variable maps (valid only during SSA construction)
#define BRAUN_MAP_MAX 256

struct Block {
    int     id;
    char   *label;          // NULL unless from a named label
    Inst   *head, *tail;
    Block **preds;  int npreds;
    Block **succs;  int nsuccs;
    int     sealed;         // all predecessors known
    int     filled;         // terminator emitted

    // Braun maps (Symbol* keys for O(1) pointer-equality lookup)
    Symbol *def_syms [BRAUN_MAP_MAX];
    Value  *def_vals [BRAUN_MAP_MAX];
    int     ndef;
    Inst   *iphi_insts[BRAUN_MAP_MAX];
    Symbol *iphi_syms[BRAUN_MAP_MAX];
    int     niphi;

    // Dominator analysis
    int     loop_depth;
    int     rpo_index;
    Block  *idom;
    Block **dom_children; int ndom_children;
    int     dom_pre, dom_post;

    // Liveness
    int      nwords;         // size of bitvector (nvalues/32 + 1)
    uint32_t *live_in;
    uint32_t *live_out;
};

struct Function {
    char    *name;
    Block  **blocks;    int nblocks;   int blk_cap;
    Value  **values;    int nvalues;   int val_cap;
    Value  **params;    int nparams;   int param_cap;
    int      next_val_id;
    int      next_blk_id;
    int      frame_size;
    bool     is_variadic;
};

// ============================================================
// Constructors
// ============================================================

Function *new_function(const char *name);
Block    *new_block(Function *f);
Value    *new_value(Function *f, ValKind kind, ValType vt);
Value    *new_const(Function *f, int ival, ValType vt);
Inst     *new_inst(Function *f, Block *b, InstKind kind, Value *dst);
void      inst_append(Block *b, Inst *inst);
void      inst_add_op(Inst *inst, Value *v);
void      block_add_succ(Block *from, Block *to);
void      block_add_pred(Block *to, Block *from);

// IR printer
void print_function(Function *f, FILE *out);
void print_inst(Inst *inst, FILE *out);

// Value resolution (follow alias chain)
static inline Value *val_resolve(Value *v) {
    while (v && v->alias) v = v->alias;
    return v;
}

// Instruction insertion
void inst_insert_before(Inst *next, Inst *new_inst);

// ValType size in bytes
int vtype_size(ValType vt);

#endif // SSA_H
