// EXPECT_R0: 1
int f(int x) { return x; } int main() { enum{A=0,B=1}; return f(B); }
