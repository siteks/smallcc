// EXPECT_R0: 30
// Inlining with ternary expressions (max, abs)
int max(int a, int b) { return a > b ? a : b; }
int abs_val(int x) { return x < 0 ? -x : x; }

int main(void) {
    return max(10, 20) + abs_val(-10);
}
