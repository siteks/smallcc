// EXPECT_R0: 77
// pointer local (2 bytes) followed by long (4 bytes): padding before long.
// Verifies adj accounts for the alignment hole.
int main() {
    int x;
    int *p;
    long n;
    x = 33;
    p = &x;
    n = 44L;
    return (int)(*p + n);
}
