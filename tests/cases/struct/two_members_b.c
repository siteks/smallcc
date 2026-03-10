// EXPECT_R0: 3
int main() { struct {int a; int b;}c; c.a=2; c.b=3; return c.b; }
