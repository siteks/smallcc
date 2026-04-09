// EXPECT_R0: 0
// EXPECT_STDOUT: Hello world  1.230 deadbeef beef 1025 x\n
#include <stdio.h>
int main()
{
    float a = 1.23;
    unsigned long b = 0xdeadbeeful;
    int c = 1025;
    char d = 'x';
    printf("Hello world %6.3f %lx %x %d %c\n", a, b, (unsigned int)(b&0xffff), c, d);
    return 0;
}
