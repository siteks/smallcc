// EXPECT_R0: 0
// TIMEOUT: 4000000
// More loop-carried values than registers, rotated every iteration: the
// out-of-SSA copies between spilled values become slot-to-slot copies,
// which spill-slot coalescing (alloc.c) turns into shared slots. The
// fourth parameter arrives on the stack and is itself a candidate.
static int rot(int n, int a0, int b0, int k) {
    int a = a0, b = b0, c = 3, d = 4, e = 5, g = 6, h = 7, i2 = 8, j = 9, l = 10,
        m = 11, o = 12, p = 13, q = 14, r = 15, s = 16, t = 17, u = 18, it;
    for (it = 0; it < n; it++) {
        int tmp = a;
        a = b; b = c; c = d; d = e; e = g; g = h; h = i2; i2 = j; j = l; l = m;
        m = o; o = p; p = q; q = r; r = s; s = t; t = u; u = tmp + it * k;
    }
    return a + 2*b + 3*c + 4*d + 5*e + 6*g + 7*h + 8*i2 + 9*j + 10*l + 11*m
         + 12*o + 13*p + 14*q + 15*r + 16*s + 17*t + 18*u;
}
int main() {
    int n;
    for (n = 0; n < 40; n += 13) {
        // reference: the same rotation on an array
        int v[18], it, k2, sum = 0;
        for (k2 = 0; k2 < 18; k2++) v[k2] = k2 + 1;
        v[0] = 100; v[1] = 200;
        for (it = 0; it < n; it++) {
            int tmp = v[0];
            for (k2 = 0; k2 < 17; k2++) v[k2] = v[k2 + 1];
            v[17] = tmp + it * 7;
        }
        for (k2 = 0; k2 < 18; k2++) sum += (k2 + 1) * v[k2];
        if (rot(n, 100, 200, 7) != sum) return 1 + n;
    }
    return 0;
}
