#ifndef STDIO_H
#define STDIO_H

int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...);
char *ecvtbuf(double value, int ndigits, int *decpt, int *sign, char *buf);

#endif
