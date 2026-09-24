# 0002 — -O0 and -O2 disagree on a fuzz program (pre-existing)

Status: OPEN (found 2026-09-24 by `tools/fuzz.py -n 300 -seed 20260924`, seed 20261101).
Reproduced on the compiler before the September 2026 legalize/inliner work (commit 479f7da),
so it is not caused by those changes. Three oracles agree (`-O2`→sim_c, `-runoos`, `-runirc`
all give 0x23); `-O0`→sim_c gives 0x6b. Either the unoptimised path miscompiles this
program or every optimising path shares a bug that -O0 avoids. Bisect with `-Omask=`.

```c
unsigned garr[8];
unsigned f0(unsigned p0)
{
    p0 ^= (((0 & p0) != p0) - ((~(p0)) > p0));
    return ((((garr[(p0) & 7] != p0) - (garr[(p0) & 7] == 76)) % (((garr[(269) & 7] % ((147) | 1u))) | 1u)) <= (p0 / ((((p0 & p0) + (278 ^ p0))) | 1u)));
}

int main(void)
{
    unsigned v0 = 66;
    unsigned v1 = 86;
    unsigned v2 = 10;
    v2 &= v2;
    v1 ^= 92;
    { int i3;
    for (i3 = 0; i3 < 7; i3++)
    {
        if ((v1 ^ (17 * 162)))
        {
            garr[(((235 - 263) & 14)) & 7] = v0;
            garr[((272 - (142 % ((v0) | 1u)))) & 7] = v2;
            v1 -= v0;
        }
        v2 = v2;
    }
    }
    v2 &= ((v0 ^ (v1 >= garr[(v1) & 7])) != ((226 ^ v2) % (((8 * v1)) | 1u)));
    v1 ^= v2;
    v2 += (v2 - (f0((garr[(v2) & 7] != v0)) < (v2 & 190)));
    v1 = (((176 <= v0) ^ f0((v1 > v0))) - (((5 * 41) != (v2 >> ((137) & 15))) | (f0((v1 ^ garr[(v1) & 7])) - (~(garr[(v1) & 7])))));
    v2 -= 35;
    garr[(((garr[(v2) & 7] + garr[(v1) & 7]) / ((v2) | 1u))) & 7] = (63 == (94 + (21 ^ v2)));
    return (int)(((v0 ^ v1 ^ v2) & 0x7fff));
}
```
