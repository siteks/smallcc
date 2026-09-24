// EXPECT_R0: 63
// Scalar static locals whose initializer is not a bare int literal
// (insert_coercions wraps it in an ND_CAST for short/char/unsigned/long
// targets; negative constants are a unary minus over a literal) used to
// be misclassified as zero-initialized BSS, dropping the initializer
// silently. Found via docs/issues/0001. Each surviving term sets a bit.
int main(void) {
    static short sh = 0x1234;
    static char c = 'x';
    static unsigned int u = 5;
    static int neg = -1;
    static long l = 9;
    static int i = 3;
    return (sh == 0x1234) | ((c == 'x') << 1) | ((u == 5) << 2) |
           ((neg == -1) << 3) | ((l == 9) << 4) | ((i == 3) << 5);
}
