// EXPECT_R0: 218
// CFLAGS: -O0
// At -O0 a store of a constant reaches emission unmaterialised. The store
// emitter used a fixed scratch register for it (r1 here), clobbering the
// live loop counter, so the loop never ended (found by tools/fuzz.py).
unsigned garr[8];
int main(void)
{
    unsigned v0 = 66, v1 = 86, v2 = 10;
    int i3;
    v2 &= v2;
    for (i3 = 0; i3 < 7; i3++)
        garr[((272 - (142 % ((v0) | 1u)))) & 7] = v2;
    return (int)((v0*7+v1*3+v2) & 0xff);
}
