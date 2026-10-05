// EXPECT_R0: 0
// TIMEOUT: 4000000
// A sparse switch on (x & 0x7f), the RV32 opcode dispatch shape: on CPU5 a
// 128-entry table with no range check (the selector's bits bound it); gaps
// go to default. Every selector 0..299 is checked against an if-chain.
int disp(unsigned x) {
    switch (x & 0x7f) {
    case 0x37: return 1;
    case 0x17: return 2;
    case 0x6f: return 3;
    case 0x67: return 4;
    case 0x63: return 5;
    case 0x03: return 6;
    case 0x23: return 7;
    case 0x13:
    case 0x33: return 8;
    case 0x0f: return 9;
    case 0x73: return 10;
    case 0x2f: return 11;
    default:   return 12;
    }
}

int ref(unsigned x) {
    unsigned k = x & 0x7f;
    if (k == 0x37) return 1;
    if (k == 0x17) return 2;
    if (k == 0x6f) return 3;
    if (k == 0x67) return 4;
    if (k == 0x63) return 5;
    if (k == 0x03) return 6;
    if (k == 0x23) return 7;
    if (k == 0x13 || k == 0x33) return 8;
    if (k == 0x0f) return 9;
    if (k == 0x73) return 10;
    if (k == 0x2f) return 11;
    return 12;
}

int main(void) {
    unsigned x;
    for (x = 0; x < 300; x++)
        if (disp(x) != ref(x)) return (int)x + 1;
    if (disp(0xffffff93u) != 8) return 1000;
    return 0;
}
