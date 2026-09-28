// EXPECT_COMPILE_FAIL
int f(int a) { int a; return 1; }
int main(void) { return f(0); }
