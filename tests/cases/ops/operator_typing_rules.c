// EXPECT_R0: 0
// Result types of shifts, pointer difference, ?: and compound assignment
// (docs/issues/0006).
struct T { int a, b, c; };                     /* 12 bytes: not a power of two */
struct T ts[5];
char cs[9];
int main(void) {
    int i = -4, n = 3, zero = 0;
    unsigned u = 1;
    unsigned char c = 200;
    short s = -32000;
    float f;

    /* shifts: the promoted left operand's type; the count's type is irrelevant */
    if ((i >> u) != -2) return 1;
    if ((-64 >> 3u) != -8) return 2;           /* folded at compile time */
    if (((unsigned)i >> 1) != 0x7ffffffe) return 3;
    if ((s >> 4) != -2000) return 4;

    /* pointer difference: an element count */
    if (&ts[4] - &ts[1] != 3) return 5;
    if (&cs[7] - cs != 7) return 6;
    if (&ts[n] - ts != n) return 7;

    /* ?: converts both branches to the common type */
    f = zero ? 1 : 0.75f;
    if (f != 0.75f) return 8;
    if ((zero ? u : -1) < 0) return 9;         /* unsigned: never negative */
    if ((n ? 'a' : 1000) != 'a') return 10;

    /* compound assignment computes in the common type */
    i = 7; i *= 1.5f;                          /* 10.5 -> 10 */
    if (i != 10) return 11;
    i = -4; i /= 2u;                           /* unsigned division */
    if (i != 0x7ffffffe) return 12;
    c /= -1;                                   /* 200 / -1 = -200 -> 56 */
    if (c != 56) return 13;
    c = 250; c += 10;                          /* wraps in the char */
    if (c != 4) return 14;
    s = 7; s %= -4;
    if (s != 3) return 15;
    return 0;
}
