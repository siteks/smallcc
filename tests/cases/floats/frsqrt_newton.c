// EXPECT_R0: 2000
// One Newton step on the frsqrt seed gives ~22 bits: for x in a range of
// magnitudes, y*y*x must be within 1e-5 of 1. Returns 2000 iff all pass.
static float rsq(float x) { float y = __builtin_frsqrt(x); return y * (1.5f - 0.5f * x * y * y); }
int main(void) {
    float xs[8]; int i, ok = 0;
    xs[0] = 1.0f; xs[1] = 2.0f; xs[2] = 0.25f; xs[3] = 3.7f; xs[4] = 1000.5f; xs[5] = 0.0012f; xs[6] = 123456.0f; xs[7] = 0.9999f;
    for (i = 0; i < 8; i++) {
        float y = rsq(xs[i]);
        float e = y * y * xs[i] - 1.0f;
        if (e < 0.0f) e = 0.0f - e;
        if (e < 0.00001f) ok = ok + 250;
    }
    return ok;
}
