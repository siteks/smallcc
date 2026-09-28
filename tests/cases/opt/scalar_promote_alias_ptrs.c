// EXPECT_R0: 0
// Scalar promotion must not keep *acc in a register while the loop reads
// the same memory through another pointer of the same type, or bytewise
// through a char pointer (docs/issues/0009).
int buf[6];
int sum_into(int *acc, int *src, int n) {
    int i;
    *acc = 1;                        /* the store that seeds a promotion */
    for (i = 0; i < n; i++) *acc += src[i];
    return *acc;
}
int bytes_seen(int *acc, unsigned char *b, int n) {
    int i, t = 0;
    *acc = 0;
    for (i = 0; i < n; i++) { *acc += 1; t += b[0]; }
    return t;
}
int main(void) {
    int i, v = 0;
    for (i = 0; i < 6; i++) buf[i] = 1;
    /* acc is buf[2]: src[2] reads the running total */
    if (sum_into(&buf[2], buf, 4) != 7) return 1;          /* 1+1, +1, +3 (itself), +1 */
    /* each iteration reads the low byte of the incremented int */
    if (bytes_seen(&v, (unsigned char *)&v, 3) != 1 + 2 + 3) return 2;
    return 0;
}
