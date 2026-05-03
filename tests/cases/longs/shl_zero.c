// EXPECT_R0: 305419896
// ILP32: int return type is 4 bytes, so the full 0x12345678 = 305419896 is
// preserved. On LP32 this truncated to 0x5678 = 22136.
int main() { long a = 0x12345678; return a<<0; }
