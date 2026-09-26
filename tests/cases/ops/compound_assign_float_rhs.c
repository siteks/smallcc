// EXPECT_R0: 3
// XFAIL: issue 0006 (compound assignment casts the RHS to the LHS type before the operation)
int main(void) { int i = 2; i *= 1.5f; return i; }
