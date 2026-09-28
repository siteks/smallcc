// EXPECT_R0: 6
struct S { char a, b, c; };
int main(void) { struct S s = {1, 2, 3}; return s.a + s.b + s.c; }
