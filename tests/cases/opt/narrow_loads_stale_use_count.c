// EXPECT_R0: 20
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
