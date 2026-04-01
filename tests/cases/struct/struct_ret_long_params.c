// EXPECT_R0: 4
// Struct-returning function with long parameters.
// On CPU4 the hidden retbuf pointer occupies a 4-byte slot, so declared
// params are shifted by 4 (not 2) and the long params land at 4-aligned
// offsets.  Before the fix the hidden-ptr slot was 2 bytes, breaking
// alignment of the 4-byte params that follow.
typedef struct { long a; long b; } Pair;
Pair swap(long x, long y) {
    Pair p;
    p.a = y;
    p.b = x;
    return p;
}
int main() {
    Pair p = swap(3L, 7L);
    return (int)(p.a - p.b);   /* 7 - 3 = 4 */
}
