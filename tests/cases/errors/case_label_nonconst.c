// EXPECT_COMPILE_FAIL
// A case label must be an integer constant expression.
int main(void) {
    int x = 1, y = 1;
    switch (x) { case y: return 1; }
    return 0;
}
