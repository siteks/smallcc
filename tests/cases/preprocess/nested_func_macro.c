// EXPECT_R0: 15
#define MUL(a,b) ((a)*(b))
#define SQ(x) MUL(x,x)
int main() { return SQ(3)+MUL(2,3); }
