// EXPECT_R0: 1
#define DEBUG
#ifdef DEBUG
int main() { return 1; }
#else
int main() { return 2; }
#endif
