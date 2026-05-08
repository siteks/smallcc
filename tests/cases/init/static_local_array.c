// EXPECT_R0: 239
// `static const T arr[] = { ... }` declared inside a function used to drop
// the initializer entirely — emit_static_local_data only handled the
// string-literal-for-char-array and scalar cases, falling through to
// `allocb` for brace-list array initializers. NanoJPEG hit this on its
// internal lookup tables and silently saw all zeros.
int main(void) {
    static const unsigned char tbl[] = { 1, 0x22, 0xCC };
    return tbl[0] + tbl[1] + tbl[2];   /* 1 + 34 + 204 = 239 */
}
