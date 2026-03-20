// EXPECT_R0: 13
// short then float (float is also 4-byte aligned): same padding hole.
int main() {
    short s;
    float f;
    s = 3;
    f = 10.0;
    return (int)((float)s + f);
}
