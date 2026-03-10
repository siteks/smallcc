// EXPECT_R0: 6
#ifdef NOTDEFINED
#define B
#endif
#ifndef B
int main() { return 6; }
#endif
