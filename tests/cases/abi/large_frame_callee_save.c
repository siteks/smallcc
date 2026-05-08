// EXPECT_R0: 42
//
// Regression: emit.c's prologue/epilogue used F2 `sl r4, imm7` /
// `ll r4, imm7` for callee-save store/restore unconditionally. F2's
// imm7 is 7-bit signed (-64..63, scaled x4 = -256..252 byte offsets);
// when a function's frame pushes the callee-save area past -256
// bytes, the assembler silently masked the negative offset to 7 bits
// and emitted the store/restore at the wrong address. The saved
// register value never made it to the stack and on the way out got
// "restored" from junk memory, after which `ret` typically
// transferred control somewhere bizarre (in the NanoJPEG bring-up
// it landed at PC=0, looking like main was being re-entered).
//
// This forces a >256-byte frame by giving the function ~80 int
// locals that all need to be live at the same time. Constant-
// folding is defeated by reading the seed from a global. The body
// returns 42 regardless of how the math is wired -- the test is
// purely about whether the function can save callee-saved
// registers, run, restore them, and return without crashing.
//
// To verify the fix: build smallcc, compile this with -O2 (default)
// and disassemble big(). The prologue should contain
// `lea r0, -288` + `sll r4, r0, 0` (or similar) instead of
// `sl r4, -72`.

int g = 11;

int big(void) {
    int a0  = g + 1,  a1  = g + 2,  a2  = g + 3,  a3  = g + 4;
    int a4  = g + 5,  a5  = g + 6,  a6  = g + 7,  a7  = g + 8;
    int a8  = g + 9,  a9  = g + 10, a10 = g + 11, a11 = g + 12;
    int a12 = g + 13, a13 = g + 14, a14 = g + 15, a15 = g + 16;
    int a16 = g + 17, a17 = g + 18, a18 = g + 19, a19 = g + 20;
    int a20 = g + 21, a21 = g + 22, a22 = g + 23, a23 = g + 24;
    int a24 = g + 25, a25 = g + 26, a26 = g + 27, a27 = g + 28;
    int a28 = g + 29, a29 = g + 30, a30 = g + 31, a31 = g + 32;
    int a32 = g + 33, a33 = g + 34, a34 = g + 35, a35 = g + 36;
    int a36 = g + 37, a37 = g + 38, a38 = g + 39, a39 = g + 40;
    int a40 = g + 41, a41 = g + 42, a42 = g + 43, a43 = g + 44;
    int a44 = g + 45, a45 = g + 46, a46 = g + 47, a47 = g + 48;
    int a48 = g + 49, a49 = g + 50, a50 = g + 51, a51 = g + 52;
    int a52 = g + 53, a53 = g + 54, a54 = g + 55, a55 = g + 56;
    int a56 = g + 57, a57 = g + 58, a58 = g + 59, a59 = g + 60;
    int a60 = g + 61, a61 = g + 62, a62 = g + 63, a63 = g + 64;
    int a64 = g + 65, a65 = g + 66, a66 = g + 67, a67 = g + 68;
    int a68 = g + 69, a69 = g + 70, a70 = g + 71, a71 = g + 72;
    int s = a0+a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14+a15
          + a16+a17+a18+a19+a20+a21+a22+a23+a24+a25+a26+a27+a28+a29+a30+a31
          + a32+a33+a34+a35+a36+a37+a38+a39+a40+a41+a42+a43+a44+a45+a46+a47
          + a48+a49+a50+a51+a52+a53+a54+a55+a56+a57+a58+a59+a60+a61+a62+a63
          + a64+a65+a66+a67+a68+a69+a70+a71;
    return (s - s) + 42;
}

int main(void) {
    return big();
}
