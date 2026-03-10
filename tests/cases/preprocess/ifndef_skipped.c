// EXPECT_R0: 3
#define GUARD
#ifndef GUARD
int main() { return 1; }
#endif
int main() { return 3; }
