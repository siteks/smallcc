int strlen(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *p = dst;
    while (*src) { *p++ = *src++; }
    *p = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *p = dst;
    while (*p) p++;
    while (*src) { *p++ = *src++; }
    *p = 0;
    return dst;
}
