// EXPECT_R0: 36
// 8 vregs live across a call: 4 land in r4-r7 (callee-saves), remaining 4 must spill.
// Tests the combined path: frame expanded for callee-saves, spill slots laid out correctly.
int nop(void) { return 0; }
int main(void) {
    int a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8;
    nop();
    return a + b + c + d + e + f + g + h;
}
