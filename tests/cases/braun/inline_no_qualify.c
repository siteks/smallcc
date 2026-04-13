// EXPECT_R0: 12
// Function with locals does NOT inline — still produces correct results via call
int foo(int x) {
    int y = x + 1;
    return y * 2;
}

int main(void) {
    return foo(5);
}
