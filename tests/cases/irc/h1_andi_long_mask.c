// H1: andi sign-extends imm7 to 32 bits; immw+and only loads 16-bit zero-extended constant.
// For a long value with non-zero upper 16 bits, andi -64 gives rs1 & 0xffffffc0
// while immw+and gives rs1 & 0x0000ffc0 — different results.
// EXPECT_R0: 1
long f(long x) { return x & (long)-64; }
int main(void) {
    // -1L = 0xffffffff; & -64 = & 0xffffffc0 = 0xffffffc0 = -64
    long r = f(-1L);
    return (r == -64L);
}
