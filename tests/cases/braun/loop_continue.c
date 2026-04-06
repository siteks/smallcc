// EXPECT_R0: 25
// continue creates a critical back-edge to the loop header phi
int main(void) {
    int sum = 0;
    int i;
    for (i = 0; i < 10; i++) {
        if (i % 2 == 0) continue;
        sum = sum + i;
    }
    return sum;
}
