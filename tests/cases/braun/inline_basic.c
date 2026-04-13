// EXPECT_R0: 34
// Basic inlining: single-return-expr leaf functions
int square(int x) { return x * x; }
int dbl(int x) { return x + x; }

int main(void) {
    return square(5) + dbl(3) + square(1) + dbl(1);
}
