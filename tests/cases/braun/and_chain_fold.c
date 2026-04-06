// EXPECT_R0: 1
// Tests: (x & 0xff) & 1 folds to x & 1  (AND-chain constant folding, Sub-pass B3)
int main() {
    int x = 0x1ff;           /* bit 0 set, upper bits also set */
    int r = (x & 0xff) & 1; /* should fold to x & 1 = 1 */
    return r;
}
