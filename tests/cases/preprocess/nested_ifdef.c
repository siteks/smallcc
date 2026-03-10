// EXPECT_R0: 5
#define A
#define B
#ifdef A
#ifdef B
int main() { return 5; }
#endif
#endif
