// EXPECT_R0: 255
// XFAIL: issue 0008 (opt_known_bits assumes a 1-byte load is zero-extended; the load is llbx)
char g = -1;
int main(void) { char *p = &g; return *p & 0xff; }
