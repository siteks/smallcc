// EXPECT_R0: 15
int main() { int a=0; int b=0; while(a<5) {a=a+1; b=b+a;} return b; }
