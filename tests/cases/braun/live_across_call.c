// EXPECT_R0: 32
// Values live across a call must land in callee-saved r4-r7, not caller-saved r0-r3
int id(int x) { return x; }
int main(void) {
    int a = 3;
    int b = 4;
    int p = a * a;
    int q = b * b;
    int r = id(a + b);
    return p + q + r;
}
