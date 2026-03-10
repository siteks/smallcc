// EXPECT_R0: 1
int main() { int *p = 0; int x = (1 || (*p = 99)); return x; }
