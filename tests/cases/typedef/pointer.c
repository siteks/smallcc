// EXPECT_R0: 7
int main() { typedef int *ip; int a=7; ip p=&a; return *p; }
