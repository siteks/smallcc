// H6: andi range boundary — C==-64 (min) and C==63 (max) both round-trip correctly.
// EXPECT_R0: 1
int main(void) {
    int a = -1;
    int lo = a & -64;   /* 0xffffffc0 = -64 */
    int hi = a & 63;    /* 0x0000003f = 63 */
    return (lo == -64) && (hi == 63);
}
