// EXPECT_R0: 7
// FILES: lib.c main.c
extern int add(int, int);
int main() { return add(3, 4); }
