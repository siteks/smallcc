// EXPECT_R0: 1
// int[70] = 140 bytes; arr[0] at bp-140, outside F2 word range (+-128 bytes).
// Forces risc_backend.c fallback: lea rd, -140; llw rd, [rd+0]
int main(void) {
    int arr[70];
    arr[0] = 1;
    arr[69] = 2;
    return arr[0];
}
