// EXPECT_R0: 92
// Found by tools/fuzz.py (seed 9199): emit-time scratch borrows (e.g. for a
// large constant operand when legalize Pass F is disabled at -O0/-O1) are
// invisible to record_function_clobbers, so a caller kept a live value in
// r2 across a call whose callee borrowed r2. emit_function now feeds its
// borrow mask back via irc_add_clobbers. The fuzzer's -O0 oracle exercises
// the original failing configuration.
unsigned f0(unsigned p0)
{
    p0 = (p0 > (((p0 <= 50) & (~(264))) & p0));
    return ((p0 ^ (266 >= (41 | 257))) ^ (~(p0)));
}
unsigned f1(unsigned p0, unsigned p1, unsigned p2)
{
    return f0((p0 - 28));
}
int main(void)
{
    unsigned v0 = 92;
    unsigned v1 = 69;
    unsigned v2 = 9;
    unsigned v3 = 23;
    v3 ^= (v0 + f1((v3 ^ v1), (v2 % ((v3) | 1u)), (v1 & v0)));
    return (int)(((v0 ^ v1 ^ v2 ^ v3) & 0x7fff));
}
