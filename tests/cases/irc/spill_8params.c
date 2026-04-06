// EXPECT_R0: 36
// Leaf fn with 8 params all live simultaneously; K=7 allocatable regs forces 1 spill
int add8(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}
int main(void) {
    return add8(1, 2, 3, 4, 5, 6, 7, 8);
}
