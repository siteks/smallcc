// EXPECT_R0: 385
// Down-count conversion + dbnz fusion: a counted loop whose IV exists only
// for trip counting becomes c = phi(bound, c-1) exiting at zero, and the
// rotated latch (dec + jnz on the same register) emits the F3d dbnz
// instruction. Sum of squares 1..10 = 385 checks trip-count correctness;
// the n=0 case checks the zero-trip guard.
unsigned sq[10];
unsigned sum(unsigned *p, unsigned n) {
    unsigned i, s = 0;
    for (i = 0; i < n; i++) { s += *p * *p; p++; }
    return s;
}
int main(void) {
    unsigned i;
    for (i = 0; i < 10; i++) sq[i] = i + 1;
    if (sum(sq, 0) != 0) return -1;      /* zero-trip must not enter */
    return (int)sum(sq, 10);
}
