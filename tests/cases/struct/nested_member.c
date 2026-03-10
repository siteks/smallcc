// EXPECT_R0: 2
struct s1 {int a; int b;}; int main() { struct {struct s1 c; int d;}e; e.c.b=2; return e.c.b; }
