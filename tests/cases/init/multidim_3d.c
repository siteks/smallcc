// EXPECT_R0: 3
int main() { int a[2][2][2] = {1,5,{2,3}}; return a[1][0][1]; }
