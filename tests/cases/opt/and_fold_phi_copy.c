// EXPECT_R0: 9
// Legalize Pass E (AND-chain fold) followed a copy of a loop-carried
// variable, which after out-of-SSA has one definition per path, and folded
// masks from the wrong one (found by tools/fuzz.py, seed 81315).
unsigned garr[8];
int main(void)
{
    unsigned v0 = 26;
    unsigned v1 = 95;
    unsigned v2 = 70;
    unsigned v3 = 55;
    v2 -= ((31 - (garr[(v1) & 7] == v0)) != (v1 + (v2 | v0)));
    { int i473;
    for (i473 = 0; i473 < 1; i473++)
    {
        v2 = (((v2 | (v1 >> ((152) & 15))) & 268) - (((208 * 193) / (((107 << ((v0) & 15))) | 1u)) != ((244 & v1) >> (((216 % ((garr[(v2) & 7]) | 1u))) & 15))));
        v1 = (garr[(v1) & 7] - (53 * (garr[(89) & 7] > (273 != 1))));
        v1 &= 139;
    }
    }
    v0 = (v3 > garr[(v1) & 7]);
    v3 = (66 / (((((garr[(v2) & 7] | 29) | (v0 - 89)) | garr[(v1) & 7])) | 1u));
    if (218)
    {
        v3 += (~((v2 ^ (~(v1)))));
        v0 = 10;
    }
    v3 = ((v1 / ((((v3 << ((garr[(v1) & 7]) & 15)) ^ (v1 / ((v2) | 1u)))) | 1u)) - ((garr[(v1) & 7] == (v3 % ((193) | 1u))) % (((garr[(garr[(v2) & 7]) & 7] ^ (v1 << ((garr[(v2) & 7]) & 15)))) | 1u)));
    garr[(v1) & 7] = (196 % ((45) | 1u));
    v2 &= v2;
    return (int)(((v0 ^ v1 ^ v2 ^ v3) & 0x7fff));
}
