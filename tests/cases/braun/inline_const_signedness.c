// EXPECT_R0: 50
// Found by tools/fuzz.py (seed 9152): braun selected div/mod/shift/compare
// signedness from the lhs VALUE's vtype instead of the C static type. An
// inlined function argument that folded to a constant carried its caller-
// side i32 vtype into an unsigned division, flipping it to IK_DIV (signed).
// All four oracles agreed with each other and were all wrong vs the host.
unsigned f0(unsigned p0)
{
    p0 &= ((~((p0 / ((73) | 1u)))) > ((223 > 275) + (121 - 83)));
    return p0;
}
int main(void)
{
    unsigned v2 = 51;
    v2 ^= f0((150 - 255));
    return (int)(v2 & 0x7fff);
}
