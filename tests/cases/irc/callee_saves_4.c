// EXPECT_R0: 100
// 4 vregs live across a call; call-site interference forces all of r4-r7 as callee-saves
// insert_callee_saves expands the frame by 16 bytes and adds prologue/epilogue saves
int nop(int x) { return x; }
int main(void) {
    int a = 10, b = 20, c = 30, d = 40;
    nop(0);
    return a + b + c + d;
}
