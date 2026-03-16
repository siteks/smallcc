/* modf: split x into integer and fractional parts (same sign as x).
   On this target double == float == 32-bit IEEE 754; long is 32-bit,
   so the cast is exact for values in [-2^31, 2^31). */
double modf(double x, double *iptr)
{
    double i = (double)(long)x;
    *iptr = i;
    return x - i;
}
