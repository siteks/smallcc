// EXPECT_R0: 28
// Chained inlining: f(g(x)) where both f and g inline
int dbl(int x) { return x + x; }
int quad(int x) { return dbl(dbl(x)); }

int main(void) {
    return quad(7);
}
