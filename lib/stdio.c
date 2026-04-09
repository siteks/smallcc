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

static void _print_ulong(unsigned long n)
{
    if (n > 9) _print_ulong(n / 10);
    putchar('0' + (int)(n % 10));
}

static void _print_hex(unsigned long n, int width)
{
    char buf[8];
    int len = 0;
    if (n == 0) { buf[len++] = '0'; }
    else {
        while (n != 0) {
            int d = (int)(n & 0xf);
            buf[len++] = d < 10 ? '0' + d : 'a' + d - 10;
            n >>= 4;
        }
    }
    int i;
    for (i = len; i < width; i++) putchar('0');
    /* Print digits stored in reverse — avoid i>=0 with negative i */
    while (len > 0) { len--; putchar(buf[len]); }
}

static void _print_long_dec(long n)
{
    if (n >= 10) _print_long_dec(n / 10);
    putchar('0' + (int)(n % 10));
}

static int _count_long_digits(long n)
{
    int d = 1;
    while (n >= 10) { d++; n /= 10; }
    return d;
}

static void _print_float_wp(double f, int width, int prec)
{
    int neg = 0;
    if (f < 0.0) { neg = 1; f = -f; }
    long ipart = (long)f;
    double fpart = f - (double)ipart;
    /* Compute total character count for padding */
    int total = (neg ? 1 : 0) + _count_long_digits(ipart) + 1 + prec;
    int i;
    for (i = total; i < width; i++) putchar(' ');
    if (neg) putchar('-');
    _print_long_dec(ipart);
    putchar('.');
    for (i = 0; i < prec; i++) {
        fpart *= 10.0;
        int d = (int)fpart;
        putchar('0' + d);
        fpart -= (double)d;
    }
}

static void _print_float(double f)
{
    _print_float_wp(f, 0, 6);
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
        fmt++;  /* skip '%' */

        /* Optional zero-pad flag and width */
        int width = 0;
        if (*fmt == '0') fmt++;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        /* Optional precision */
        int prec = 6;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }

        /* Optional length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt)
        {
            case 100:   /* 'd' */
            {
                if (is_long) {
                    long n = va_arg(ap, long);
                    if (n < 0) { putchar('-'); _print_long_dec(-n); }
                    else _print_long_dec(n);
                } else {
                    int n = va_arg(ap, int);
                    _print_int(n);
                }
                break;
            }
            case 117:   /* 'u' */
            {
                if (is_long) {
                    unsigned long n = va_arg(ap, unsigned long);
                    _print_ulong(n);
                } else {
                    /* unsigned int is same slot size as int on this target */
                    unsigned int n = (unsigned int)va_arg(ap, int);
                    _print_ulong((unsigned long)n);
                }
                break;
            }
            case 120:   /* 'x' */
            case 88:    /* 'X' */
            {
                unsigned long n;
                if (is_long) n = va_arg(ap, unsigned long);
                else n = (unsigned long)(unsigned int)va_arg(ap, int);
                _print_hex(n, width);
                break;
            }
            case 115:   /* 's' */
            {
                char *s = va_arg(ap, char *);
                _print_str(s);
                break;
            }
            case 99:    /* 'c' */
            {
                int c = va_arg(ap, int);
                putchar(c);
                count++;
                break;
            }
            case 102:   /* 'f' */
            {
                double v = va_arg(ap, double);
                _print_float_wp(v, width, prec);
                break;
            }
            case 37:    /* '%' */
                putchar('%');
                count++;
                break;
        }
        fmt++;
    }
    va_end(ap);
    return count;
}
