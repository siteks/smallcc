/* Hardware _putc: writes bytes to the memory-mapped ASCII framebuffer.
 *   FB base : 0xF000
 *   Layout  : 80 columns × 30 rows, 1 ASCII byte per cell (2400 bytes)
 * The cursor is tracked in software. '\n' advances to the start of the next
 * row. Overflow shifts rows up by one and clears the bottom row with spaces. */

static int _fb_cursor;

static void _fb_scroll(void)
{
    unsigned char *fb = (unsigned char *)0xF000;
    int i;
    for (i = 0; i < 80 * 29; i++)
        fb[i] = fb[i + 80];
    for (i = 80 * 29; i < 80 * 30; i++)
        fb[i] = ' ';
}

int _putc(int c)
{
    unsigned char *fb = (unsigned char *)0xF000;
    if (c == '\n') {
        int col = _fb_cursor % 80;
        _fb_cursor = _fb_cursor + (80 - col);
    } else {
        fb[_fb_cursor] = (unsigned char)c;
        _fb_cursor = _fb_cursor + 1;
    }
    if (_fb_cursor >= 80 * 30) {
        _fb_scroll();
        _fb_cursor = 80 * 29;
    }
    return c;
}
