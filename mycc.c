#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: num\n");
        return 1;
    }
    char *p = argv[1];
    printf("main:\n");
    printf("    ldi     %d\n", (int)strtol(p, &p, 10));
    while (*p)
    {
        if (*p == '+')
        {
            p++;
            printf("    ldir    $1 %d\n", (int)strtol(p, &p, 10));
            printf("    add     $1\n");
            continue;
        }
        if (*p == '-')
        {
            p++;
            printf("    ldir    $1 %d\n", (int)strtol(p, &p, 10));
            printf("    sub     $1\n");
            continue;
        }
        fprintf(stderr, "Bad input %c\n", *p);
        return 1;
    }
    printf("    halt\n");
    return 0;
}
