// EXPECT_R0: 1
int x;
int *p = &x;
int main(void) { return p == &x; }
