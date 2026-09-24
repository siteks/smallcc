// EXPECT_R0: 7
// Float constant expressions fold at compile time (no fdiv at run time),
// including unary minus on a literal and folded comparisons.
static int cmp(float a, float b) { return a < b; }
int main(void) {
    float inv = 1.0f / 240.0f;
    float k = -1.0f;
    float m = 2.5f * 4.0f - 3.0f;   /* 7.0f */
    int r = 0;
    if (inv > 0.004f && inv < 0.005f) r += 1;
    if (k < 0.0f) r += 2;
    if (1.0f / 240.0f == inv) r += 4;
    if (cmp(k, m) && (int)m == 7) r += 0;
    return r;
}
