// EXPECT_R0: 14
int main() { int a[3]={1,2,3}; int i=1; a[i++]+=10; return a[1]+i; }
