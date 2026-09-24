// EXPECT_R0: 41
// An address-taken parameter that is modified before its address is passed
// to a callee: the callee must observe the modified value, and the caller
// must not keep using the stale parameter register (the value was forwarded
// from the pre-coloured landing value in an earlier compiler; the fix routes
// forwarding through a working copy).
static void take(int *p, int *q, int base) { *q = *p + base; }
static int f(int n, int *mem, int seed) {
    int out = 0;
    int *cur = mem;
    *cur = seed;
    mem = mem + 1;
    *mem = seed + 1;
    take(mem, &out, n);        /* out = (seed+1) + n */
    return out + *cur;         /* + seed */
}
int main(void) {
    int buf[4];
    return f(10, buf, 15);     /* 16+10+15 = 41 */
}
