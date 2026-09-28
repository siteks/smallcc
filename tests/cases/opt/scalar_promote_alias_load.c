// EXPECT_R0: 15
int g;
int main(void) {
    int *p = &g, i, s = 0;
    g = 0;
    for (i = 0; i < 5; i++) { *p += 1; s += g; }
    return s;
}
