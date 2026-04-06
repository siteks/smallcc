// EXPECT_R0: 180
// 10 simultaneously-live vregs in leaf loop (8 params + sum + i); K=7 forces 3 spills
// Spill slots are accessed on every loop iteration
int loop_sum(int a, int b, int c, int d, int e, int f, int g, int h) {
    int sum = 0;
    int i;
    for (i = 0; i < 5; i++)
        sum = sum + a + b + c + d + e + f + g + h;
    return sum;
}
int main(void) {
    return loop_sum(1, 2, 3, 4, 5, 6, 7, 8);
}
