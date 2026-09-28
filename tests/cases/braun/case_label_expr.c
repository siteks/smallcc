// EXPECT_R0: 9
int main(void) {
    int x = 3;
    switch (x) {
    case 1 + 2: return 9;
    }
    return 0;
}
