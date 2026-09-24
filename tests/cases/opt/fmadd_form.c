// EXPECT_R0: 1078984704
// CFLAGS: -Opass=fmadd
// fmadd/fmsub formation: dot-product shapes give the same bits as the
// separate fmul+fadd (the ISA defines fmadd as that sequence).
// 0.25 + 1.5*2.0 = 3.25 = 0x40500000; then (3.25 - 0.5*2.5) = 2.0, and
// the two-address fallback paths (accumulator not in rd) must still work.
static int bits(float f) { union { float f; int i; } u; u.f = f; return u.i; }
static float dot3(float *a, float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
int main(void) {
    float acc = 0.25f, a = 1.5f, b = 2.0f;
    float v1[3], v2[3];
    float r = acc + a * b;               /* 3.25 */
    float s = r - 0.5f * 2.5f;           /* 2.0 */
    v1[0] = 1.0f; v1[1] = 2.0f; v1[2] = 3.0f;
    v2[0] = 4.0f; v2[1] = 0.5f; v2[2] = 1.0f;
    if (dot3(v1, v2) != 8.0f) return 1;  /* 4 + 1 + 3 */
    if (s != 2.0f) return 2;
    if (acc + a * b != r) return 3;      /* acc still intact after the two-address op */
    return bits(r);
}
