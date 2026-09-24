#include <stdarg.h>
#include <_putc.h>

/* Output sink state.
 * _out_buf == NULL → write to _putc (printf, puts)
 * _out_buf != NULL:
 *   _out_size <  0  → write unbounded to _out_buf (sprintf)
 *   _out_size >= 0  → write at most _out_size-1 bytes to _out_buf (snprintf)
 * _out_pos counts bytes that *would* be written, used for the return value. */
static char *_out_buf;
static int   _out_size;
static int   _out_pos;

int putchar(int c)
{
    return _putc(c);
}

static void _emit(int c)
{
    if (_out_buf == 0) {
        _putc(c);
    } else if (_out_size < 0 || _out_pos + 1 < _out_size) {
        _out_buf[_out_pos] = (char)c;
    }
    _out_pos++;
}

static void _print_str(const char *s)
{
    while (*s)
        _emit(*s++);
}

static void _print_ulong(unsigned long n);
static void _print_int(int n)
{
    if (n < 0) {
        _emit('-');
        if (n == -2147483647 - 1) { _print_ulong(2147483648u); return; }   /* -n would overflow */
        n = -n;
    }
    if (n > 9) _print_int(n / 10);
    _emit('0' + n % 10);
}

static void _print_ulong(unsigned long n)
{
    if (n > 9) _print_ulong(n / 10);
    _emit('0' + (int)(n % 10));
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
    for (i = len; i < width; i++) _emit('0');
    while (len > 0) { len--; _emit(buf[len]); }
}

static void _print_long_dec(long n)
{
    if (n >= 10) _print_long_dec(n / 10);
    _emit('0' + (int)(n % 10));
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
    int total = (neg ? 1 : 0) + _count_long_digits(ipart) + 1 + prec;
    int i;
    for (i = total; i < width; i++) _emit(' ');
    if (neg) _emit('-');
    _print_long_dec(ipart);
    _emit('.');
    for (i = 0; i < prec; i++) {
        fpart *= 10.0;
        int d = (int)fpart;
        _emit('0' + d);
        fpart -= (double)d;
    }
}

int puts(const char *s)
{
    _out_buf = 0;
    _print_str(s);
    _emit('\n');
    return 0;
}

static void _vformat(const char *fmt, va_list ap)
{
    while (*fmt)
    {
        if (*fmt != '%')
        {
            _emit(*fmt);
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
                    if (n < 0) { _emit('-'); _print_long_dec(-n); }
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
                _emit(c);
                break;
            }
            case 102:   /* 'f' */
            {
                double v = va_arg(ap, double);
                _print_float_wp(v, width, prec);
                break;
            }
            case 37:    /* '%' */
                _emit('%');
                break;
        }
        fmt++;
    }
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _out_buf = 0;
    _out_pos = 0;
    _vformat(fmt, ap);
    va_end(ap);
    return _out_pos;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _out_buf = buf;
    _out_size = -1;
    _out_pos = 0;
    _vformat(fmt, ap);
    va_end(ap);
    buf[_out_pos] = '\0';
    return _out_pos;
}

int snprintf(char *buf, int size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _out_buf = buf;
    _out_size = size;
    _out_pos = 0;
    _vformat(fmt, ap);
    va_end(ap);
    if (size > 0) {
        int term = _out_pos;
        if (term > size - 1) term = size - 1;
        buf[term] = '\0';
    }
    return _out_pos;
}
