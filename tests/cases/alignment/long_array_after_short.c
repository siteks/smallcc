// EXPECT_R0: 11
// short followed by long array: array base must be 4-byte aligned in frame.
// Exercises the coremark pattern (short/ptr + long[] in same scope).
int main() {
    short tag;
    long arr[4];
    int i;
    tag = 1;
    for (i = 0; i < 4; i = i + 1) arr[i] = (long)(i + 1);
    return (int)(tag + arr[0] + arr[1] + arr[2] + arr[3]);
}
