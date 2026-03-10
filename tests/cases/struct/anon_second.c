// EXPECT_R0: 1
int main() { struct{int a;int b;}c; c.b=1; return c.b; }
