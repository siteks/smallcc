// EXPECT_R0: 4
#define X 3
#undef X
#define X 4
int main() { return X; }
