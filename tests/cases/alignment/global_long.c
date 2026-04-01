// EXPECT_R0: 42
// Global long variables must be 4-byte aligned for CPU4's 32-bit load.
// The sim_c CPU4 assembler's 'align' directive now aligns to 4 bytes,
// matching the cpu4/assembler.py behaviour.
volatile long g1 = 12;
volatile long g2 = 30;
int main() {
    return (int)(g1 + g2);
}
