// H2: E5 peephole kills immw 0xff without checking single-use.
// If the constant register is shared, killing the immw breaks the second use.
// EXPECT_R0: 1
int main(void) {
    unsigned int a = 0x1234;
    unsigned int b = 0x5678;
    unsigned int ax = a & 0xff;   /* = 0x34 */
    unsigned int bx = b & 0xff;   /* = 0x78 */
    return (ax == 0x34) && (bx == 0x78);
}
