// EXPECT_R0: 2
int one(int x){return 1;} int two(int x){return 2;} int main(){int(*fp)(int);fp=one;fp=two;return fp(0);}
