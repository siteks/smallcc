// EXPECT_R0: 60
// int + long + int parameters: the long must land at a 4-byte-aligned offset.
// flush_for_call_n must account for the internal padding gap between slots,
// not merely round the total size up.  Before the fix the long was stored
// two bytes too low and the callee read garbage.
long sum3(int a, long b, int c) {
    return (long)a + b + (long)c;
}
int main() {
    return (int)sum3(10, 20L, 30);
}
