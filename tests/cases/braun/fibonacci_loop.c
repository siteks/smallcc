// EXPECT_R0: 55
// Two-variable phi cycle at loop header (a, b both live across back-edge)
int main(void) {
    int a = 0;
    int b = 1;
    int i;
    for (i = 0; i < 10; i++) {
        int t = a + b;
        a = b;
        b = t;
    }
    return a;
}
