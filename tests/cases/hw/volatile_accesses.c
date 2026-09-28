// EXPECT_R0: 0
// SIM_ARGS: -maxsteps 200000
// volatile accesses all happen, in order (docs/issues/0007). The cycle
// counter at 0xFF00 advances once per instruction, so a deleted, merged or
// hoisted read shows up as a smaller difference or a loop that never ends.
struct regs { unsigned cycles; };
int main(void) {
    volatile unsigned *t = (volatile unsigned *)0xff00;
    volatile struct regs *r = (volatile struct regs *)0xff00;
    unsigned a, b, t0;
    volatile int k = 0;
    int i;
    a = *t;
    *t; *t; *t;                              /* unused reads still happen */
    b = *t;
    if (b - a < 4) return 1;
    t0 = r->cycles;                          /* a member of a volatile struct */
    while (r->cycles - t0 < 50u) { }
    for (i = 0; i < 5; i++) k++;             /* a volatile local lives in memory */
    if (k != 5) return 2;
    return 0;
}
