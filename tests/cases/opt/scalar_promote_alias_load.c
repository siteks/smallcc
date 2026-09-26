// EXPECT_R0: 15
// XFAIL: issue 0009 (opt_scalar_promote keeps *p in a register while g is read from memory; always-on pass)
int g;
int main(void) {
    int *p = &g, i, s = 0;
    g = 0;
    for (i = 0; i < 5; i++) { *p += 1; s += g; }
    return s;
}
