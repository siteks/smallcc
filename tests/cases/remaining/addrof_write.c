// EXPECT_R0: 7
int main() { int a=5; int *p=&a; *p=7; return a; }
