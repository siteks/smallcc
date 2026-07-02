// EXPECT_R0: 35
// Found by tools/fuzz.py (seed 40156): IRC's spill loop livelocked -- the
// spill-cost heuristic always picked the previous iteration's own reload
// temp (cheapest use_count), spilling it again forever. After 500 iterations
// it gave up and SILENTLY emitted code with uncolored registers (r0=0).
// Spill temps are now excluded from candidate selection (first pass), and
// exhaustion is a hard compile error instead of silent garbage.
unsigned f0(unsigned p0, unsigned p1, unsigned p2)
{
    p2 |= ((~((p1 % ((p1) | 1u)))) - (~((p1 << ((62) & 15)))));
    p0 |= (((p1 | p2) * (232 << ((252) & 15))) << ((p0) & 15));
    return ((290 - ((61 ^ 193) / ((97) | 1u))) != p0);
}

unsigned f1(unsigned p0, unsigned p1)
{
    p0 ^= f0((161 << ((271) & 15)), (70 + 245), p0);
    p1 |= (((19 % ((p0) | 1u)) & (p0 | 291)) + 196);
    return (((12 >= (p1 * p1)) & p0) >> ((p1) & 15));
}

int main(void)
{
    unsigned v0 = 89;
    unsigned v1 = 78;
    unsigned v2 = 17;
    unsigned v3 = 48;
    unsigned v4 = 43;
    v2 ^= (((v3 % ((v4) | 1u)) + (v1 % ((155) | 1u))) | 142);
    v4 |= (((v0 - v1) / (((v4 / ((v0) | 1u))) | 1u)) <= (76 <= (178 >> ((165) & 15))));
    v4 &= v4;
    { int i529;
    for (i529 = 0; i529 < 10; i529++)
    {
        if ((v3 >> (((v4 >> ((160) & 15))) & 15)))
        {
            v1 -= v2;
            v2 ^= (182 | (v3 >> ((208) & 15)));
            v2 = ((((v0 <= v2) | 8) << (((v0 | 11)) & 15)) ^ 28);
        }
    }
    }
    { int i722;
    for (i722 = 0; i722 < 10; i722++)
    {
        v1 = ((~(v0)) | (((92 - v4) > (v4 == 44)) >= 21));
        if ((~((v4 >> ((107) & 15)))))
        {
            v4 -= v4;
        }
        { int i684;
        for (i684 = 0; i684 < 4; i684++)
        {
            v0 = (204 | ((~((v1 < v2))) | ((v4 < 233) - (80 - 156))));
            v3 = 67;
        }
        }
    }
    }
    v0 = (((f0(f1((181 ^ 214), v4), 104, (170 | v4)) + v4) >= ((v4 & v1) | (172 & v3))) << (((~((f1((250 & v2), (v0 + v2)) & 117)))) & 15));
    v3 = (237 ^ ((~((69 + 31))) >= (v2 == v1)));
    return (int)(((v0 ^ v1 ^ v2 ^ v3 ^ v4) & 0x7fff));
}
