// EXPECT_R0: 3
// XFAIL: issue 0004 (lower.c only understands literal initialisers; 1 + 2 is zero-filled)
int g = 1 + 2;
int main(void) { return g; }
