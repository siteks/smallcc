// EXPECT_R0: 0
// CFLAGS: -Oparam=inline_cf_nodes=80
// SIM_ARGS: -maxsteps 4000000
// FILES: ../../../bench/coremark/coremark_single.c
// CoreMark with more inlining makes main large enough that a fused cbeq's
// reach estimate fell 12 bytes short once later loop rotations grew the code
// between it and its target (CPU5: displacement 2048, reach 2047; the
// assembler rejected it). Emission now checks every PC-relative branch in
// its final dry run and bans the fusions that do not reach.
