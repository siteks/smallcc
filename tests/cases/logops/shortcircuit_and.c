// EXPECT_R0: 1
int main() { int *p = 0; int x = (p != 0 && *p > 0); return !x; }
