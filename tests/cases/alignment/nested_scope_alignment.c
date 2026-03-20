// EXPECT_R0: 99
// Alignment padding in an inner compound statement scope.
// Inner scope: short then long; outer adj must not count the inner padding twice.
int main() {
    int result;
    result = 0;
    {
        short s;
        long n;
        s = 9;
        n = 90L;
        result = (int)((long)s + n);
    }
    return result;
}
