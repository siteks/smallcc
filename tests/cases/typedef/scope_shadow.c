// EXPECT_R0: 5
int main() { typedef int T; {typedef float T; T x=5.0; return (int)x;} return 0; }
