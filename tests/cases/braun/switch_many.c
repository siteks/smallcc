// EXPECT_R0: 204
// Many-predecessor phi: 9-case switch stresses sealBlock and readVariableRecursive
int main(void) {
    int sum = 0;
    int i;
    for (i = 0; i < 9; i++) {
        int v;
        switch (i) {
            case 0: v = 0; break;
            case 1: v = 1; break;
            case 2: v = 4; break;
            case 3: v = 9; break;
            case 4: v = 16; break;
            case 5: v = 25; break;
            case 6: v = 36; break;
            case 7: v = 49; break;
            default: v = 64; break;
        }
        sum = sum + v;
    }
    return sum;
}
