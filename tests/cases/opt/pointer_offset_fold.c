// EXPECT_R0: 73
// TIMEOUT: 4000000
// Loads/stores through p + k fold the constant into the access offset when
// it fits the register-relative encoding, including negative offsets and
// offsets at the edge of the imm10*size range; larger ones stay as adds.
static int arr[1100];
int main(void) {
    int *p = arr + 500;
    int *q;
    int s;
    p[-500] = 1; p[-1] = 2; p[0] = 3; p[1] = 4;
    p[511] = 5; p[512] = 6; p[599] = 7;
    q = p + 512;
    q[-512] = q[-512] + 10;      /* arr[500] = 13 */
    s = p[-500] + p[-1] + p[0] + p[1] + p[511] + p[512] + p[599] + q[-512];
    return s + arr[500] + arr[1099] + 2;   /* 1+2+13+4+5+6+7+13 = 51; +13 +7 +2 = 73 */
}
