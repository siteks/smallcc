// EXPECT_R0: 1
// XFAIL: issue 0005 (unsigned short int is treated as unsigned int)
int main(void) { return (unsigned short int)-1 == 65535; }
