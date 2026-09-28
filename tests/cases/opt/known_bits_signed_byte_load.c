// EXPECT_R0: 255
char g = -1;
int main(void) { char *p = &g; return *p & 0xff; }
