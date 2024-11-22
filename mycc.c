#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: num\n");
        return 1;
    }
    printf("main:\n");
    printf("    ldi     %d\n", atoi(argv[1]));
    printf("    halt\n");
    return 0;
}
