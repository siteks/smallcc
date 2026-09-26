// EXPECT_R0: 1
// XFAIL: issue 0004 (a top-level &x initialiser is zero-filled; only inside an init list is it handled)
int x;
int *p = &x;
int main(void) { return p == &x; }
