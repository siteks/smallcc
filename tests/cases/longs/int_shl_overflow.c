// EXPECT_R0: 65536
// ILP32: int is 4 bytes, so 1<<16 is 0x10000 (no overflow). On the previous
// LP32 layout int was 2 bytes and the shift overflowed to 0.
long main() { int a=1; return a<<16; }
