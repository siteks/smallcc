// EXPECT_R0: 285
// Inlined function called in a loop
int square(int x) { return x * x; }

int main(void) {
    int sum = 0;
    int i;
    for (i = 0; i < 10; i++) {
        sum = sum + square(i);
    }
    return sum;
}
