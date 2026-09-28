// EXPECT_R0: 1
int main(void) {
    int c = 0;
    float f = c ? 1 : 2.5f;
    return f == 2.5f;
}
