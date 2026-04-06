// EXPECT_R0: 45
// 9 locals in a non-leaf function; MAX_PROMO_NONLEAF=8 so the 9th (k) uses the
// memory path (IR3_LEA + IR3_LOAD/STORE) instead of a pure SSA vreg.
int nop(int x) { return x; }
int main(void) {
    int a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, k=9;
    nop(0);
    return a + b + c + d + e + f + g + h + k;
}
