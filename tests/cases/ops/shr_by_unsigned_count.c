// EXPECT_R0: 1
// XFAIL: issue 0006 (shifts go through usual arithmetic conversion, so -8 >> 1u is logical; the R2C/P7 folds also shift logically)
int main(void) { int x = -8; return (x >> 1u) == -4 && (-8 >> 1u) == -4; }
