// H1 variant: AND with small negative constant on unsigned long.
// 0xdeadbeef & 0xffffffc0 = 0xdeadbec0.
// EXPECT_R0: 1
int main(void) {
    unsigned long x = 0xdeadbeefUL;
    unsigned long y = x & (unsigned long)-64;
    return (y == 0xdeadbec0UL);
}
