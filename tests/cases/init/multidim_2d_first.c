// EXPECT_R0: 1
int main() { int a[3][3] = {1,{2,3},4}; return a[0][0]; }
