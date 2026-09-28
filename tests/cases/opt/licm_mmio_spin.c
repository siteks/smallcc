// EXPECT_R0: 7
// SIM_ARGS: -maxsteps 200000
// The cycle counter at 0xFF00 advances once per instruction, so the loop
// terminates within a few hundred steps unless the load is hoisted.
int main(void) {
    volatile unsigned *t = (volatile unsigned *)0xFF00;
    unsigned t0 = *t;
    while (*t - t0 < 100u) { }
    return 7;
}
