// EXPECT_R0: 3
int main() { int x=1; goto b; a: return 10; b: x=x+1; goto c; a2: return 20; c: x=x+1; return x; }
