// EXPECT_R0: 305419896
// ILP32: int and long are both 4 bytes, so the conversion is identity (no
// truncation). On LP32 the long was narrowed to 16-bit int (0x5678 = 22136).
int main() { long a = 0x12345678; return a; }
