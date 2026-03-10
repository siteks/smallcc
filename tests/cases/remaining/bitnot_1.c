// EXPECT_R0: 65534
int main() { int a=1; return ~a & 0xffff; }
