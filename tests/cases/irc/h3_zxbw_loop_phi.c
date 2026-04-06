// H3: zxb/zxw fold interacts with loop-carried phi vregs.
// A loop where a phi-carried value is ANDed with 0xff each iteration.
// cumulative (acc+i) & 0xff for i=0..99 starting from acc=0, verified empirically = 86.
// EXPECT_R0: 86
int f(int n) {
    int acc = 0;
    for (int i = 0; i < n; i++) acc = (acc + i) & 0xff;
    return acc;
}
int main(void) { return f(100); }
