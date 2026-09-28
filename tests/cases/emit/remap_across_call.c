// EXPECT_R0: 50
// Emission's register remap (remap_single_use_values) moved a value live
// across a call into r5, treating only r0-r3 as clobbered by calls; on CPU5
// r4-r7 are caller-saved too (found by tools/fuzz.py -arch cpu5, seed 84053).
unsigned garr[8];
unsigned f0(unsigned p0, unsigned p1)
{
    {
        {
        }
    }
    return (garr[(p1) & 7] | (((252 >> ((86) & 15)) < p0) & (~((p0 * p0)))));
}

unsigned f1(unsigned p0)
{
    {
    }
    {
        if ((p0 & (187 - p0)))
        {
            p0 -= ((p0 != (291 / ((p0) | 1u))) | ((84 & garr[(p0) & 7]) * f0((280 >> ((p0) & 15)), (p0 >> ((65) & 15)))));
        }
    }
    return p0;
}

int main(void)
{
    unsigned v0 = 47;
    unsigned v1 = 36;
    unsigned v2 = 57;
    {
    }
    {
    }
    { int i37;
    {
        if (((v0 > garr[(v2) & 7]) & (9 * v1)))
        {
            v2 |= (~(((v1 >> ((159) & 15)) >> ((228) & 15))));
        }
    }
    }
    v1 ^= (((v2 ^ 145) | 129) == f1(garr[(298) & 7]));
    v2 -= (((v1 != 43) - f0(v2, v1)) >> ((v2) & 15));
    return (int)(((v0 ^ v1 ^ v2) & 0x7fff));
}
