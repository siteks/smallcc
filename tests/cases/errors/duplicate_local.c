// EXPECT_COMPILE_FAIL
// XFAIL: issue 0005 (a local that redeclares a parameter in the same scope is accepted)
int f(int a) { int a; return 1; }
int main(void) { return f(0); }
