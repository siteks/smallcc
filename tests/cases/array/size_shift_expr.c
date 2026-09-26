// EXPECT_R0: 64
// XFAIL: issue 0005 (eval_const_expr has no shift operator and silently yields 0)
int main(void) { int buf[1 << 4]; return sizeof(buf); }
