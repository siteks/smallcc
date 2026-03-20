// EXPECT_R0: 42
// short local followed by long: 2-byte padding inserted before the long.
// adj must include the padding so the long isn't below sp.
int main() {
    short a;
    long b;
    a = 10;
    b = 32L;
    return (int)(a + b);
}
