// EXPECT_R0: 13059
// Found by tools/fuzz.py (seed 15): sim_c executed rdivli/rmodli (the
// reverse unsigned div/mod F0b forms) with SIGNED semantics, contradicting
// docs/isa/cpu4.md and irsim. P19 emits rdivli for UDIV(const, x) when the
// const fits sext9 — here ~239 = -240 as a bit pattern, but the division
// must treat it as the unsigned dividend 4294967056.
unsigned g(unsigned x) { return ~x; }
int main(void) {
    unsigned d = 5;
    return (int)((g(239) / d) & 0x7fff);   /* 4294967056/5 = 0x33333303 */
}
