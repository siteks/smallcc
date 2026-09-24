// EXPECT_R0: 3210
// Signed compares against zero (int and float) fuse into the F3d
// bltz/bgez/bgtz/blez branches; both operand orders and both branch senses.
static int cls(int v)   { int r = 0; if (v < 0) r += 1; if (v <= 0) r += 10; if (v > 0) r += 100; if (v >= 0) r += 1000; return r; }
static int clsf(float v){ int r = 0; if (v < 0.0f) r += 1; if (0.0f >= v) r += 10; if (0.0f < v) r += 100; if (v >= 0.0f) r += 1000; return r; }
static int loopdown(int n) { int s = 0; while (n >= 0) { s = s + n; n = n - 1; } return s; }
int main(void) {
    int a = cls(-7) + cls(0) + cls(9);          /* 11 + 1010 + 1100 = 2121 */
    int b = clsf(-2.5f) + clsf(0.0f) + clsf(4.0f); /* 11 + 1010 + 1100 = 2121 */
    int c = loopdown(4);                          /* 10 */
    return a - b + c + 3200;                      /* 0 + 10 + 3200 */
}
