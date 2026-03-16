/* ecvtbuf: convert double to ndigits significant decimal digits.
   Returns buf. *decpt = decimal-point position (digits from left),
   *sign = 1 if negative. Used by ee_printf for %e/%f formatting. */
char *ecvtbuf(double value, int ndigits, int *decpt, int *sign, char *buf)
{
    int i, d;
    if (value < 0.0) { *sign = 1; value = -value; } else { *sign = 0; }
    if (value == 0.0) {
        *decpt = 0;
        for (i = 0; i < ndigits; i++) buf[i] = '0';
        buf[ndigits] = '\0';
        return buf;
    }
    /* Normalise to [1.0, 10.0) and count the decimal exponent */
    *decpt = 1;
    while (value >= 10.0) { value /= 10.0; (*decpt)++; }
    while (value <   1.0) { value *= 10.0; (*decpt)--; }
    /* Extract ndigits significant decimal digits */
    for (i = 0; i < ndigits; i++) {
        d = (int)value;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        buf[i] = '0' + d;
        value = (value - (double)d) * 10.0;
    }
    buf[ndigits] = '\0';
    return buf;
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
