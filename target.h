/*
 * target.h — what the back end needs to know about the machine it emits for.
 *
 * One Target per ISA; g_target is the one selected (CPU4 until -arch picks
 * another). Register sets are masks over r0..r{nregs-1}. Everything here is
 * ABI or register-file shape; encodings (immediate ranges, instruction
 * sizes) come from the ISA definition's generated tables.
 */
#ifndef TARGET_H
#define TARGET_H

#include <stdint.h>

typedef uint32_t regmask_t;          // bit r = register r
#define MAX_GPR 32

struct IsaInstr_s;

/* The optimiser's register-pressure budgets. Per target (they depend on the
 * register file and the cost of callee-saved registers), and overridable
 * with -Oparam=NAME=VALUE for tuning sweeps. Reserves are subtracted from
 * the register count. */
typedef struct Tune {
    int lc_reserve;        // LICM consts: budget K - this for a small loop body
    int lc_reserve_large;  //   ... K - this when the body is larger than lc_large_body
    int lc_large_body;     //   instructions
    int lc_cap_reserve;    //   hard cap K - this, also for the loop-bound constant
    int lc_max_hoist;      //   constants hoisted per loop
    int lc_min_uses;       //   hoist a constant used more than this many times
    int licm_reserve;      // general LICM: K - this - live-ins
    int licm_max;          //   hoists per loop
    int licm_dense_hi;     //   more loop-defined values than this: at most 1 hoist
    int licm_dense_lo;     //   more than this: at most 2
    int lsr_reserve;       // LSR: K - this - live-ins reductions
} Tune;
extern Tune g_tune;        // the target's, then -Oparam overrides
int tune_set(const char *name_eq_value);   // 0 if the name is unknown
void tune_list(void);                      // names and values to stderr

typedef struct Target {
    const char *name;
    const void *instrs;              // the ISA's generated IsaInstr table (<arch>/isa_table_c.h)
    const void *pseudos;             // ... and its IsaPseudo table
    int         nregs;               // allocatable general registers (IRC's K)
    regmask_t   caller_saved;        // a call may clobber these (scratch pool)
    regmask_t   callee_saved;        // a function that uses these saves them
    int         ret_reg;             // return value; indirect-call target register
    int         n_arg_regs;          // leading arguments passed in registers
    int         arg_regs[4];         // ... in these, in order
    Tune        tune;                // default optimiser budgets
} Target;

extern const Target *g_target;
extern const Target  target_cpu4, target_cpu5;
const Target *target_find(const char *name);   // by -arch name, or NULL
const char   *target_names(void);              // "cpu4, cpu5" for messages

static inline regmask_t target_all_regs(void) {
    return g_target->nregs >= 32 ? 0xffffffffu : ((1u << g_target->nregs) - 1);
}
static inline regmask_t target_arg_mask(void) {
    regmask_t m = 0;
    for (int i = 0; i < g_target->n_arg_regs; i++) m |= 1u << g_target->arg_regs[i];
    return m;
}
static inline int reg_in(regmask_t m, int r) { return r >= 0 && r < 32 && (m >> r) & 1; }

/* Encodings, from the ISA definition's generated table: one query per
 * mnemonic the back end is about to emit, so no range or size is written
 * into the compiler. Pseudo-ops resolve to their real instruction. */
int  isa_has(const char *mnem);                    // the target has this instruction (or pseudo-op)
int  isa_real(const char *mnem);                   // ... as a real instruction, not a pseudo-op
int  isa_bytes(const char *mnem);                  // its size in bytes (0: unknown)
// Source-unit range of the k-th immediate operand (0-based, registers not
// counted): what the assembler accepts. Returns 0 if there is none.
int  isa_imm_range(const char *mnem, int k, long *lo, long *hi);
// Whether v is accepted as that immediate (range, and a multiple of the
// scale for a byte offset). False for an unknown mnemonic.
int  isa_imm_fits(const char *mnem, int k, long v);

#endif
