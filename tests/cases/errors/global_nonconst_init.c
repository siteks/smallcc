// EXPECT_COMPILE_FAIL
// A static initialiser that is not a constant expression is an error, not a zero.
int x = 3;
int g = x + 1;
int main(void) { return g; }
