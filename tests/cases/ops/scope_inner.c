// EXPECT_R0: 2
int main() { int a=1; {int a=2; return a;} }
