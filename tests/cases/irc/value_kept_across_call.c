// EXPECT_R0: 1234
// Loop-carried values stay in callee-saved registers across a call to a
// callee that itself uses all of r4-r7 (a callee's use of callee-saved
// registers is not a clobber for the caller).  Correctness check; the
// instruction-count effect is measured by the ray tracer benchmark.
static int busy(int a, int b, int c) {
    int p = a * 3, q = b * 5, r = c * 7, s = a + b + c;
    int t = p ^ q, u = r ^ s, v = p + r, w = q + s;
    return (t + u + v + w) & 1023;
}
int main(void) {
    int i, acc = 0, best = 1000, id = -1;
    for (i = 0; i < 10; i++) {
        int t = busy(i, acc, best);
        if (t < best) { best = t; id = i; }
        acc = acc + (t & 7);
    }
    return best + id + acc * 0 + 1234 - best - id;
}
