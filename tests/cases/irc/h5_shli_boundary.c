// H5: shli boundary — shifts by 0, 1, 30, 31 all produce correct results.
// EXPECT_R0: 1
int main(void) {
    unsigned long x = 1UL;
    return (x << 0)  == 1UL
        && (x << 1)  == 2UL
        && (x << 30) == 0x40000000UL
        && (x << 31) == 0x80000000UL;
}
