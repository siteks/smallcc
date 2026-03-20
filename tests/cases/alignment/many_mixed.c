// EXPECT_R0: 55
// Multiple alternating 2-byte/4-byte locals to stress alignment padding.
// Each long after a short creates a 2-byte hole; adj must cover all holes.
int main() {
    short a;
    long  w;
    short b;
    long  x;
    short c;
    long  y;
    a = 1; w = 10L;
    b = 2; x = 12L;
    c = 3; y = 27L;
    return (int)(a + w + b + x + c + y);
}
