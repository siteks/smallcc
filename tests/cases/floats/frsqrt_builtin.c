// EXPECT_R0: 1065345024
// __builtin_frsqrt(1.0f) is the ROM seed 0x3f7fe000 (0.99951171875): the
// table holds interval midpoints, so 1.0 is not exact until a Newton step.
// Checks the builtin, the seed definition and that the value round-trips
// through an int without conversion.
static int bits(float f) { union { float f; int i; } u; u.f = f; return u.i; }
int main(void) {
    float x = 1.0f;
    float y = __builtin_frsqrt(x);
    return bits(y);
}
