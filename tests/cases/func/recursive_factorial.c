// EXPECT_R0: 24
int f(int a) { if(a<3) return a; return a*f(a-1); } int main() { return f(4); }
