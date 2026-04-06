// H4: multi_def guard for phi-carried constant across branches.
// k is conditionally redefined on one branch; use in AND should not fold to andi
// if it's phi-connected.
// EXPECT_R0: 1
int f(int n, int flag) {
    int k = 0x3f;
    int s = 0;
    for (int i = 0; i < n; i++) {
        if (flag && i == 3) k = 0x3f;   /* same value, but phi-carried in SSA */
        s += (i & k);
    }
    return s;
}
int main(void) {
    /* sum of (i & 0x3f) for i=0..9 = 0+1+2+3+4+5+6+7+8+9 = 45 */
    return f(10, 0) == 45;
}
