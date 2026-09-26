// EXPECT_R0: 7
// XFAIL: issue 0004 (an enum constant as a global initialiser is zero-filled)
enum { E = 7 };
int g = E;
int main(void) { return g; }
