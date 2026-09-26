// EXPECT_R0: 20
// XFAIL: issue 0010 (opt_narrow_loads narrows the load to one byte on use_count == 1 while a second use survives via a trivial-phi alias)
unsigned v = 0x13000005;
int main(void) {
    unsigned *p = &v;
    unsigned x = *p;
    unsigned i = 0;
    int n = 0;
    while (x & 0xff) {
        if (i < x) n++; else break;
        i += 0x1000000;
    }
    return n;
}
