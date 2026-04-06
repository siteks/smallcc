// EXPECT_R0: 60
// Nested loops with inner break: outer loop phi must not be corrupted
int main(void) {
    int sum = 0;
    int i;
    for (i = 0; i < 5; i++) {
        int j;
        for (j = 0; j < 10; j++) {
            if (j == 1) break;
        }
        sum = sum + 10 + i;
    }
    return sum;
}
