// EXPECT_R0: 32717
// Found by tools/fuzz.py (seed 5031): remap_single_use_values chained its
// single-user scan through a dead coalesced IK_COPY without checking the
// copy dst's own use_count. Here ~(...) is copied (type coercion, coalesced
// dead), used once by the unsigned compare and AGAIN in the return chain;
// the remap moved it onto the compare's destination register, so the second
// use read the comparison result instead of the value.
int main(void) {
    unsigned v0 = 25;
    unsigned v1 = 26;
    unsigned v2 = 4;
    unsigned v3 = 32;
    unsigned v4 = 87;
    unsigned v5 = 93;
    v1 = (~(((1 ^ (176 != v5)) ^ ((9 - v5) > v4))));
    v2 ^= (((v2 > v1) ^ (222 / ((294) | 1u))) | v2);
    return (int)(((v0 ^ v1 ^ v2 ^ v3 ^ v4 ^ v5) & 0x7fff));
}
