// EXPECT_R0: 1
// XFAIL: issue 0006 (the ternary takes the then-branch type; the else branch is not converted)
int main(void) {
    int c = 0;
    float f = c ? 1 : 2.5f;
    return f == 2.5f;
}
