// EXPECT_R0: 1597463007
// A union type-pun through a non-escaping local is a register move after
// frame-slot store->load forwarding; the value must still be exact.
// 0x5f3759df - (bits(1.0f) >> 1) = 0x5f3759df - 0x1fc00000 = 0x3f7759df
// then the float bits round-trip back through the union unchanged.
static int seed(float x) {
    union { float f; int i; } u;
    u.f = x;
    u.i = 0x5f3759df - (u.i >> 1);
    return u.i;
}
int main(void) {
    union { float f; int i; } v;
    int a = seed(1.0f);              /* 0x3f7759df */
    v.i = 0x5f3759df;
    v.f = v.f;                       /* store then load of the same slot */
    return (a - 0x3f7759df) + v.i;   /* 0 + 0x5f3759df */
}
