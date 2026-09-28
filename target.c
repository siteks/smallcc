/* target.c — the Target descriptions (target.h). */
#include "target.h"

const Target target_cpu4 = {
    .name         = "cpu4",
    .nregs        = 8,
    .caller_saved = 0x0f,            // r0-r3
    .callee_saved = 0xf0,            // r4-r7
    .ret_reg      = 0,
    .n_arg_regs   = 3,
    .arg_regs     = {1, 2, 3},
};

const Target *g_target = &target_cpu4;
