
## Compare branch ops in Coremark
```
      11200 cycles  r0=1    bne r4, r0
      10560 cycles  r0=44   beq r6, r0
       7280 cycles  r0=2    beq r4, r0
       6416 cycles  r0=4    beq r4, r0
       3792 cycles  r0=5    beq r4, r0
       2960 cycles  r0=101  beq r6, r0
       2624 cycles  r0=46   beq r6, r0
       1232 cycles  r0=43   beq r6, r0
       1200 cycles  r0=0    blts r0, r7 signed pos non-zero
        848 cycles  r0=45   beq r6, r0
        832 cycles  r0=3    beq r4, r0
        628 cycles  r0=0    beq r3, r0  non-zero
        448 cycles  r0=6    beq r4, r0
        408 cycles  r0=0    beq r4, r0  non-zero
        368 cycles  r0=46   beq r6, r0
        288 cycles  r0=45   beq r6, r0
        240 cycles  r0=0    blts r0, r7 signed pos non-zero
        192 cycles  r0=46   beq r6, r0
        178 cycles  r0=0    blt r0, r2  non-zero
        176 cycles  r5=0    blt r5, r2  non-zero
        156 cycles  r0=1    beq r5, r0
        144 cycles  r6=0    blt r6, r4  non-zero
        132 cycles  r0=2    beq r5, r0
        112 cycles  r0=7    beq r4, r0
        110 cycles  r0=3    beq r5, r0
         88 cycles  r0=4    beq r5, r0
         72 cycles  r6=0    blt r6, r4  non-zero
         72 cycles  r7=0    blt r7, r4  non-zero
         66 cycles  r0=5    beq r5, r0
         64 cycles  r0=8    blt r2, r0
         64 cycles  r0=8    blt r4, r0
         48 cycles  r0=1    beq r1, r0
         44 cycles  r0=6    beq r5, r0
         42 cycles  r0=0    bne r2, r0  non-zero
         40 cycles  r0=10   blts r2, r0
         40 cycles  r0=0    blts r0, r5
         36 cycles  r0=9    blt r0, r4
         32 cycles  r7=100  beq r2, r7
         28 cycles  r7=117  beq r2, r7
         22 cycles  r0=7    beq r5, r0
         18 cycles  r5=0    blt r5, r4  non-zero
         14 cycles  r7=120  beq r2, r7
         10 cycles  r1=0    bne r6, r1  non-zero
         10 cycles  r0=0    blts r0, r5 signed pos non-zero
          8 cycles  r0=8    blt r2, r0
          8 cycles  r0=8    blt r4, r0
          8 cycles  r0=0    beq r2, r0  non-zero
          8 cycles  r0=0    beq r2, r0  non-zero
          8 cycles  r0=34   blts r2, r0
          6 cycles  r0=10   bles r0, r4
          4 cycles  r0=10   bles r0, r1
          4 cycles  r7=88   beq r2, r7
          4 cycles  r1=115  beq r2, r1
          4 cycles  r1=99   beq r2, r1
          4 cycles  r1=102  beq r2, r1
          2 cycles  r0=0    blts r4, r0 signed negative
          2 cycles  r0=9    blts r0, r4
          2 cycles  r0=0    blts r4, r0 signed negative
          2 cycles  r0=59893  beq r4, r0
          2 cycles  r0=8151 beq r2, r0
```

About 54k cycles, gain of 26k with three instructions, compare eq/ne imm7, not sign extended, branch imm7, and a new branch if positive in format 3d
