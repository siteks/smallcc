// EXPECT_R0: 3
int main() { int a=0; if (1) a=1; if (0) a=2; else a=3; return a; }
