// EXPECT_R0: 11
int main() { int a=0; int b=0; {a=a+1; b=b+a+10;} return b; }
