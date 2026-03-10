// EXPECT_R0: 2
int main() { int x=0; goto done; x=99; done: x=x+2; return x; }
