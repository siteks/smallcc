// EXPECT_R0: 99
// long[70] = 280 bytes; arr[0] at bp-280, outside F2 long range (+-256 bytes).
// Forces risc_backend.c fallback: lea rd, -280; lll rd, [rd+0]
// Store fallback must save/restore r7 to use it as address scratch register.
int main(void) {
    long arr[70];
    arr[0] = 99L;
    arr[69] = 42L;
    return (int)arr[0];
}
