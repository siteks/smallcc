// EXPECT_R0: 1
int main() { struct{int a;}b; b.a=1; return b.a; }
