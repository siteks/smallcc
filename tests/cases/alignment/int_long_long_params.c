// EXPECT_R0: 100
// int followed by two longs: both longs must be at 4-byte-aligned offsets.
// Exercises the two-pass flush_for_call_n layout with consecutive 4-byte args.
long sum3l(int a, long b, long c) {
    return (long)a + b + c;
}
int main() {
    return (int)sum3l(10, 40L, 50L);
}
