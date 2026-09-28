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

typedef struct Target {
    const char *name;
    int         nregs;               // allocatable general registers (IRC's K)
    regmask_t   caller_saved;        // a call may clobber these (scratch pool)
    regmask_t   callee_saved;        // a function that uses these saves them
    int         ret_reg;             // return value; indirect-call target register
    int         n_arg_regs;          // leading arguments passed in registers
    int         arg_regs[4];         // ... in these, in order
} Target;

extern const Target *g_target;
extern const Target  target_cpu4;

static inline regmask_t target_all_regs(void) {
    return g_target->nregs >= 32 ? 0xffffffffu : ((1u << g_target->nregs) - 1);
}
static inline regmask_t target_arg_mask(void) {
    regmask_t m = 0;
    for (int i = 0; i < g_target->n_arg_regs; i++) m |= 1u << g_target->arg_regs[i];
    return m;
}
static inline int reg_in(regmask_t m, int r) { return r >= 0 && r < 32 && (m >> r) & 1; }

#endif
