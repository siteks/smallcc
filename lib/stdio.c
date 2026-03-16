int putchar(int c)
{
    __putchar(c);
    return c;
}

static void _print_str(const char *s)
{
    while (*s)
        putchar(*s++);
}

static void _print_int(int n)
{
    if (n < 0) { putchar('-'); n = -n; }
    if (n > 9) _print_int(n / 10);
    putchar('0' + n % 10);
}

int puts(const char *s)
{
    _print_str(s);
    putchar('\n');
    return 0;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int count = 0;
    while (*fmt)
    {
        if (*fmt != '%')
        {
            putchar(*fmt);
            count++;
            fmt++;
            continue;
        }
        fmt++;
        switch (*fmt)
        {
            case 100:   // 'd'
            {
                int n = va_arg(ap, int);
                _print_int(n);
                break;
            }
            case 115:   // 's'
            {
                char *s = va_arg(ap, char *);
                _print_str(s);
                break;
            }
            case 99:    // 'c'
            {
                int c = va_arg(ap, int);
                putchar(c);
                count++;
                break;
            }
            case 37:    // '%'
                putchar('%');
                count++;
                break;
        }
        fmt++;
    }
    va_end(ap);
    return count;
}
