// EXPECT_R0: 0
// An unsigned char/short held in a register wraps under += and ++ (it used
// to keep the out-of-range value, since widening it is a plain copy).
int main(void) {
    unsigned char c = 250, d = 250, k;
    unsigned short w = 65535;
    int n = 0;
    c += 10;
    d++; d++; d++; d++; d++; d++;
    w += 2;
    if (c != 4 || d != 0 || w != 1) return 1;
    c = 3; c -= 5;
    if (c != 254) return 2;
    c = 128; c <<= 1;
    if (c != 0) return 3;
    for (k = 0; k < 8; k++) n += k;            /* bounded: the wrap is dropped */
    if (n != 28) return 4;
    c = 0xf0; c |= 0x0f; c ^= 0x3; c >>= 4;    /* cannot leave the range */
    if (c != 15) return 5;
    return 0;
}
