// EXPECT_R0: 11
// Switch inside loop: loop-header phi has predecessors from pre-header and switch exits
int main(void) {
    int sum = 0;
    int i;
    for (i = 0; i < 5; i++) {
        int add;
        switch (i % 3) {
            case 0: add = 1; break;
            case 1: add = 2; break;
            default: add = 5; break;
        }
        sum = sum + add;
    }
    return sum;
}
