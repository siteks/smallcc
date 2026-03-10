// EXPECT_R0: 2
struct a{int b;}; int main() { struct a c; c.b=2; return c.b; }
