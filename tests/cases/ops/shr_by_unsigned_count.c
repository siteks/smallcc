// EXPECT_R0: 1
int main(void) { int x = -8; return (x >> 1u) == -4 && (-8 >> 1u) == -4; }
