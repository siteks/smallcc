// EXPECT_R0: 2
// XFAIL: issue 0005 (typespec_to_base maps DS_SHORT|DS_INT to int)
int main(void) { return sizeof(short int); }
