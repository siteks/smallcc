// EXPECT_R0: 5
// Swap creates a true phi copy cycle (a<-b, b<-a) that deconstruction must break with a temp
int main(void) {
    int a = 5;
    int b = 3;
    int i;
    for (i = 0; i < 6; i++) {
        int tmp = a;
        a = b;
        b = tmp;
    }
    return a;
}
