// EXPECT_R0: 6
// XFAIL: issue 0011 (zero-fill stores 2 bytes at every even offset: misaligned for a 3-byte, 1-aligned struct, and one byte past it)
struct S { char a, b, c; };
int main(void) { struct S s = {1, 2, 3}; return s.a + s.b + s.c; }
