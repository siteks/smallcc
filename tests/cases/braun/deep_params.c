// EXPECT_R0: 36
// 8 parameters all promoted to SSA; tests MAX_PARAM_SEEDS seeding
int sum8(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}
int main(void) {
    return sum8(1, 2, 3, 4, 5, 6, 7, 8);
}
