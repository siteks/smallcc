// EXPECT_R0: 1051372160
// __builtin_frecip(3.0f) is the ROM seed 0x3eaaaa80 (0.33333206...).
static int bits(float f) { union { float f; int i; } u; u.f = f; return u.i; }
int main(void) {
    float x = 3.0f;
    return bits(__builtin_frecip(x));
}
